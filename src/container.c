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
pane_prune(struct cow_container *c, struct cow_container_pane *pane)
{
	struct cow_container_layout *parent = pane->layout.parent;

	if (pane->n_tabs != 0 || parent == NULL)
		return;
	struct cow_container_layout *sibling =
	    parent->child[parent->child[0] == &pane->layout];
	pane_replace(c, parent, sibling);
	xfree(container_pane_from_layout(parent));
	xfree(pane);
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
		cow_titlebar_frame_geometry(view, 0, 0, &fw, &fh, &cx, &cy);
		if (fw < 0 || fh < 0 || w > INT_MAX - fw || h > INT_MAX - fh)
			return false;
		if (w + fw > minw)
			minw = w + fw;
		if (h + fh > minh)
			minh = h + fh;
	}
	pane->layout.min_width	= minw;
	pane->layout.min_height = minh;
	return pane->active != NULL;
}

struct cow_box
container_pane_content(const struct cow_container_member *m)
{
	struct cow_box box = m->pane->layout.box;
	int	       fw, fh, cx, cy;

	cow_titlebar_frame_geometry(m->view, 0, 0, &fw, &fh, &cx, &cy);
	box.x += cx;
	box.y += cy;
	box.width -= fw;
	box.height -= fh;
	return box;
}

void
container_apply_layout(struct cow_container *c)
{
	struct cow_container_member *m;
	bool split = c->root->layout.axis != COW_CONTAINER_LEAF;

	wl_list_for_each(m, &c->members, link)
	{
		struct cow_box box = container_pane_content(m);
		m->view->use_ssd   = m->pane->active == m;
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
cow_container_arrange(struct cow_container *c, struct cow_box frame)
{
	if (!container_pane_minimum(c->root, true) ||
	    !cow_container_layout_arrange(&c->root->layout, frame, 0))
		return false;
	c->frame = frame;
	container_apply_layout(c);
	return true;
}

bool
cow_container_is_split(const struct cow_view *view)
{
	struct cow_container *c = cow_view_container(view);

	return c != NULL && c->root->layout.axis != COW_CONTAINER_LEAF;
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
	if (pane->active == m) {
		pane->active = NULL;
		if (!wl_list_empty(&pane->tabs))
			pane->active = wl_container_of(pane->tabs.next,
			    pane->active, pane_link);
	}
	m->view->container_member = NULL;
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
	cow_container_add(c, second);

	/* first (the drop target) is the active tab */
	c->active = wl_container_of(c->members.next, c->active, link);

	cow_update_request(COW_UPDATE_MANAGE);
	return c;
}

static bool
container_add(struct cow_container *c, struct cow_container_pane *pane,
    struct cow_view *view)
{
	if (view->container_member != NULL)
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
	if (c->root->layout.axis != COW_CONTAINER_LEAF &&
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

	/* Non-active members must not draw their own frame */
	if (m->pane->active != m) {
		view->use_ssd = false;
		cow_view_drop_decoration(view);
	}

	/* Sync dimensions to container */
	if (c->root->layout.axis == COW_CONTAINER_LEAF)
		cow_view_set_pending_geometry(view, c->x, c->y,
		    c->content_width, c->content_height);
	else
		container_apply_layout(c);
	return true;
}

bool
cow_container_add(struct cow_container *c, struct cow_view *view)
{
	return container_add(c, c->active != NULL ? c->active->pane : c->root,
	    view);
}

bool
cow_container_add_to(struct cow_view *target, struct cow_view *source)
{
	struct cow_container_member *m = target->container_member;

	return m != NULL && container_add(m->container, m->pane, source);
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

	unsigned depth = 0;
	for (struct cow_container_layout *n = &tm->pane->layout; n != NULL;
	    n				    = n->parent) {
		if (++depth >= 64)
			return false;
	}

	struct cow_container	    *c		     = tm->container;
	struct cow_container_pane   *old	     = tm->pane;
	struct cow_container_pane   *leaf	     = container_pane_create();
	struct cow_container_pane   *branch	     = container_pane_create();
	struct cow_container_member *previous_active = old->active;
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
	branch->layout.axis  = axis;
	branch->layout.share = 5000;
	pane_replace(c, &old->layout, &branch->layout);
	branch->layout.child[before ? 0 : 1] = &leaf->layout;
	branch->layout.child[before ? 1 : 0] = &old->layout;
	old->layout.parent = leaf->layout.parent = &branch->layout;
	if (cow_container_arrange(c, frame))
		return true;

	/* Restore membership and topology atomically on a rejected split. */
	pane_replace(c, &branch->layout, &old->layout);
	wl_list_remove(&sm->pane_link);
	wl_list_insert(previous, &sm->pane_link);
	sm->pane = old;
	old->n_tabs++;
	old->active = previous_active;
	xfree(leaf);
	xfree(branch);
	return false;
}

void
cow_container_activate(struct cow_container *c,
    struct cow_container_member		    *member)
{
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
		nv->vx = (nv->x - nv->output->x) + nv->desk->page_vx;
		nv->vy = (nv->y - nv->output->y) + nv->desk->page_vy;
	}

	cow_wm.pending_focus = member->view;
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

void
cow_container_remove_view(struct cow_view *view)
{
	struct cow_container_member *m = view->container_member;
	if (m == NULL)
		return;

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

	if (c->n_members <= 1) {
		/* Disband: release the last member too */
		if (!wl_list_empty(&c->members)) {
			struct cow_container_member *last =
			    wl_container_of(c->members.next, last, link);
			struct cow_view *lv = last->view;
			member_unlink(last);
			xfree(last);
			lv->use_ssd = true;
			cow_decoration_fini(&lv->dec);
			cow_decoration_init(lv);
			lv->dec.dirty = true;
		}
		wl_list_remove(&c->wm_link);
		container_pane_destroy(c->root);
		xfree(c);
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
	struct cow_container_member *m, *tmp;
	wl_list_for_each_safe(m, tmp, &c->members, link)
	{
		m->view->use_ssd = true;
		member_unlink(m);
		xfree(m);
	}
	wl_list_remove(&c->wm_link);
	container_pane_destroy(c->root);
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
	struct cow_container_pane   *pane	= m->pane;
	member_unlink(m);
	if (pane->active != NULL) {
		pane->active->view->use_ssd   = true;
		pane->active->view->dec.dirty = true;
	}
	pane_prune(c, pane);
	xfree(m);
	if (c->n_members == 0) {
		wl_list_remove(&c->wm_link);
		container_pane_destroy(c->root);
		xfree(c);
		return;
	} else if (was_active) {
		c->active = wl_container_of(c->members.next, c->active, link);
		c->active->view->use_ssd   = true;
		c->active->view->dec.dirty = true;
	}
	if (was_split)
		cow_container_arrange(c, c->frame);
}

struct cow_container *
cow_view_container(const struct cow_view *view)
{
	if (view->container_member == NULL)
		return NULL;
	return view->container_member->container;
}

bool
cow_view_is_active_tab(const struct cow_view *view)
{
	if (view->container_member == NULL)
		return true; /* uncontained views are always "active" */
	return view->container_member->pane->active == view->container_member;
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
