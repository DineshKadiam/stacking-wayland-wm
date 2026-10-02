/*
 * Copyright (c) 2026 Thomas Adam
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */
#include "cow/view.h"
#include "container_internal.h"
#include "cow/stack.h"
#include "cow/decoration.h"
#include "cow/memory.h"
#include "cow/output.h"
#include "cow/update.h"
#include "cow/wm.h"
#include "cow/titlebar.h"

#include <limits.h>
#include <string.h>

#include "river-window-management-v1-client-protocol.h"

static int next_container_id;
static int next_pane_id;
static int  pane_index(const struct cow_container_layout *,
     const struct cow_container_layout *, int *);
static bool pane_can_expand(const struct cow_container_layout *,
    const struct cow_container_layout *);
static bool pane_title_visible(const struct cow_container *,
    const struct cow_container_pane *);
static struct cow_container_layout *pane_column(struct cow_container_layout *);

static struct cow_container_pane *
zoomed_pane(const struct cow_container *c)
{
	struct cow_container_pane *pane = NULL;
	while ((pane = cow_container_pane_next(c, pane)) != NULL) {
		if (pane->zoom != COW_CONTAINER_ZOOM_NONE)
			return pane;
	}
	return NULL;
}

static bool
zoom_hides_pane(struct cow_container_pane *zoom,
    const struct cow_container_pane	  *pane)
{
	if (!zoom || zoom == pane)
		return false;
	if (zoom->zoom == COW_CONTAINER_ZOOM_ALL)
		return true;
	const struct cow_container_layout *column = &pane->layout;
	while (column->parent && column->parent->axis == COW_CONTAINER_VERTICAL)
		column = column->parent;
	return pane_column(&zoom->layout) == column;
}

static const struct cow_container_layout *
pane_display_layout(const struct cow_container *c,
    const struct cow_container_pane	       *pane)
{
	if (!c->shaded && pane->zoom == COW_CONTAINER_ZOOM_ALL)
		return &c->root->layout;
	if (!c->shaded && pane->zoom == COW_CONTAINER_ZOOM_COLUMN) {
		const struct cow_container_layout *column = &pane->layout;
		while (column->parent &&
		    column->parent->axis == COW_CONTAINER_VERTICAL)
			column = column->parent;
		return column;
	}
	return &pane->layout;
}

static void
clear_zoom(struct cow_container *c)
{
	struct cow_container_pane *pane = NULL;
	while ((pane = cow_container_pane_next(c, pane)) != NULL)
		pane->zoom = COW_CONTAINER_ZOOM_NONE;
}

void
container_pane_normalize_columns(struct cow_container_layout *node)
{
	if (node->axis == COW_CONTAINER_HORIZONTAL) {
		container_pane_normalize_columns(node->child[0]);
		container_pane_normalize_columns(node->child[1]);
	} else if (!pane_can_expand(node, NULL)) {
		while (node->axis != COW_CONTAINER_LEAF)
			node = node->child[0];
		node->collapsed = COW_CONTAINER_LEAF;
	}
}

void
cow_container_sync_flags(struct cow_view *view, uint32_t mask)
{
	struct cow_container *c = cow_container_top_group(view);
	if (c == NULL)
		return;
	c->flags = (c->flags & ~mask) | (view->flags & mask);
	struct cow_view *member = NULL;
	while ((member = cow_container_next_view(c, member)) != NULL) {
		member->flags = (member->flags & ~mask) | (view->flags & mask);
		member->dec.dirty = true;
	}
}

struct cow_container_pane *
container_pane_from_layout(struct cow_container_layout *layout)
{
	struct cow_container_pane *pane;

	if (layout == NULL)
		return NULL;
	return wl_container_of(layout, pane, layout);
}

struct cow_container_pane *
container_pane_create(void)
{
	struct cow_container_pane *pane = xcalloc(1, sizeof(*pane));
	pane->id			= ++next_pane_id;

	wl_list_init(&pane->tabs);
	return pane;
}

void
container_pane_destroy(struct cow_container_pane *pane)
{
	if (pane == NULL)
		return;
	if (pane->layout.axis != COW_CONTAINER_LEAF) {
		container_pane_destroy(container_pane_from_layout(pane->layout.child[0]));
		container_pane_destroy(container_pane_from_layout(pane->layout.child[1]));
	}
	xfree(pane->name);
	cow_container_placeholder_destroy(pane);
	if (cow_wm.drag_drop_pane == pane) {
		cow_wm.drag_drop_pane = NULL;
	}
	xfree(pane);
}

static void
pane_replace(struct cow_container *c, struct cow_container_layout *old,
    struct cow_container_layout *replacement)
{
	struct cow_container_layout *parent = old->parent;

	replacement->parent = parent;
	if (parent == NULL)
		c->root = container_pane_from_layout(replacement);
	else
		parent->child[parent->child[1] == old] = replacement;
}

static void
pane_branch_insert(struct cow_container *c,
    struct cow_container_layout		*destination,
    struct cow_container_layout *branch, struct cow_container_layout *leaf,
    enum cow_container_axis axis, bool before)
{
	branch->axis  = axis;
	branch->share = 5000;
	pane_replace(c, destination, branch);
	branch->child[before ? 0 : 1] = leaf;
	branch->child[before ? 1 : 0] = destination;
	destination->parent = leaf->parent = branch;
}

static void
pane_prune(struct cow_container *c, struct cow_container_pane *pane)
{
	struct cow_container_layout *parent = pane->layout.parent;

	if (c->persistent || pane->n_tabs != 0 || parent == NULL)
		return;
	struct cow_container_layout *sibling =
	    parent->child[parent->child[0] == &pane->layout];
	pane_replace(c, parent, sibling);
	xfree(container_pane_from_layout(parent));
	container_pane_destroy(pane);
}

static uint32_t
pane_outer_edges(const struct cow_container *c,
    const struct cow_container_pane *pane, bool display)
{
	uint32_t edges = COW_EDGE_ALL;
	if (c->shaded) {
		int count = 0, index = 0;
		pane_index(&c->root->layout, NULL, &count);
		int position = pane_index(&c->root->layout, &pane->layout,
		    &index);
		return COW_EDGE_TOP | COW_EDGE_BOTTOM |
		    (position == 0 ? COW_EDGE_LEFT : 0) |
		    (position == count - 1 ? COW_EDGE_RIGHT : 0);
	}
	for (const struct cow_container_layout *n =
		 (display ? pane_display_layout(c, pane) : &pane->layout);
	    n->parent != NULL; n = n->parent) {
		bool second = n == n->parent->child[1];
		if (n->parent->axis == COW_CONTAINER_HORIZONTAL)
			edges &= ~(second ? COW_EDGE_LEFT : COW_EDGE_RIGHT);
		else
			edges &= ~(second ? COW_EDGE_TOP : COW_EDGE_BOTTOM);
	}
	return edges;
}

uint32_t
cow_container_outer_edges(const struct cow_view *view)
{
	struct cow_container_pane *pane = cow_view_pane(view);
	return pane == NULL || view->fullscreen ?
	    COW_EDGE_ALL :
	    pane_outer_edges(cow_view_container(view), pane, true);
}

uint32_t
cow_container_shared_edges(const struct cow_view *view)
{
	return ~cow_container_outer_edges(view) &
	    (COW_EDGE_LEFT | COW_EDGE_TOP);
}

struct cow_view *
cow_container_border_title_target(struct cow_view *view, int x, int y)
{
	struct cow_container_pane *pane = cow_view_pane(view);
	if (pane == NULL || pane->title_hidden ||
	    cow_container_window_owner(view) != NULL)
		return view;
	struct cow_container	  *c	 = cow_view_container(view);
	struct cow_container_pane *other = NULL;
	while ((other = cow_container_pane_next(c, other)) != NULL) {
		if (!other->title_hidden || other->active == NULL)
			continue;
		struct cow_view *candidate = other->active->view;
		if (candidate->closed || cow_container_hides_view(candidate))
			continue;
		struct cow_box box = cow_view_frame_box(candidate);
		int	       bw  = candidate->dec.border_width;
		if (x < box.x || y < box.y || x >= box.x + box.width ||
		    y >= box.y + box.height)
			continue;
		/* These transparent strips receive input through the neighbour
		 * which draws the shared divider.
		 */
		uint32_t shared = cow_container_shared_edges(candidate);
		if (((shared & COW_EDGE_LEFT) && x < box.x + bw) ||
		    ((shared & COW_EDGE_TOP) && y < box.y + bw))
			return candidate;
	}
	return view;
}

static void
pane_frame_metrics(const struct cow_container_member *m, int *fw, int *fh,
    int *cx, int *cy, bool display)
{
	const struct cow_view *view = m->view;
	cow_titlebar_frame_geometry(view, 0, 0, fw, fh, cx, cy);
	/* Also works while validating restart state, before membership is live.
	 */
	int tt	  = pane_title_visible(m->container, m->pane) ?
	       view->dec.nominal_title_thickness :
	       0;
	int delta = tt - view->dec.title_thickness;
	if (cow_titlebar_is_vertical(view->cfg.titlebar.position))
		*fw += delta;
	else
		*fh += delta;
	if (view->cfg.titlebar.position == COW_TITLEBAR_TOP)
		*cy += delta;
	else if (view->cfg.titlebar.position == COW_TITLEBAR_LEFT)
		*cx += delta;
	uint32_t edges = ~pane_outer_edges(m->container, m->pane, display) &
	    (COW_EDGE_LEFT | COW_EDGE_TOP);
	int bw = view->dec.border_width;
	if (edges & COW_EDGE_LEFT) {
		*fw -= bw;
		*cx -= bw;
	}
	if (edges & COW_EDGE_TOP) {
		*fh -= bw;
		*cy -= bw;
	}
}

bool
container_pane_minimum(struct cow_container_pane *pane, bool client_hints)
{
	if (pane->layout.axis != COW_CONTAINER_LEAF)
		return container_pane_minimum(container_pane_from_layout(pane->layout.child[0]),
			   client_hints) &&
		    container_pane_minimum(container_pane_from_layout(pane->layout.child[1]),
			client_hints);
	int			     minw = 1, minh = 1;
	struct cow_container_member *m;
	wl_list_for_each(m, &pane->tabs, pane_link)
	{
		int		 fw, fh, cx, cy;
		struct cow_view *view = m->view;
		int		 w    = view->min_width > COW_MIN_WINDOW_DIM ?
				    view->min_width :
				    COW_MIN_WINDOW_DIM;
		int		 h    = view->min_height > COW_MIN_WINDOW_DIM ?
				    view->min_height :
				    COW_MIN_WINDOW_DIM;
		if (!client_hints)
			w = h = 1;
		if (pane->layout.collapsed == COW_CONTAINER_HORIZONTAL)
			w = 0;
		if (pane->layout.collapsed == COW_CONTAINER_VERTICAL)
			h = 0;
		pane_frame_metrics(m, &fw, &fh, &cx, &cy, false);
		if (fw < 0 || fh < 0 || w > INT_MAX - fw || h > INT_MAX - fh)
			return false;
		if (w + fw > minw)
			minw = w + fw;
		if (h + fh > minh)
			minh = h + fh;
	}
	pane->layout.min_width	= minw;
	pane->layout.min_height = minh;
	return true;
}

static int
pane_index(const struct cow_container_layout *node,
    const struct cow_container_layout *target, int *index)
{
	if (node == target)
		return *index;
	if (node->axis == COW_CONTAINER_LEAF) {
		(*index)++;
		return -1;
	}
	int found = pane_index(node->child[0], target, index);
	return found >= 0 ? found : pane_index(node->child[1], target, index);
}

struct cow_box
container_pane_content(const struct cow_container_member *m)
{
	struct cow_box box = pane_display_layout(m->container, m->pane)->box;
	int	       fw, fh, cx, cy;

	if (m->container->shaded) {
		int count = 0, index = 0;
		pane_index(&m->container->root->layout, NULL, &count);
		int position = pane_index(&m->container->root->layout,
		    &m->pane->layout, &index);
		box	     = m->container->frame;
		int start    = (int)((int64_t)box.width * position / count);
		int end = (int)((int64_t)box.width * (position + 1) / count);
		box.x += start;
		box.width = end - start;
		pane_frame_metrics(m, &fw, &fh, &cx, &cy, true);
		box.x += cx;
		box.y += cy;
		box.width  = box.width > fw ? box.width - fw : 1;
		box.height = 0;
		return box;
	}
	pane_frame_metrics(m, &fw, &fh, &cx, &cy, true);
	box.x += cx;
	box.y += cy;
	box.width -= fw;
	box.height -= fh;
	return box;
}

static bool
pane_has_title(const struct cow_container_pane *pane)
{
	return pane->n_tabs > 1 ||
	    pane->layout.collapsed != COW_CONTAINER_LEAF ||
	    (!pane->title_hidden && pane->active != NULL &&
		pane->active->view->cfg.titlebar.enabled);
}

static const struct cow_container_pane *
protected_title(const struct cow_container *c)
{
	if (c->shaded || (c->frame_view && c->frame_view->cfg.titlebar.enabled))
		return NULL;
	/* Keep a handle when the last titled pane is removed or replaced. */
	struct cow_container_member *m;
	wl_list_for_each(m, &c->members, link)
	{
		if (m == m->pane->active && pane_has_title(m->pane))
			return NULL;
	}
	struct cow_container_pane *first = NULL;
	while ((first = cow_container_pane_next(c, first)) != NULL) {
		if (first->active != NULL)
			return first;
	}
	return NULL;
}

static bool
pane_title_visible(const struct cow_container *c,
    const struct cow_container_pane	      *pane)
{
	return c->shaded || pane_has_title(pane) || protected_title(c) == pane;
}

bool
cow_container_title_visible(const struct cow_view *view)
{
	struct cow_container *c = cow_view_container(view);
	return c == NULL ? view->cfg.titlebar.enabled :
			   pane_title_visible(c, cow_view_pane(view));
}

static void
container_title_metrics(struct cow_container *c)
{
	const struct cow_container_pane *protected = protected_title(c);
	struct cow_container_member *m;
	wl_list_for_each(m, &c->members, link)
	{
		int thickness = (c->shaded || pane_has_title(m->pane) ||
				    protected == m->pane) ?
		    m->view->dec.nominal_title_thickness :
		    0;
		if (m->view->dec.title_thickness != thickness) {
			m->view->dec.title_thickness = thickness;
			m->view->dec.dirty	     = true;
		}
	}
}

static bool
title_can_hide(struct cow_view *view)
{
	struct cow_container	  *c	= cow_view_container(view);
	struct cow_container_pane *pane = cow_view_pane(view);
	if (c == NULL || c->shaded || pane->n_tabs > 1 ||
	    pane->layout.collapsed != COW_CONTAINER_LEAF)
		return false;
	struct cow_container_member *m;
	bool other = c->frame_view && c->frame_view->cfg.titlebar.enabled;
	wl_list_for_each(m, &c->members, link)
	{
		if (m->pane != pane && m == m->pane->active &&
		    cow_container_title_visible(m->view))
			other = true;
	}
	return other;
}

bool
cow_container_title_hide(struct cow_view *view)
{
	if (!title_can_hide(view))
		return false;
	struct cow_container	  *c	= cow_view_container(view);
	struct cow_container_pane *pane = cow_view_pane(view);
	pane->title_hidden = true;
	if (cow_container_arrange(c, c->frame))
		return true;
	pane->title_hidden = false;
	container_title_metrics(c);
	return false;
}

static struct cow_container *
title_container(struct cow_view *view)
{
	struct cow_container *c = cow_container_window_owner(view);
	return c ? c : cow_view_container(view);
}

static struct cow_view *
title_view(struct cow_container *c)
{
	if (c->active)
		return c->active->view;
	struct cow_container_pane *pane = NULL;
	while ((pane = cow_container_pane_next(c, pane)) != NULL) {
		if (pane->active)
			return pane->active->view;
	}
	return NULL;
}

bool
cow_container_titles_restore(struct cow_view *view)
{
	struct cow_container *c = title_container(view);
	if (c == NULL)
		return false;
	struct cow_container_member *m;
	wl_list_for_each(m, &c->members, link)
	{
		m->pane->title_hidden = false;
	}
	view = title_view(c);
	return view == NULL || cow_container_refresh(view);
}

bool
cow_container_titles_toggle(struct cow_view *view, bool all)
{
	struct cow_container *c = title_container(view);
	if (c == NULL)
		return false;
	if (!all) {
		/* An outer frame addresses its active pane, not its outer tabs.
		 */
		if (cow_container_window_owner(view))
			view = title_view(c);
		if (view == NULL)
			return false;
		struct cow_container_pane *pane = cow_view_pane(view);
		if (pane == NULL)
			return false;
		if (!pane->title_hidden)
			return cow_container_title_hide(view);
		pane->title_hidden = false;
		return cow_container_refresh(view);
	}
	struct cow_container_pane *pane = NULL;
	while ((pane = cow_container_pane_next(c, pane)) != NULL) {
		if (pane->title_hidden)
			return cow_container_titles_restore(view);
	}
	bool changed = false;
	while ((pane = cow_container_pane_next(c, pane)) != NULL) {
		if (pane->active && pane->active->view->cfg.titlebar.enabled &&
		    title_can_hide(pane->active->view)) {
			pane->title_hidden = true;
			changed		   = true;
		}
	}
	if (!changed)
		return false;
	if (cow_container_arrange(c, c->frame))
		return true;
	/* None were hidden on entry. Restore that state if layout fails. */
	while ((pane = cow_container_pane_next(c, pane)) != NULL)
		pane->title_hidden = false;
	container_title_metrics(c);
	return false;
}

bool
cow_container_zoom(struct cow_view *view, enum cow_container_zoom scope)
{
	struct cow_container *c = title_container(view);
	if (!c || scope < COW_CONTAINER_ZOOM_NONE ||
	    scope > COW_CONTAINER_ZOOM_ALL)
		return false;
	if (cow_container_window_owner(view))
		view = title_view(c);
	if (!view || !cow_container_is_split(view))
		return false;
	struct cow_container_pane *pane = cow_view_pane(view);
	if (scope != COW_CONTAINER_ZOOM_NONE &&
	    (c->shaded || view->fullscreen ||
		pane->layout.collapsed != COW_CONTAINER_LEAF))
		return false;
	bool restore = pane->zoom == scope;
	clear_zoom(c);
	if (!restore)
		pane->zoom = scope;
	container_apply_layout(c);
	cow_container_activate(c, view->container_member);
	cow_stack_mark_dirty();
	return true;
}

void
container_apply_layout(struct cow_container *c)
{
	container_title_metrics(c);
	struct cow_container_pane   *zoom = c->shaded ? NULL : zoomed_pane(c);
	struct cow_container_member *m;
	bool			     split = c->frame_view ||
	    c->root->layout.axis != COW_CONTAINER_LEAF;

	wl_list_for_each(m, &c->members, link)
	{
		if (m->view->fullscreen)
			continue;
		struct cow_box box = container_pane_content(m);
		m->view->use_ssd   = m->pane->active == m &&
		    !zoom_hides_pane(zoom, m->pane);
		m->view->dec.dirty = true;
		bool collapsed	   = m->pane->layout.collapsed !=
			COW_CONTAINER_LEAF ||
		    c->shaded;
		m->view->pending_clip = true;
		m->view->clip_x = m->view->clip_y = 0;
		/* A lone pane must resize freely, without its former tile clip.
		 */
		m->view->clip_w = collapsed ? 1 : split ? box.width : 0;
		m->view->clip_h = collapsed ? 1 : split ? box.height : 0;
		if (m->pane->layout.collapsed == COW_CONTAINER_HORIZONTAL)
			box.width = m->view->content_width;
		if (m->pane->layout.collapsed == COW_CONTAINER_VERTICAL)
			box.height = m->view->content_height;
		if (c->shaded) {
			box.width  = m->view->content_width;
			box.height = m->view->content_height;
		}
		cow_view_set_pending_geometry(m->view, box.x, box.y, box.width,
		    box.height);
		cow_view_set_tiled_edges(m->view,
		    split ? RIVER_WINDOW_V1_EDGES_TOP |
			    RIVER_WINDOW_V1_EDGES_BOTTOM |
			    RIVER_WINDOW_V1_EDGES_LEFT |
			    RIVER_WINDOW_V1_EDGES_RIGHT :
			    RIVER_WINDOW_V1_EDGES_NONE);
	}
	if (!split && c->root->active != NULL) {
		struct cow_box box = container_pane_content(c->root->active);
		c->x		   = box.x;
		c->y		   = box.y;
		c->content_width   = box.width;
		c->content_height  = box.height;
	}
	cow_update_request(COW_UPDATE_MANAGE | COW_UPDATE_STATUS);
}

bool
cow_container_shaded_size(const struct cow_view *view, int *width, int *height)
{
	struct cow_container *c = cow_view_container(view);
	if (c == NULL || !c->shaded || view->fullscreen)
		return false;
	struct cow_box box = container_pane_content(view->container_member);
	*width		   = box.width;
	*height		   = box.height;
	return true;
}

bool
cow_container_shade(struct cow_view *view, bool shaded)
{
	struct cow_container *c = cow_view_container(view);
	if (c == NULL || !cow_container_is_split(view) || c->shaded == shaded)
		return false;
	struct cow_container_member *m;
	wl_list_for_each(m, &c->members, link)
	{
		if (shaded &&
		    (m->view->fullscreen ||
			cow_titlebar_is_vertical(
			    m->view->cfg.titlebar.position)))
			return false;
	}
	c->shaded = shaded;
	container_apply_layout(c);
	return true;
}

struct cow_box
cow_container_frame_at(const struct cow_view *view, int x, int y)
{
	struct cow_container *c = cow_view_container(view);
	if (c == NULL || !cow_container_is_split(view))
		return cow_view_frame_box_at(view, x, y);
	struct cow_box content = container_pane_content(view->container_member);
	struct cow_box frame   = c->frame;
	frame.x += x - content.x;
	frame.y += y - content.y;
	return frame;
}

bool
cow_container_arrange(struct cow_container *c, struct cow_box frame)
{
	container_title_metrics(c);
	container_pane_normalize_columns(&c->root->layout);
	if (c->root->layout.axis == COW_CONTAINER_LEAF)
		c->shaded = false;
	if (!container_pane_minimum(c->root, true) ||
	    !cow_container_layout_arrange(&c->root->layout, frame, 0))
		return false;
	c->frame = frame;
	container_apply_layout(c);
	cow_container_window_geometry(c);
	return true;
}

/* Honour new minimum sizes without shrinking the user's floating layout. */
bool
cow_container_refresh(struct cow_view *view)
{
	if (!cow_container_is_split(view))
		return true;
	struct cow_container *c = cow_view_container(view);
	container_title_metrics(c);
	int		      width, height;
	if (!container_pane_minimum(c->root, true) ||
	    !cow_container_layout_minimum(&c->root->layout, 0, &width, &height))
		return false;
	struct cow_box frame = c->frame;
	if (frame.width < width)
		frame.width = width;
	if (frame.height < height)
		frame.height = height;
	bool grew = frame.width != c->frame.width ||
	    frame.height != c->frame.height;
	if (!cow_container_arrange(c, frame))
		return false;
	if (grew)
		cow_view_set_maximized(view, false);
	return true;
}

/* Translate cached layout coordinates without proposing client sizes. */
void
cow_container_sync_frame(struct cow_view *view)
{
	/* Pending pane positions are not yet reflected in x/y or vx/vy. */
	if (!cow_container_is_split(view) || view->fullscreen ||
	    view->pending_resize || view->deferred_position ||
	    !cow_view_is_active_tab(view))
		return;
	struct cow_container *c = cow_view_container(view);
	if (c->shaded)
		return;
	struct cow_box frame = cow_container_frame_at(view, view->x, view->y);
	if (frame.x == c->frame.x && frame.y == c->frame.y)
		return;
	if (cow_container_layout_arrange(&c->root->layout, frame, 0))
		c->frame = frame;
}

bool
cow_container_maximize(struct cow_view *view, bool horizontal, bool vertical,
    bool restore, bool keep, bool ignore_reserved)
{
	struct cow_container *c = cow_view_container(view);
	if (c == NULL || view->output == NULL || view->fullscreen)
		return false;
	cow_container_shade(view, false);
	struct cow_output *o = view->output;
	int ox = o->x - (view->desk != NULL ? view->desk->page_vx : 0);
	int oy = o->y - (view->desk != NULL ? view->desk->page_vy : 0);
	struct cow_box frame = c->frame, saved = c->restore_frame;
	struct cow_box area = ignore_reserved ?
	    (struct cow_box) { o->x, o->y, o->width, o->height } :
	    o->usable_area;
	if (horizontal) {
		if (restore) {
			if (c->maximized_h) {
				frame.x	    = saved.x + ox;
				frame.width = saved.width;
			}
		} else {
			if (!c->maximized_h) {
				saved.x	    = frame.x - ox;
				saved.width = frame.width;
			}
			if (!keep) {
				frame.x	    = area.x;
				frame.width = area.width;
			}
		}
	}
	if (vertical) {
		if (restore) {
			if (c->maximized_v) {
				frame.y	     = saved.y + oy;
				frame.height = saved.height;
			}
		} else {
			if (!c->maximized_v) {
				saved.y	     = frame.y - oy;
				saved.height = frame.height;
			}
			if (!keep) {
				frame.y	     = area.y;
				frame.height = area.height;
			}
		}
	}
	if (!cow_container_arrange(c, frame))
		return false;
	c->restore_frame = saved;
	if (horizontal)
		c->maximized_h = !restore;
	if (vertical)
		c->maximized_v = !restore;
	struct cow_container_member *m;
	wl_list_for_each(m, &c->members, link)
	{
		struct cow_view *v = m->view;
		v->maximized_h	   = c->maximized_h;
		v->maximized_v	   = c->maximized_v;
		v->maximized	   = v->maximized_h || v->maximized_v;
		if (horizontal) {
			v->maximized_h_ignore_reserved = !restore &&
			    ignore_reserved;
			v->maximized_h_keep_geometry = !restore && keep;
		}
		if (vertical) {
			v->maximized_v_ignore_reserved = !restore &&
			    ignore_reserved;
			v->maximized_v_keep_geometry = !restore && keep;
		}
		v->icon_inverted_button = v->maximized ?
		    cow_view_maximize_button_index() :
		    -1;
		cow_view_set_pending_maximized_hint(v, v->maximized);
		v->dec.dirty = true;
	}
	return true;
}

void
cow_container_reflow(struct cow_view *view)
{
	struct cow_container *c = cow_view_container(view);
	if (c == NULL || c->active == NULL || c->active->view != view)
		return;
	if (c->maximized_h)
		cow_container_maximize(view, true, false, false,
		    view->maximized_h_keep_geometry,
		    view->maximized_h_ignore_reserved);
	if (c->maximized_v)
		cow_container_maximize(view, false, true, false,
		    view->maximized_v_keep_geometry,
		    view->maximized_v_ignore_reserved);
}

bool
cow_container_is_split(const struct cow_view *view)
{
	struct cow_container *c = cow_view_container(view);

	return c != NULL &&
	    (c->frame_view || c->root->layout.axis != COW_CONTAINER_LEAF);
}

static bool
pane_can_expand(const struct cow_container_layout *node,
    const struct cow_container_layout		  *except)
{
	if (node->axis == COW_CONTAINER_LEAF)
		return node != except && node->collapsed == COW_CONTAINER_LEAF;
	return pane_can_expand(node->child[0], except) ||
	    pane_can_expand(node->child[1], except);
}

static struct cow_container_layout *
pane_column(struct cow_container_layout *row)
{
	while (
	    row->parent != NULL && row->parent->axis == COW_CONTAINER_VERTICAL)
		row = row->parent;
	return row;
}

bool
cow_container_collapse(struct cow_view *view)
{
	struct cow_container *zoom_container = cow_view_container(view);
	if (zoom_container && zoomed_pane(zoom_container))
		return false;
	if (!cow_container_is_split(view) || view->fullscreen ||
	    !cow_container_title_visible(view))
		return false;
	struct cow_container	    *c	  = cow_view_container(view);
	struct cow_container_layout *leaf = &cow_view_pane(view)->layout;
	enum cow_container_axis	     old  = leaf->collapsed;
	if (old == COW_CONTAINER_LEAF &&
	    (cow_titlebar_is_vertical(view->cfg.titlebar.position) ||
		!pane_can_expand(pane_column(leaf), leaf)))
		return false;
	leaf->collapsed = old != COW_CONTAINER_LEAF ? COW_CONTAINER_LEAF :
						      COW_CONTAINER_VERTICAL;
	if (cow_container_arrange(c, c->frame))
		return true;
	leaf->collapsed = old;
	return false;
}

bool
cow_container_equalize(struct cow_view *view)
{
	struct cow_container_pane *pane = cow_view_pane(view);
	if (pane == NULL || pane->layout.parent == NULL)
		return false;
	struct cow_container_layout *parent = pane->layout.parent;
	unsigned		     old    = parent->share;
	parent->share			    = 5000;
	struct cow_container *c		    = cow_view_container(view);
	if (cow_container_arrange(c, c->frame))
		return true;
	parent->share = old;
	return false;
}

bool
cow_container_rotate(struct cow_container *c, unsigned turns)
{
	if (c == NULL || c->root->layout.axis == COW_CONTAINER_LEAF ||
	    turns < 1 || turns > 3 || c->shaded || zoomed_pane(c))
		return false;
	if (c->frame_view &&
	    (c->frame_view->fullscreen || c->frame_view->sd != COW_SHADE_NONE))
		return false;
	struct cow_container_pane *pane = NULL;
	while ((pane = cow_container_pane_next(c, pane)) != NULL) {
		if (pane->layout.collapsed != COW_CONTAINER_LEAF)
			return false;
	}
	struct cow_container_member *m;
	wl_list_for_each(m, &c->members, link)
	{
		if (m->view->fullscreen)
			return false;
	}
	cow_container_layout_rotate(&c->root->layout, turns);
	if (cow_container_arrange(c, c->frame))
		return true;
	/* Arrange validates minima before changing any boxes. Undo the tree
	 * transformation and refresh the orientation-dependent frame minima.
	 */
	cow_container_layout_rotate(&c->root->layout, 4 - turns);
	container_pane_minimum(c->root, true);
	return false;
}

struct cow_view *
cow_container_neighbor(struct cow_view *view, uint32_t edge)
{
	struct cow_container *c = cow_view_container(view);
	if (c == NULL)
		return NULL;
	struct cow_box		     from     = cow_view_pane(view)->layout.box;
	struct cow_view		    *best     = NULL;
	int64_t			     distance = INT64_MAX;
	struct cow_container_member *m;
	wl_list_for_each(m, &c->members, link)
	{
		if (m->pane == cow_view_pane(view) || m != m->pane->active)
			continue;
		struct cow_box to = m->pane->layout.box;
		int64_t	       dx = (int64_t)to.x * 2 + to.width -
		    ((int64_t)from.x * 2 + from.width);
		int64_t dy = (int64_t)to.y * 2 + to.height -
		    ((int64_t)from.y * 2 + from.height);
		if ((edge == COW_EDGE_LEFT && dx >= 0) ||
		    (edge == COW_EDGE_RIGHT && dx <= 0) ||
		    (edge == COW_EDGE_TOP && dy >= 0) ||
		    (edge == COW_EDGE_BOTTOM && dy <= 0))
			continue;
		int64_t score = (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);
		if (score < distance) {
			best	 = m->view;
			distance = score;
		}
	}
	return best;
}

static struct cow_container_layout *
pane_divider(struct cow_container_layout *leaf, uint32_t edge)
{
	bool horizontal = (edge & (COW_EDGE_LEFT | COW_EDGE_RIGHT)) != 0;
	bool before = (edge & (COW_EDGE_TOP | COW_EDGE_LEFT)) != 0;
	for (struct cow_container_layout *n = leaf; n->parent != NULL;
	    n				    = n->parent) {
		struct cow_container_layout *p = n->parent;
		if ((p->axis == COW_CONTAINER_HORIZONTAL) == horizontal &&
		    p->child[before ? 1 : 0] == n)
			return p;
	}
	return NULL;
}

bool
cow_container_resize(struct cow_view *view, uint32_t edges,
    struct cow_box initial, struct cow_box pane, int dx, int dy)
{
	struct cow_container *c = cow_view_container(view);
	if (!cow_container_is_split(view) || view->fullscreen || zoomed_pane(c))
		return false;
	struct cow_container_layout *leaf =
	    &view->container_member->pane->layout;
	struct cow_container_layout *changed[2] = { NULL, NULL };
	unsigned		     shares[2]	= { 0, 0 };
	struct cow_box		     frame	= initial;
	for (int axis = 0; axis < 2; axis++) {
		uint32_t leading = axis == 0 ? COW_EDGE_LEFT : COW_EDGE_TOP;
		uint32_t trailing = axis == 0 ? COW_EDGE_RIGHT : COW_EDGE_BOTTOM;
		uint32_t edge	  = edges & (leading | trailing);
		if (edge == 0)
			continue;
		bool			     before = (edge & leading) != 0;
		struct cow_container_layout *p	    = pane_divider(leaf,
			 before ? leading : trailing);
		int			     delta  = axis == 0 ? dx : dy;
		if (p != NULL) {
			int origin = axis == 0 ? p->box.x : p->box.y;
			int size   = axis == 0 ? p->box.width : p->box.height;
			int pos	   = axis == 0 ? pane.x : pane.y;
			if (!before)
				pos += axis == 0 ? pane.width : pane.height;
			int64_t share = ((int64_t)pos + delta - origin) *
			    10000 / size;
			changed[axis] = p;
			shares[axis]  = p->share;
			p->share      = share < 1 ? 1 :
				 share > 9999	  ? 9999 :
						    (unsigned)share;
		} else {
			int    *origin = axis == 0 ? &frame.x : &frame.y;
			int    *size = axis == 0 ? &frame.width : &frame.height;
			int64_t extent = (int64_t)*size +
			    (before ? -(int64_t)delta : delta);
			int64_t start = (int64_t)*origin + (before ? delta : 0);
			if (extent < 1 || extent > INT_MAX || start < INT_MIN ||
			    start > INT_MAX)
				goto fail;
			*origin = (int)start;
			*size	= (int)extent;
		}
	}
	if (cow_container_arrange(c, frame))
		return true;
fail:
	for (int axis = 0; axis < 2; axis++)
		if (changed[axis] != NULL)
			changed[axis]->share = shares[axis];
	return false;
}

void
cow_container_resize_save(struct cow_view *view, uint32_t edges, int ids[2],
    unsigned shares[2])
{
	for (int axis = 0; axis < 2; axis++) {
		uint32_t edge = edges & (axis == 0 ?
		    COW_EDGE_LEFT | COW_EDGE_RIGHT : COW_EDGE_TOP | COW_EDGE_BOTTOM);
		struct cow_container_layout *p = edge != 0 ?
		    pane_divider(&cow_view_pane(view)->layout, edge) :
		    NULL;
		ids[axis]    = p != NULL ? container_pane_from_layout(p)->id : -1;
		shares[axis] = p != NULL ? p->share : 0;
	}
}

bool
cow_container_resize_restore(struct cow_view *view, uint32_t edges,
    struct cow_box frame, const int ids[2], const unsigned shares[2])
{
	if (!cow_container_is_split(view))
		return false;
	struct cow_container_layout *nodes[2];
	unsigned		     previous[2];
	for (int axis = 0; axis < 2; axis++) {
		uint32_t edge = edges & (axis == 0 ?
		    COW_EDGE_LEFT | COW_EDGE_RIGHT : COW_EDGE_TOP | COW_EDGE_BOTTOM);
		nodes[axis]   = edge != 0 ?
		      pane_divider(&cow_view_pane(view)->layout, edge) :
		      NULL;
		if ((nodes[axis] != NULL ? container_pane_from_layout(nodes[axis])->id :
					   -1) != ids[axis])
			return false;
		previous[axis] = nodes[axis] != NULL ? nodes[axis]->share : 0;
	}
	for (int axis = 0; axis < 2; axis++) {
		if (nodes[axis] != NULL)
			nodes[axis]->share = shares[axis];
	}
	if (cow_container_arrange(cow_view_container(view), frame))
		return true;
	for (int axis = 0; axis < 2; axis++) {
		if (nodes[axis] != NULL)
			nodes[axis]->share = previous[axis];
	}
	return false;
}

bool
container_frame_delta(struct cow_box frame, struct cow_box old, struct cow_box requested,
    struct cow_box *result)
{
	int64_t x = (int64_t)frame.x + requested.x - old.x;
	int64_t y = (int64_t)frame.y + requested.y - old.y;
	int64_t w = (int64_t)frame.width + requested.width - old.width;
	int64_t h = (int64_t)frame.height + requested.height - old.height;

	if (x < INT_MIN || x > INT_MAX || y < INT_MIN || y > INT_MAX || w < 1 ||
	    w > INT_MAX || h < 1 || h > INT_MAX)
		return false;
	*result = (struct cow_box) { (int)x, (int)y, (int)w, (int)h };
	return true;
}

static void
member_unlink(struct cow_container_member *m)
{
	struct cow_container_pane *pane = m->pane;

	wl_list_remove(&m->link);
	wl_list_remove(&m->pane_link);
	m->container->n_members--;
	pane->n_tabs--;
	if (pane->n_tabs == 0)
		pane->zoom = COW_CONTAINER_ZOOM_NONE;
	if (m->container->active == m)
		m->container->active = NULL;
	if (m->container->persistent && pane->n_tabs == 0)
		pane->layout.collapsed = COW_CONTAINER_LEAF;
	if (pane->active == m) {
		pane->active = NULL;
		if (!wl_list_empty(&pane->tabs))
			pane->active = wl_container_of(pane->tabs.next,
			    pane->active, pane_link);
	}
	m->view->container_member = NULL;
	m->view->dec.title_thickness = m->view->cfg.titlebar.enabled ?
	    m->view->dec.nominal_title_thickness :
	    0;
	m->view->dec.dirty	     = true;
	m->view->pending_clip	  = true;
	m->view->clip_x = m->view->clip_y = 0;
	m->view->clip_w = m->view->clip_h = 0;
}

/* Remove membership and repair the surviving pane, leaving geometry and
 * member lifetime to the caller.
 */
static void
member_detach(struct cow_container_member *m)
{
	struct cow_container *c = m->container;
	struct cow_container_pane *pane = m->pane;
	member_unlink(m);
	if (pane->active != NULL) {
		pane->active->view->use_ssd = true;
		pane->active->view->dec.dirty = true;
	}
	pane_prune(c, pane);
}

struct cow_container *
cow_container_create(struct cow_view *first, struct cow_view *second)
{
	struct cow_container *c = xcalloc(1, sizeof(*c));

	wl_list_init(&c->members);
	c->root	     = container_pane_create();
	c->n_members = 0;
	c->id	     = next_container_id++;

	/* Geometry, desk and output come from the drop target (first) */
	c->x		  = first->x;
	c->y		  = first->y;
	c->content_width  = first->content_width;
	c->content_height = first->content_height;
	c->desk		  = first->desk;
	c->output	  = first->output;
	c->flags	  = first->flags;

	wl_list_insert(&cow_wm.containers, &c->wm_link);

	cow_container_add(c, first);
	if (second != NULL)
		cow_container_add(c, second);

	/* first (the drop target) is the active tab */
	c->active = wl_container_of(c->members.next, c->active, link);

	cow_update_request(COW_UPDATE_MANAGE);
	return c;
}

struct cow_container *
cow_container_find(const char *name)
{
	if (name == NULL || *name == '\0')
		return NULL;
	struct cow_container *c;
	wl_list_for_each(c, &cow_wm.containers, wm_link)
	{
		if (c->name != NULL && strcmp(c->name, name) == 0)
			return c;
	}
	return NULL;
}

struct cow_container_pane *
cow_container_pane_next(const struct cow_container *c,
    struct cow_container_pane			   *pane)
{
	struct cow_container_layout *node = c->root ? &c->root->layout : NULL;
	if (pane != NULL) {
		node = &pane->layout;
		while (node->parent != NULL && node == node->parent->child[1])
			node = node->parent;
		if (node->parent == NULL)
			return NULL;
		node = node->parent->child[1];
	}
	while (node != NULL && node->axis != COW_CONTAINER_LEAF)
		node = node->child[0];
	return container_pane_from_layout(node);
}

struct cow_container_pane *
cow_container_pane_find(struct cow_container *c, const char *name)
{
	if (c == NULL || name == NULL || *name == '\0')
		return NULL;
	struct cow_container_pane *pane = NULL;
	while ((pane = cow_container_pane_next(c, pane)) != NULL) {
		if (pane->name != NULL && strcmp(pane->name, name) == 0)
			return pane;
	}
	return NULL;
}

struct cow_container *
cow_container_create_empty(const char *name, const char *pane_name)
{
	if (*name == '\0' || (pane_name && *pane_name == '\0'))
		return NULL;
	struct cow_container *existing = cow_container_find(name);
	if (existing != NULL)
		return existing->persistent &&
			(!pane_name || cow_container_pane_find(existing, pane_name)) ?
		    existing :
		    NULL;
	struct cow_container *c = xcalloc(1, sizeof(*c));
	c->name			= xstrdup(name);
	c->id			= next_container_id++;
	c->persistent		= true;
	wl_list_init(&c->members);
	c->root	      = container_pane_create();
	c->root->name = xstrdup0(pane_name);
	wl_list_insert(&cow_wm.containers, &c->wm_link);
	cow_update_request(COW_UPDATE_MANAGE | COW_UPDATE_STATUS);
	return c;
}

static bool
container_can_split(const struct cow_container *c)
{
	struct cow_container_member *m;
	wl_list_for_each(m, &c->members, link)
	{
		if (cow_container_window_owner(m->view))
			return false;
	}
	/* Keep every branch within the restart decoder's depth limit. A new
	 * column can wrap more than the targeted leaf, including deeper rows.
	 */
	struct cow_container_pane *pane = NULL;
	while ((pane = cow_container_pane_next(c, pane)) != NULL) {
		unsigned depth = 0;
		for (struct cow_container_layout *n = &pane->layout; n;
		    n				    = n->parent) {
			if (++depth >= 64)
				return false;
		}
	}
	return true;
}

static bool
container_split_outer_tab(struct cow_view *target, struct cow_view *source,
    enum cow_container_axis axis, bool before, const char *empty_name)
{
	struct cow_container *outer = cow_view_container(target);
	if (!outer || outer->framed || outer->persistent ||
	    outer->root->layout.axis != COW_CONTAINER_LEAF)
		return false;
	if (cow_container_window_owner(target) ||
	    (source && cow_container_window_owner(source)))
		return false;
	struct cow_container *pending;
	wl_list_for_each(pending, &cow_wm.containers, wm_link)
	{
		if (pending->split_target == target ||
		    pending->split_source == target ||
		    (source && (pending->split_target == source ||
			pending->split_source == source)))
			return false;
	}
	struct cow_container *c = xcalloc(1, sizeof(*c));
	wl_list_init(&c->members);
	c->root		= container_pane_create();
	c->id		= next_container_id++;
	c->framed	= true;
	c->frame	= (struct cow_box) { target->x, target->y,
		       target->content_width, target->content_height };
	c->output	= target->output;
	c->desk		= target->desk;
	c->flags	= target->flags;
	c->split_target = target;
	c->split_source = source;
	c->split_empty = source == NULL;
	if (c->split_empty)
		c->root->name = xstrdup0(empty_name);
	c->split_axis	= axis;
	c->split_before = before;
	wl_list_insert(&cow_wm.containers, &c->wm_link);
	cow_update_request(COW_UPDATE_MANAGE);
	return true;
}

bool
cow_container_complete_tab_split(struct cow_container *c)
{
	struct cow_view *target = c->split_target, *source = c->split_source;
	struct cow_view *frame = c->frame_view;
	if (!target || !frame || target->closed ||
	    (!c->split_empty && (!source || source->closed)))
		return false;
	struct cow_container_member *tm = target->container_member;
	struct cow_container_member *sm = source ? source->container_member : NULL;
	if (!tm || (!c->split_empty && (!sm || tm == sm ||
	    tm->container != sm->container || tm->pane != sm->pane)) ||
	    tm->container->framed ||
	    tm->container->root->layout.axis != COW_CONTAINER_LEAF)
		return false;
	struct cow_container	    *outer = tm->container;
	struct cow_container_member *members[2] = { NULL, NULL };
	struct cow_container_pane   *leaves[2];
	struct cow_view		    *views[2] = { target, source };
	/* Validate the new layout before changing either outer tab. */
	for (int i = 0; i < 2; i++) {
		leaves[i]	      = container_pane_create();
		leaves[i]->layout.parent = &c->root->layout;
		if (i == 1 && c->split_empty)
			continue;
		members[i]	      = xcalloc(1, sizeof(*members[i]));
		*members[i]	      = i == 0 ? *tm : *sm;
		members[i]->container = c;
		members[i]->pane      = leaves[i];
		wl_list_insert(&leaves[i]->tabs, &members[i]->pane_link);
		wl_list_insert(c->members.prev, &members[i]->link);
		leaves[i]->n_tabs	 = 1;
		leaves[i]->active	 = members[i];
	}
	c->root->layout.axis			       = c->split_axis;
	c->root->layout.share			       = 5000;
	c->root->layout.child[c->split_before ? 1 : 0] = &leaves[0]->layout;
	c->root->layout.child[c->split_before ? 0 : 1] = &leaves[1]->layout;
	if (!container_pane_minimum(c->root, true) ||
	    !cow_container_layout_arrange(&c->root->layout, c->frame, 0)) {
		for (int i = 0; i < 2; i++) {
			if (members[i] == NULL)
				continue;
			wl_list_remove(&members[i]->link);
			wl_list_remove(&members[i]->pane_link);
			xfree(members[i]);
		}
		return false;
	}
	bool active = outer->active == tm || outer->active == sm;
	if (sm != NULL) {
		member_unlink(sm);
		xfree(sm);
	}
	/* Replace the target in place, retaining the outer tab order. */
	tm->view		= frame;
	frame->container_member = tm;
	c->n_members = c->split_empty ? 1 : 2;
	if (c->split_empty) {
		leaves[1]->name = c->root->name;
		c->root->name = NULL;
		c->persistent = true;
	}
	c->split_target = c->split_source = NULL;
	for (int i = 0; i < 2; i++) {
		if (members[i] == NULL)
			continue;
		views[i]->container_member = members[i];
		views[i]->use_ssd	   = true;
		views[i]->dec.dirty	   = true;
	}
	frame->layer	 = target->layer;
	frame->raise_seq = target->raise_seq;
	frame->flags	 = target->flags;
	container_title_metrics(outer);
	container_apply_layout(c);
	if (active)
		cow_container_activate(c, members[c->split_empty ? 0 : 1]);
	else {
		c->active      = members[0];
		frame->use_ssd = false;
	}
	cow_stack_mark_dirty();
	cow_update_request(COW_UPDATE_MANAGE | COW_UPDATE_STATUS);
	return true;
}

bool
cow_container_split_empty(struct cow_container *c,
    struct cow_container_pane *pane, const char *name,
    enum cow_container_axis axis, bool before)
{
	if ((name != NULL && *name == '\0') ||
	    (axis != COW_CONTAINER_HORIZONTAL &&
		axis != COW_CONTAINER_VERTICAL))
		return false;
	/* Declarations may be replayed by config reload without moving clients.
	 */
	if (cow_container_pane_find(c, name))
		return true;
	if (!container_can_split(c))
		return pane && pane->active &&
		    container_split_outer_tab(pane->active->view, NULL,
			axis, before, name);
	struct cow_box frame = c->frame;
	if (!c->persistent && !c->frame_view &&
	    c->root->layout.axis == COW_CONTAINER_LEAF && pane->active)
		frame = cow_view_frame_box(pane->active->view);
	struct cow_container_layout *destination = axis ==
		COW_CONTAINER_HORIZONTAL ?
	    pane_column(&pane->layout) :
	    &pane->layout;
	struct cow_container_pane   *leaf	 = container_pane_create(),
				  *branch	 = container_pane_create();
	leaf->name				 = xstrdup0(name);
	pane_branch_insert(c, destination, &branch->layout, &leaf->layout, axis,
	    before);
	if ((frame.width == 0 && c->n_members == 0) ||
	    cow_container_arrange(c, frame)) {
		c->persistent = true;
		cow_update_request(COW_UPDATE_MANAGE | COW_UPDATE_STATUS);
		return true;
	}
	pane_replace(c, &branch->layout, destination);
	container_pane_destroy(leaf);
	xfree(branch);
	container_title_metrics(c);
	return false;
}

bool
cow_container_set_name(struct cow_view *view, const char *name)
{
	struct cow_container *c	    = cow_view_container(view);
	struct cow_container *other = cow_container_find(name);
	if (c != NULL && c->persistent && *name == '\0')
		return false;
	if (other != NULL && other != c)
		return false;
	if (c == NULL) {
		if (*name == '\0' || view->parent != NULL || view->fullscreen)
			return false;
		c = cow_container_create(view, NULL);
	}
	return cow_container_rename(c, name);
}

bool
cow_container_rename(struct cow_container *c, const char *name)
{
	struct cow_container *other = cow_container_find(name);
	if ((other != NULL && other != c) || (c->persistent && *name == '\0'))
		return false;
	char *copy = *name != '\0' ? xstrdup(name) : NULL;
	xfree(c->name);
	c->name = copy;
	if (c->name == NULL && c->n_members == 1 &&
	    (!c->frame_view || !c->frame_view->container_member))
		cow_container_destroy(c);
	cow_update_request(COW_UPDATE_MANAGE | COW_UPDATE_STATUS);
	return true;
}

bool
cow_container_pane_set_name(struct cow_view *view, const char *name)
{
	struct cow_container	  *c	= cow_view_container(view);
	struct cow_container_pane *pane = cow_view_pane(view);
	return pane != NULL && cow_container_pane_rename(c, pane, name);
}

bool
cow_container_pane_rename(struct cow_container *c,
    struct cow_container_pane *pane, const char *name)
{
	struct cow_container_pane *other = cow_container_pane_find(c, name);
	if (pane == NULL || (other != NULL && other != pane))
		return false;
	char *copy = *name != '\0' ? xstrdup(name) : NULL;
	xfree(pane->name);
	pane->name = copy;
	cow_update_request(COW_UPDATE_MANAGE | COW_UPDATE_STATUS);
	return true;
}

bool
cow_container_can_tab(const struct cow_container *c,
    const struct cow_view			 *view)
{
	if (!view || view->container_placeholder)
		return false;
	struct cow_container *child = cow_container_window_owner(view);
	if (!child)
		return true;
	/* Layouts may be outer tabs, never panes of another layout. This also
	 * prevents a layout from containing itself, directly or indirectly.
	 */
	return c == NULL ||
	    (c != child && !c->persistent && !c->frame_view && !c->framed &&
		(!c->root || c->root->layout.axis == COW_CONTAINER_LEAF));
}

static bool
container_add(struct cow_container *c, struct cow_container_pane *pane,
    struct cow_view *view)
{
	if (view->container_member != NULL ||
	    (!view->container_placeholder && !cow_container_can_tab(c, view)))
		return false;

	struct cow_container_member *m = xcalloc(1, sizeof(*m));
	m->view			       = view;
	m->container		       = c;
	m->pane			       = pane;
	wl_list_insert(m->pane->tabs.prev, &m->pane_link);
	m->pane->n_tabs++;
	if (m->pane->active == NULL)
		m->pane->active = m;
	wl_list_insert(c->members.prev, &m->link); /* append */
	c->n_members++;

	view->container_member = m;

	/* Reject an addition that cannot fit before changing client state. */
	if ((c->persistent || c->root->layout.axis != COW_CONTAINER_LEAF) &&
	    (!container_pane_minimum(c->root, true) ||
		!cow_container_layout_arrange(&c->root->layout, c->frame, 0))) {
		member_unlink(m);
		xfree(m);
		return false;
	}

	/* Save pre-container geometry for restoration on detach */
	m->pre_x      = view->x;
	m->pre_y      = view->y;
	m->pre_width  = view->content_width;
	m->pre_height = view->content_height;

	/* A newly joined tab shares the group's hidden state. */
	uint32_t flags = c->active != NULL ? c->active->view->flags : c->flags;
	view->flags    = (view->flags & ~COW_VIEW_ICONIFIED_FLAG) |
	    (flags & COW_VIEW_ICONIFIED_FLAG);
	view->flags = (view->flags & ~COW_VIEW_STICKY_FLAG) |
	    (flags & COW_VIEW_STICKY_FLAG);
	if (c->active != NULL)
		view->layer = c->active->view->layer;
	cow_view_icon_refresh(view);
	if (c->output != NULL &&
	    (view->output != c->output || view->desk != c->desk)) {
		wl_list_remove(&view->desk_views);
		wl_list_remove(&view->output_views);
		view->output = c->output;
		view->desk   = c->desk;
		wl_list_insert(&c->desk->views, &view->desk_views);
		wl_list_insert(&c->output->visible_views, &view->output_views);
	}

	/* Non-active members must not draw their own frame */
	if (m->pane->active != m) {
		view->use_ssd = false;
		cow_view_drop_decoration(view);
	}

	/* Sync dimensions to container */
	container_title_metrics(c);
	if (c->root->layout.axis == COW_CONTAINER_LEAF && !c->persistent)
		cow_view_set_pending_geometry(view, c->x, c->y,
		    c->content_width, c->content_height);
	else
		container_apply_layout(c);
	return true;
}

bool
cow_container_add(struct cow_container *c, struct cow_view *view)
{
	return container_add(c,
	    c->active != NULL ? c->active->pane :
				cow_container_pane_next(c, NULL),
	    view);
}

bool
cow_container_insert(struct cow_container *c, struct cow_container_pane *pane,
    struct cow_view *view)
{
	if (cow_container_window_view(view))
		return false;
	if (view->closed || view->parent != NULL || view->fullscreen ||
	    view->container_member != NULL)
		return !view->closed && cow_view_pane(view) == pane;
	struct cow_box saved	= c->frame;
	bool	       unplaced = c->n_members == 0 && c->output == NULL;
	if (unplaced)
		c->frame = cow_view_frame_box(view);
	if (!container_add(c, pane, view)) {
		c->frame = saved;
		return false;
	}
	if (c->frame_view != NULL)
		view->layer = c->frame_view->layer;
	cow_container_placeholder_destroy(pane);
	if (c->output == NULL) {
		c->output = view->output;
		c->desk	  = view->desk;
		c->flags  = view->flags;
	}
	cow_container_activate(c, view->container_member);
	cow_container_arrange(c, c->frame);
	return true;
}

int
cow_container_window_count(const struct cow_container *c)
{
	int			     count = 0;
	struct cow_container_member *m;
	wl_list_for_each(m, &c->members, link)
	{
		if (!m->view->container_placeholder)
			count++;
	}
	return count;
}

int
cow_container_pane_window_count(const struct cow_container_pane *pane)
{
	int			     count = 0;
	struct cow_container_member *m;
	wl_list_for_each(m, &pane->tabs, pane_link)
	{
		if (!m->view->container_placeholder)
			count++;
	}
	return count;
}

bool
cow_container_add_placeholder(struct cow_container *c,
    struct cow_container_pane *pane, struct cow_view *view)
{
	if (pane->n_tabs || !view->container_placeholder ||
	    !container_add(c, pane, view))
		return false;
	if (c->frame_view) {
		view->raise_seq = c->frame_view->raise_seq;
		view->layer	= c->frame_view->layer;
	}
	cow_container_arrange(c, c->frame);
	cow_stack_mark_dirty();
	return true;
}

void
cow_container_remove_empty_pane(struct cow_container *c,
    struct cow_container_pane			     *pane)
{
	if (cow_container_pane_window_count(pane))
		return;
	if (!pane->layout.parent) {
		cow_container_destroy(c);
		return;
	}
	cow_container_placeholder_destroy(pane);
	bool persistent = c->persistent;
	c->persistent	= false;
	pane_prune(c, pane);
	c->persistent = persistent;
	cow_container_arrange(c, c->frame);
	cow_update_request(COW_UPDATE_MANAGE | COW_UPDATE_STATUS);
}

bool
cow_container_add_to(struct cow_view *target, struct cow_view *source)
{
	struct cow_container_member *m = target->container_member;

	return m != NULL && container_add(m->container, m->pane, source);
}

/* Move a whole pane, keeping its tabs and saved detach geometries intact. */
bool
cow_container_rearrange(struct cow_view *target, struct cow_view *source,
    enum cow_container_axis axis, bool before)
{
	struct cow_container	  *c	= cow_view_container(source);
	struct cow_container_pane *from = cow_view_pane(source);
	struct cow_container_pane *to	= cow_view_pane(target);
	if (!c || cow_view_container(target) != c || !from || !to ||
	    from == to || source->closed || target->closed ||
	    zoomed_pane(c) != NULL || source->container_placeholder ||
	    !from->layout.parent ||
	    (axis != COW_CONTAINER_LEAF && axis != COW_CONTAINER_HORIZONTAL &&
		axis != COW_CONTAINER_VERTICAL))
		return false;

	struct cow_container_layout *parent = from->layout.parent;
	struct cow_container_layout  saved  = *parent;
	struct cow_container_layout *sibling =
	    parent->child[parent->child[0] == &from->layout];
	struct cow_container_member *active = to->active;
	int			     count  = from->n_tabs;
	pane_replace(c, parent, sibling);
	struct cow_container_layout *destination = axis ==
		COW_CONTAINER_HORIZONTAL ?
	    pane_column(&to->layout) :
	    &to->layout;

	if (axis == COW_CONTAINER_LEAF) {
		struct cow_container_member *m, *next;
		wl_list_for_each_safe(m, next, &from->tabs, pane_link)
		{
			wl_list_remove(&m->pane_link);
			wl_list_insert(to->tabs.prev, &m->pane_link);
			m->pane = to;
		}
		to->n_tabs += count;
		from->n_tabs = 0;
		to->active   = from->active;
	} else {
		pane_branch_insert(c, destination, parent, &from->layout, axis,
		    before);
	}
	if (!cow_container_arrange(c, c->frame)) {
		if (axis == COW_CONTAINER_LEAF) {
			for (int i = 0; i < count; i++) {
				struct cow_container_member *m =
				    wl_container_of(to->tabs.prev, m,
					pane_link);
				wl_list_remove(&m->pane_link);
				wl_list_insert(&from->tabs, &m->pane_link);
				m->pane = from;
			}
			from->n_tabs = count;
			to->n_tabs -= count;
			to->active = active;
		} else {
			pane_replace(c, parent, destination);
		}
		pane_replace(c, sibling, parent);
		*parent		    = saved;
		from->layout.parent = sibling->parent = parent;
		container_title_metrics(c);
		return false;
	}
	if (axis == COW_CONTAINER_LEAF) {
		cow_container_placeholder_destroy(to);
		xfree(container_pane_from_layout(parent));
		container_pane_destroy(from);
	}
	cow_container_activate(c, source->container_member);
	cow_update_request(COW_UPDATE_MANAGE | COW_UPDATE_STATUS);
	return true;
}

bool
cow_container_split(struct cow_view *target, struct cow_view *source,
    enum cow_container_axis axis, bool before)
{
	struct cow_container_member *tm = target->container_member;
	struct cow_container_member *sm = source->container_member;

	if (target == source || tm == NULL || sm == NULL ||
	    tm->container != sm->container || tm->pane != sm->pane ||
	    (axis != COW_CONTAINER_HORIZONTAL &&
		axis != COW_CONTAINER_VERTICAL))
		return false;

	if (!container_can_split(tm->container))
		return container_split_outer_tab(target, source, axis, before, NULL);

	struct cow_container	    *c		     = tm->container;
	struct cow_container_pane   *old	     = tm->pane;
	struct cow_container_pane   *leaf	     = container_pane_create();
	struct cow_container_pane   *branch	     = container_pane_create();
	struct cow_container_member *previous_active = old->active;
	struct cow_container_layout *destination     = axis ==
		    COW_CONTAINER_HORIZONTAL ?
		pane_column(&old->layout) :
		&old->layout;
	struct wl_list		    *previous	     = sm->pane_link.prev;
	struct cow_box		     frame	     = c->frame;
	if (c->root->layout.axis == COW_CONTAINER_LEAF)
		frame = cow_view_frame_box(old->active->view);

	wl_list_remove(&sm->pane_link);
	old->n_tabs--;
	if (old->active == sm)
		old->active = tm;
	sm->pane = leaf;
	wl_list_insert(&leaf->tabs, &sm->pane_link);
	leaf->n_tabs	     = 1;
	leaf->active	     = sm;
	pane_branch_insert(c, destination, &branch->layout, &leaf->layout, axis,
	    before);
	if (cow_container_arrange(c, frame))
		return true;

	/* Restore membership and topology atomically on a rejected split. */
	pane_replace(c, &branch->layout, destination);
	wl_list_remove(&sm->pane_link);
	wl_list_insert(previous, &sm->pane_link);
	sm->pane = old;
	old->n_tabs++;
	old->active = previous_active;
	container_pane_destroy(leaf);
	xfree(branch);
	container_title_metrics(c);
	return false;
}

void
cow_container_activate(struct cow_container *c,
    struct cow_container_member		    *member)
{
	if (c->frame_view && c->frame_view->container_member)
		cow_container_activate(cow_view_container(c->frame_view),
		    c->frame_view->container_member);
	struct cow_container_member *other;
	wl_list_for_each(other, &c->members, link)
	{
		if (other != member && other->view->fullscreen)
			cow_view_exit_fullscreen(other->view);
	}
	if (zoom_hides_pane(c->shaded ? NULL : zoomed_pane(c), member->pane)) {
		clear_zoom(c);
		container_apply_layout(c);
	}
	if (c->active == member && member->pane->active == member)
		return;

	/* Deactivate the current active member.
	 * Do NOT destroy the decoration here: the old surface must remain
	 * alive until the render cycle so that window_interaction events
	 * from rapid clicks are still routed to a valid decoration surface.
	 * The render cycle's deferred cleanup (cow_view_apply_render) will
	 * destroy it once the new active member's decoration is ready. */
	if (member->pane->active != NULL && member->pane->active != member) {
		struct cow_view *old = member->pane->active->view;
		old->use_ssd	     = false;
		old->dec.dirty	     = true;
		old->needs_raise     = false;
	}

	c->active		  = member;
	member->pane->active	  = member;
	member->view->use_ssd	  = true;
	member->view->dec.dirty	  = true;
	member->view->needs_raise = true;
	container_title_metrics(c);

	/*
	 * Re-sync the new active member's geometry to the container's
	 * authoritative position.  Without this, a member whose vx/vy
	 * was set on a different page (e.g. the container was scrolled
	 * to another page after the member joined) would be deemed
	 * off-page by cow_view_visible() and silently hidden, making
	 * the tab click appear to do nothing while the user-visible
	 * focus appears to land on the wrong window.
	 */
	struct cow_view *nv = member->view;
	if (cow_container_is_split(nv))
		cow_container_arrange(c, c->frame);
	if (nv->output != NULL && nv->desk != NULL) {
		struct cow_box box = cow_container_is_split(nv) ?
		    container_pane_content(member) :
		    (struct cow_box) { .x = c->x, .y = c->y };
		nv->x		   = box.x;
		nv->y		   = box.y;
		cow_view_sync_vxy_from_global(nv);
	}

	if (cow_view_is_iconified(nv)) {
		if (nv->output != NULL)
			cow_view_layout_icons_on_output(nv->output);
	} else {
		struct cow_container *child = cow_container_window_owner(nv);
		cow_wm.pending_focus	    = child && child->active ?
			   child->active->view :
			   nv;
	}
	cow_update_request(COW_UPDATE_MANAGE);
}

static void
container_redraw(struct cow_container *c)
{
	if (c->active != NULL && c->active->view->use_ssd)
		c->active->view->dec.dirty = true;
	cow_update_request(COW_UPDATE_MANAGE);
}

void
cow_container_swap_next(struct cow_container *c)
{
	if (c->active == NULL)
		return;
	struct wl_list *cur  = &c->active->pane_link;
	struct wl_list *next = cur->next;
	if (next == &c->active->pane->tabs)
		return; /* already the last tab */

	/* Remove cur from its current position then re-insert after next */
	wl_list_remove(cur);
	wl_list_insert(next, cur);
	struct cow_container_member *sibling = wl_container_of(next, sibling,
	    pane_link);
	wl_list_remove(&c->active->link);
	wl_list_insert(&sibling->link, &c->active->link);
	container_redraw(c);
}

void
cow_container_swap_prev(struct cow_container *c)
{
	if (c->active == NULL)
		return;
	struct wl_list *cur  = &c->active->pane_link;
	struct wl_list *prev = cur->prev;
	if (prev == &c->active->pane->tabs)
		return; /* already the first tab */

	/* Remove cur then re-insert before prev (i.e. after prev->prev) */
	struct wl_list *before_prev = prev->prev;
	wl_list_remove(cur);
	wl_list_insert(before_prev, cur);
	struct cow_container_member *sibling = wl_container_of(prev, sibling,
	    pane_link);
	wl_list_remove(&c->active->link);
	wl_list_insert(sibling->link.prev, &c->active->link);
	container_redraw(c);
}

static void
container_cycle(struct cow_container *c, bool forward)
{
	struct cow_container_pane *pane = c->active != NULL ? c->active->pane :
							      c->root;
	struct wl_list *link = pane->active != NULL ? &pane->active->pane_link :
						      &pane->tabs;
	struct cow_container_member *member;

	if (wl_list_empty(&pane->tabs))
		return;
	link = forward ? link->next : link->prev;
	if (link == &pane->tabs)
		link = forward ? link->next : link->prev;
	member = wl_container_of(link, member, pane_link);
	cow_container_activate(c, member);
}

void
cow_container_activate_next(struct cow_container *c)
{
	container_cycle(c, true);
}

void
cow_container_activate_prev(struct cow_container *c)
{
	container_cycle(c, false);
}

static void
container_became_empty(struct cow_container *c)
{
	c->active		= NULL;
	c->shaded		= false;
	c->maximized_h = c->maximized_v = false;
	c->flags &= ~COW_VIEW_ICONIFIED_FLAG;
}

void
cow_container_remove_view(struct cow_view *view)
{
	struct cow_container_member *m = view->container_member;
	if (m == NULL)
		return;
	if (view->container_placeholder) {
		char *name;
		xasprintf(&name, "detached-%d", next_container_id);
		struct cow_container *empty = cow_container_create_empty(name,
		    m->pane->name ? m->pane->name : "pane");
		xfree(name);
		if (empty) {
			empty->frame  = m->pane->layout.box;
			empty->output = view->output;
			empty->desk   = view->desk;
			cow_container_remove_empty_pane(m->container, m->pane);
		}
		return;
	}

	struct cow_container	  *c	      = m->container;
	bool			   was_active = (c->active == m);
	bool			   was_split  = cow_container_is_split(view);

	member_detach(m);
	int saved_pre_x	     = m->pre_x;
	int saved_pre_y	     = m->pre_y;
	int saved_pre_width  = m->pre_width;
	int saved_pre_height = m->pre_height;
	xfree(m);
	m = NULL;

	/* Restore independent decoration */
	view->use_ssd = true;
	cow_decoration_fini(&view->dec);
	cow_decoration_init(view);

	/* Restore pre-container geometry */
	if (saved_pre_width > 0 && saved_pre_height > 0) {
		view->x = saved_pre_x;
		view->y = saved_pre_y;
		cow_view_set_pending_geometry(view, saved_pre_x, saved_pre_y,
		    saved_pre_width, saved_pre_height);
	} else {
		view->x = c->x + 30;
		view->y = c->y + 30;
	}
	view->dec.dirty = true;
	if (was_split)
		cow_view_set_tiled_edges(view, RIVER_WINDOW_V1_EDGES_NONE);
	if (was_split && c->n_members > 0)
		cow_container_arrange(c, c->frame);

	if (c->persistent && c->n_members == 0) {
		container_became_empty(c);
	} else if (c->n_members == 0 ||
	    (c->n_members == 1 && c->name == NULL && !c->persistent &&
		(!c->frame_view || !c->frame_view->container_member))) {
		cow_container_destroy(c);
	} else if (was_active) {
		/* Activate the first remaining member */
		struct cow_container_member *next =
		    wl_container_of(c->members.next, next, link);
		c->active = NULL; /* clear first so activate doesn't skip */
		cow_container_activate(c, next);
	}

	cow_update_request(COW_UPDATE_MANAGE);
}

void
cow_container_destroy(struct cow_container *c)
{
	cow_container_window_destroy(c);
	struct cow_container_member *m, *tmp;
	wl_list_for_each_safe(m, tmp, &c->members, link)
	{
		m->view->use_ssd = true;
		member_unlink(m);
		m->view->pending_clip = true;
		m->view->clip_x = m->view->clip_y = 0;
		m->view->clip_w = m->view->clip_h = 0;
		m->view->dec.dirty = true;
		cow_view_set_tiled_edges(m->view, RIVER_WINDOW_V1_EDGES_NONE);
		xfree(m);
	}
	wl_list_remove(&c->wm_link);
	container_pane_destroy(c->root);
	xfree(c->name);
	xfree(c->restore_identifier);
	xfree(c);
}

/*
 * Detach 'view' from its container without cascading (callers are
 * tearing the view down).  Frees the container if this was the last
 * member; otherwise re-activates a sibling and marks it for repaint.
 */
void
cow_container_forget_view(struct cow_view *view)
{
	if (view->container_member == NULL)
		return;
	struct cow_container_member *m		= view->container_member;
	struct cow_container	    *c		= m->container;
	bool			     was_active = (c->active == m);
	bool			     was_split	= cow_container_is_split(view);
	member_detach(m);
	xfree(m);
	if (c->n_members == 0) {
		if (c->persistent) {
			container_became_empty(c);
			cow_update_request(
			    COW_UPDATE_MANAGE | COW_UPDATE_STATUS);
			return;
		}
		cow_container_destroy(c);
		return;
	} else if (was_active) {
		c->active = wl_container_of(c->members.next, c->active, link);
		c->active->view->use_ssd   = true;
		c->active->view->dec.dirty = true;
	}
	container_title_metrics(c);
	if (was_split)
		cow_container_arrange(c, c->frame);
	/* Keep named or persistent groups available for later additions. */
	if (c->n_members == 1 && c->name == NULL && !c->persistent &&
	    (!c->frame_view || !c->frame_view->container_member))
		cow_container_destroy(c);
	cow_update_request(COW_UPDATE_MANAGE | COW_UPDATE_STATUS);
}

struct cow_container *
cow_view_container(const struct cow_view *view)
{
	if (view->container_member == NULL)
		return NULL;
	return view->container_member->container;
}

struct cow_view *
cow_container_ancestor(struct cow_view *view, unsigned *depth)
{
	for (unsigned i = 0; view != NULL && i < 64; i++, view = view->parent) {
		if (!view->closed && view->container_member != NULL) {
			if (depth != NULL)
				*depth = i;
			return view;
		}
	}
	return NULL;
}

bool
cow_view_is_active_tab(const struct cow_view *view)
{
	if (view->container_member == NULL)
		return true; /* uncontained views are always "active" */
	return view->container_member->pane->active == view->container_member;
}

bool
cow_container_hides_view(const struct cow_view *view)
{
	struct cow_container *c = cow_view_container(view);
	if (view->container_placeholder && c == NULL)
		return true;
	if (c && c->frame_view &&
	    (c->frame_view->closed || c->frame_view->sd != COW_SHADE_NONE ||
		!cow_view_visible(c->frame_view, c->frame_view->output)))
		return true;
	if (c == NULL) {
		struct cow_view *parent = cow_container_ancestor(view->parent,
		    NULL);
		return parent != NULL &&
		    (cow_view_is_iconified(parent) ||
			cow_container_hides_view(parent));
	}
	if (!cow_view_is_active_tab(view))
		return true;
	if (zoom_hides_pane(c->shaded ? NULL : zoomed_pane(c),
		cow_view_pane(view)))
		return true;
	struct cow_container_member *m;
	wl_list_for_each(m, &c->members, link) {
		if (m->view != view && m->view->fullscreen)
			return true;
	}
	return false;
}

struct cow_container_pane *
cow_view_pane(const struct cow_view *view)
{
	return view->container_member != NULL ? view->container_member->pane :
						NULL;
}

void
cow_view_sync_container_pos(struct cow_view *view)
{
	if (view->container_member == NULL)
		return;

	struct cow_container	    *c = view->container_member->container;
	struct cow_container_member *m;

	if (cow_container_is_split(view)) {
		struct cow_box old	 = container_pane_content(view->container_member);
		struct cow_box requested = old, frame;
		requested.x		 = view->x;
		requested.y		 = view->y;
		if (!container_frame_delta(c->frame, old, requested, &frame) ||
		    !cow_container_layout_arrange(&c->root->layout, frame, 0))
			return;
		c->frame = frame;
		cow_container_window_geometry(c);
		wl_list_for_each(m, &c->members, link)
		{
			struct cow_box box = container_pane_content(m);
			m->view->x	   = box.x;
			m->view->y	   = box.y;
			if (m->view->pending_resize) {
				m->view->pending_x = box.x;
				m->view->pending_y = box.y;
			}
			if (m->view->deferred_position) {
				m->view->deferred_x = box.x;
				m->view->deferred_y = box.y;
			}
			cow_view_sync_vxy_from_global(m->view);
			m->view->dec.dirty = true;
		}
		cow_update_request(COW_UPDATE_MANAGE | COW_UPDATE_STATUS);
		return;
	}

	c->x = view->x;
	c->y = view->y;

	wl_list_for_each(m, &c->members, link)
	{
		if (m->view == view)
			continue;
		m->view->x = c->x;
		m->view->y = c->y;

		/*
		 * Keep the sibling's virtual (vx/vy) coords in step with
		 * its global ones.  Without this, cow_view_apply_render
		 * later recomputes x/y from the stale vx/vy
		 */
		cow_view_sync_vxy_from_global(m->view);
		m->view->dec.dirty = true;
	}
}

void
cow_view_sync_container_geometry(struct cow_view *view, int x, int y, int w,
    int h)
{
	if (view->container_member == NULL)
		return;

	struct cow_container	    *c = view->container_member->container;
	struct cow_container_member *m;

	if (cow_container_is_split(view)) {
		struct cow_box old	 = container_pane_content(view->container_member);
		struct cow_box requested = { x, y, w, h }, frame;
		if (!container_frame_delta(c->frame, old, requested, &frame) ||
		    !cow_container_arrange(c, frame))
			container_apply_layout(c);
		return;
	}

	c->x		  = x;
	c->y		  = y;
	c->content_width  = w;
	c->content_height = h;

	wl_list_for_each(m, &c->members, link)
	{
		if (m->view == view)
			continue;
		m->view->x = x;
		m->view->y = y;

		cow_view_sync_vxy_from_global(m->view);
		m->view->dec.dirty = true;
		cow_view_set_pending_geometry(m->view, x, y, w, h);
	}
}


int
container_allocate_id(void)
{
	return next_container_id++;
}
