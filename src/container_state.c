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
#include <limits.h>
#include <string.h>

#include "cow/view.h"
#include "cow/memory.h"
#include "cow/output.h"
#include "cow/titlebar.h"
#include "cow/update.h"
#include "cow/wm.h"
#include "container_internal.h"

static cJSON *
pane_save(const struct cow_container_pane *pane)
{
	cJSON *node = cJSON_CreateObject();

	if (pane->layout.axis != COW_CONTAINER_LEAF) {
		cJSON_AddStringToObject(node, "axis",
		    pane->layout.axis == COW_CONTAINER_HORIZONTAL ?
			"horizontal" :
			"vertical");
		cJSON_AddNumberToObject(node, "share", pane->layout.share);
		cJSON *children = cJSON_AddArrayToObject(node, "children");
		cJSON_AddItemToArray(children,
		    pane_save(
			container_pane_from_layout(pane->layout.child[0])));
		cJSON_AddItemToArray(children,
		    pane_save(
			container_pane_from_layout(pane->layout.child[1])));
		return node;
	}

	cJSON *tabs = cJSON_AddArrayToObject(node, "tabs");
	if (pane->name != NULL)
		cJSON_AddStringToObject(node, "name", pane->name);
	cJSON_AddNumberToObject(node, "collapsed", pane->layout.collapsed);
	cJSON_AddBoolToObject(node, "title_hidden", pane->title_hidden);
	cJSON_AddBoolToObject(node, "hidden", pane->layout.hidden);
	cJSON_AddNumberToObject(node, "zoom", pane->zoom);
	struct cow_container_member *m;
	wl_list_for_each(m, &pane->tabs, pane_link)
	{
		if (m->view->closed || m->view->container_placeholder ||
		    m->view->identifier == NULL)
			continue;
		cJSON *tab = cJSON_CreateObject();
		cJSON_AddStringToObject(tab, "id", m->view->identifier);
		if (cow_container_window_owner(m->view))
			cJSON_AddBoolToObject(tab, "frame", true);
		struct cow_box restore = { m->pre_x, m->pre_y, m->pre_width,
			m->pre_height };
		cow_box_json_add(cJSON_AddObjectToObject(tab, "restore"),
		    &restore);
		cJSON_AddItemToArray(tabs, tab);
		if (m == pane->active)
			cJSON_AddStringToObject(node, "active",
			    m->view->identifier);
	}
	return node;
}

cJSON *
cow_container_save(const struct cow_container *c)
{
	cJSON *record = cJSON_CreateObject();
	if (c->name != NULL)
		cJSON_AddStringToObject(record, "name", c->name);
	struct cow_box	       frame  = c->frame;
	const struct cow_view *active = c->active ? c->active->view : NULL;

	if (!c->framed && active != NULL &&
	    c->root->layout.axis == COW_CONTAINER_LEAF) {
		/* Preserve full content dimensions when the frame is shaded. */
		int cx, cy;
		cow_titlebar_frame_geometry(active, active->content_width,
		    active->content_height, &frame.width, &frame.height, &cx,
		    &cy);
		frame.x = active->x - cx;
		frame.y = active->y - cy;
	} else if (active != NULL && !active->fullscreen) {
		/* Account for viewport movement since the last layout. */
		struct cow_box old     = container_pane_content(c->active),
			       current = old;
		current.x = active->pending_resize ? active->pending_x :
		    active->deferred_position	   ? active->deferred_x :
						     active->x;
		current.y = active->pending_resize ? active->pending_y :
		    active->deferred_position	   ? active->deferred_y :
						     active->y;
		container_frame_delta(frame, old, current, &frame);
	}
	cJSON_AddNumberToObject(record, "version", 1);
	cJSON_AddBoolToObject(record, "persistent", c->persistent);
	cJSON_AddBoolToObject(record, "framed", c->framed);
	if (c->frame_view) {
		const struct cow_view *v = c->frame_view;
		if (v->identifier)
			cJSON_AddStringToObject(record, "frame_id",
			    v->identifier);
		cJSON *state = cJSON_AddObjectToObject(record, "frame_state");
		struct cow_box restore = { v->saved_x, v->saved_y,
			v->saved_width, v->saved_height };
		cow_box_json_add(cJSON_AddObjectToObject(state, "restore"),
		    &restore);
		cJSON_AddNumberToObject(state, "flags", v->flags);
		cJSON_AddNumberToObject(state, "layer", v->layer);
		cJSON_AddNumberToObject(state, "shade", v->sd);
		cJSON_AddBoolToObject(state, "maximized_h", v->maximized_h);
		cJSON_AddBoolToObject(state, "maximized_v", v->maximized_v);
		cJSON_AddBoolToObject(state, "ignore_h",
		    v->maximized_h_ignore_reserved);
		cJSON_AddBoolToObject(state, "ignore_v",
		    v->maximized_v_ignore_reserved);
		cJSON_AddBoolToObject(state, "keep_h",
		    v->maximized_h_keep_geometry);
		cJSON_AddBoolToObject(state, "keep_v",
		    v->maximized_v_keep_geometry);
	}
	if (c->persistent && c->output != NULL && c->desk != NULL) {
		cJSON_AddStringToObject(record, "output", c->output->name);
		cJSON_AddNumberToObject(record, "desk", c->desk->nr);
	}
	cow_box_json_add(cJSON_AddObjectToObject(record, "restore_frame"),
	    &c->restore_frame);
	cJSON_AddBoolToObject(record, "maximized_h", c->maximized_h);
	cJSON_AddBoolToObject(record, "maximized_v", c->maximized_v);
	cJSON_AddBoolToObject(record, "shaded", c->shaded);
	cow_box_json_add(cJSON_AddObjectToObject(record, "frame"), &frame);
	if (active != NULL && active->identifier != NULL)
		cJSON_AddStringToObject(record, "active", active->identifier);
	cJSON_AddItemToObject(record, "tree", pane_save(c->root));
	return record;
}

/* Require exact, representable integers before using saved geometry. */
static bool
state_int(const cJSON *object, const char *key, int *result)
{
	const cJSON *value = cJSON_GetObjectItemCaseSensitive(object, key);

	if (!cJSON_IsNumber(value) || value->valuedouble != value->valueint)
		return false;
	*result = value->valueint;
	return true;
}

static bool
state_box(const cJSON *object, struct cow_box *box)
{
	return cJSON_IsObject(object) && state_int(object, "x", &box->x) &&
	    state_int(object, "y", &box->y) &&
	    state_int(object, "width", &box->width) &&
	    state_int(object, "height", &box->height) && box->width >= 0 &&
	    box->height >= 0 && (int64_t)box->x + box->width <= INT_MAX &&
	    (int64_t)box->y + box->height <= INT_MAX;
}

struct container_restore {
	struct cow_container *container;
	struct cow_container *replace;
	unsigned	      remaining;
	bool		      valid;
	unsigned	      zoom_count;
};

/* Native container windows are recreated on restart. Resolve saved tab
 * references through their layout, not through a vanished River id.
 */
static struct cow_container *
restore_frame(const char *id)
{
	struct cow_container *c;
	wl_list_for_each(c, &cow_wm.containers, wm_link)
	{
		if ((c->restore_identifier &&
			strcmp(c->restore_identifier, id) == 0) ||
		    (c->frame_view && c->frame_view->identifier &&
			strcmp(c->frame_view->identifier, id) == 0))
			return c;
	}
	return NULL;
}

static cJSON *pending_tab_groups;

bool
cow_container_restore_defer(const cJSON *record)
{
	const cJSON *tree = cJSON_GetObjectItemCaseSensitive(record, "tree");
	const cJSON *tabs = cJSON_GetObjectItemCaseSensitive(tree, "tabs"),
		    *tab;
	cJSON_ArrayForEach(tab, tabs)
	{
		if (!cJSON_IsTrue(
			cJSON_GetObjectItemCaseSensitive(tab, "frame")))
			continue;
		if (!pending_tab_groups)
			pending_tab_groups = cJSON_CreateArray();
		cJSON_AddItemToArray(pending_tab_groups,
		    cJSON_Duplicate(record, true));
		return true;
	}
	return false;
}

void
cow_container_restore_pending(void)
{
	for (int i = 0; i < cJSON_GetArraySize(pending_tab_groups);) {
		const cJSON *record = cJSON_GetArrayItem(pending_tab_groups, i);
		const cJSON *tree   = cJSON_GetObjectItemCaseSensitive(record,
		      "tree");
		const cJSON *tabs   = cJSON_GetObjectItemCaseSensitive(tree,
				  "tabs"),
			    *tab;
		bool waiting = false;
		cJSON_ArrayForEach(tab, tabs)
		{
			const cJSON *id = cJSON_GetObjectItemCaseSensitive(tab,
			    "id");
			if (!cJSON_IsString(id) ||
			    !cJSON_IsTrue(
				cJSON_GetObjectItemCaseSensitive(tab, "frame")))
				continue;
			struct cow_container *c = restore_frame(
			    id->valuestring);
			if (c &&
			    (!c->frame_view || !c->frame_view->has_dimensions ||
				c->frame_view->pending_resize ||
				c->frame_state.pending))
				waiting = true;
		}
		if (waiting) {
			i++;
			continue;
		}
		if (cow_container_restore(record))
			cow_update_request(
			    COW_UPDATE_MANAGE | COW_UPDATE_STATUS);
		cJSON_DeleteItemFromArray(pending_tab_groups, i);
	}
}

void
cow_container_restore_clear(void)
{
	cJSON_Delete(pending_tab_groups);
	pending_tab_groups = NULL;
}

static struct cow_container_pane *
pane_restore(struct container_restore *state, const cJSON *node, unsigned depth)
{
	if (!state->valid || !cJSON_IsObject(node) || depth >= 64 ||
	    state->remaining == 0) {
		state->valid = false;
		return NULL;
	}
	state->remaining--;
	struct cow_container_pane *pane = container_pane_create();
	const cJSON *hidden		= cJSON_GetObjectItemCaseSensitive(node,
			"title_hidden");
	if (hidden != NULL && !cJSON_IsBool(hidden))
		goto invalid;
	pane->title_hidden = cJSON_IsTrue(hidden);
	const cJSON *pane_hidden = cJSON_GetObjectItemCaseSensitive(node, "hidden");
	if (pane_hidden != NULL && !cJSON_IsBool(pane_hidden))
		goto invalid;
	pane->layout.hidden = cJSON_IsTrue(pane_hidden);
	const cJSON *zoom  = cJSON_GetObjectItemCaseSensitive(node, "zoom");
	if (zoom != NULL) {
		int value;
		if (!state_int(node, "zoom", &value) ||
		    value < COW_CONTAINER_ZOOM_NONE ||
		    value > COW_CONTAINER_ZOOM_ALL)
			goto invalid;
		pane->zoom = value;
		if (value != COW_CONTAINER_ZOOM_NONE && ++state->zoom_count > 1)
			goto invalid;
	}
	const cJSON *axis      = cJSON_GetObjectItemCaseSensitive(node, "axis");
	const cJSON *collapsed = cJSON_GetObjectItemCaseSensitive(node,
	    "collapsed");
	if (collapsed != NULL) {
		int value;
		if (!state_int(node, "collapsed", &value) || value < 0 ||
		    value > 2)
			goto invalid;
		pane->layout.collapsed = value;
	}
	const cJSON *tabs     = cJSON_GetObjectItemCaseSensitive(node, "tabs");
	const cJSON *children = cJSON_GetObjectItemCaseSensitive(node,
	    "children");

	if (axis != NULL) {
		int share;
		if (pane->zoom != COW_CONTAINER_ZOOM_NONE)
			goto invalid;
		if (!cJSON_IsString(axis) || tabs != NULL ||
		    !cJSON_IsArray(children) ||
		    cJSON_GetArraySize(children) != 2 ||
		    !state_int(node, "share", &share) || share < 1 ||
		    share > 9999)
			goto invalid;
		if (strcmp(axis->valuestring, "horizontal") == 0)
			pane->layout.axis = COW_CONTAINER_HORIZONTAL;
		else if (strcmp(axis->valuestring, "vertical") == 0)
			pane->layout.axis = COW_CONTAINER_VERTICAL;
		else
			goto invalid;
		pane->layout.share = share;
		for (int i = 0; i < 2; i++) {
			struct cow_container_pane *child = pane_restore(state,
			    cJSON_GetArrayItem(children, i), depth + 1);
			if (child != NULL) {
				pane->layout.child[i] = &child->layout;
				child->layout.parent  = &pane->layout;
			}
			if (!state->valid)
				goto invalid;
		}
		if (pane->layout.child[0] == NULL ||
		    pane->layout.child[1] == NULL) {
			struct cow_container_layout *survivor =
			    pane->layout.child[pane->layout.child[0] == NULL];
			xfree(pane);
			if (survivor == NULL)
				return NULL;
			survivor->parent = NULL;
			return container_pane_from_layout(survivor);
		}
		return pane;
	}

	const cJSON *active = cJSON_GetObjectItemCaseSensitive(node, "active");
	const cJSON *name   = cJSON_GetObjectItemCaseSensitive(node, "name");
	if (name != NULL) {
		if (!cJSON_IsString(name) || name->valuestring[0] == '\0' ||
		    cow_container_pane_find(state->container,
			name->valuestring) != NULL)
			goto invalid;
		pane->name = xstrdup(name->valuestring);
	}
	if (!cJSON_IsArray(tabs) || children != NULL ||
	    (active != NULL && !cJSON_IsString(active)))
		goto invalid;
	const cJSON *tab;
	cJSON_ArrayForEach(tab, tabs)
	{
		const cJSON   *id = cJSON_GetObjectItemCaseSensitive(tab, "id");
		struct cow_box restore;
		if (state->remaining == 0 || !cJSON_IsObject(tab) ||
		    !cJSON_IsString(id) ||
		    !state_box(cJSON_GetObjectItemCaseSensitive(tab, "restore"),
			&restore))
			goto invalid;
		state->remaining--;
		struct cow_view *view = cow_view_find_by_identifier(
		    id->valuestring);
		if (cJSON_IsTrue(
			cJSON_GetObjectItemCaseSensitive(tab, "frame"))) {
			struct cow_container *child = restore_frame(
			    id->valuestring);
			view = child ? child->frame_view : NULL;
		}
		if (view == NULL)
			continue;
		if (!cow_container_can_tab(state->container, view) ||
		    (cow_container_window_owner(view) && depth != 0))
			goto invalid;
		if (view->container_member != NULL &&
		    view->container_member->container != state->replace)
			goto invalid;
		struct cow_container_member *m;
		wl_list_for_each(m, &state->container->members, link)
		{
			if (m->view == view)
				goto invalid;
		}
		m	      = xcalloc(1, sizeof(*m));
		m->view	      = view;
		m->container  = state->container;
		m->pane	      = pane;
		m->pre_x      = restore.x;
		m->pre_y      = restore.y;
		m->pre_width  = restore.width;
		m->pre_height = restore.height;
		wl_list_insert(state->container->members.prev, &m->link);
		wl_list_insert(pane->tabs.prev, &m->pane_link);
		state->container->n_members++;
		pane->n_tabs++;
		if (pane->active == NULL ||
		    (active != NULL &&
			strcmp(active->valuestring, id->valuestring) == 0))
			pane->active = m;
	}
	if (pane->n_tabs == 0 && !state->container->persistent) {
		xfree(pane->name);
		xfree(pane);
		return NULL;
	}
	if (pane->n_tabs == 0)
		pane->layout.collapsed = COW_CONTAINER_LEAF;
	return pane;

invalid:
	state->valid = false;
	container_pane_destroy(pane);
	return NULL;
}

static bool
pane_collapse_valid(const struct cow_container_layout *node)
{
	if (node->axis == COW_CONTAINER_LEAF)
		return node->collapsed != COW_CONTAINER_HORIZONTAL;
	/* Rotation can put columns beneath rows. The decoder validates the
	 * split topology; only unsupported collapse states are rejected here.
	 */
	return pane_collapse_valid(node->child[0]) &&
	    pane_collapse_valid(node->child[1]);
}

struct cow_container *
cow_container_restore(const cJSON *record)
{
	int	       version;
	struct cow_box frame;
	if (!cJSON_IsObject(record) ||
	    !state_int(record, "version", &version) || version != 1 ||
	    !state_box(cJSON_GetObjectItemCaseSensitive(record, "frame"),
		&frame))
		return NULL;

	struct cow_container *c = xcalloc(1, sizeof(*c));
	wl_list_init(&c->members);
	struct cow_container *replace = NULL;
	const cJSON *persistent	      = cJSON_GetObjectItemCaseSensitive(record,
		  "persistent");
	if (persistent != NULL && !cJSON_IsBool(persistent))
		goto fail;
	c->persistent = cJSON_IsTrue(persistent);
	c->framed     = cJSON_IsTrue(
		cJSON_GetObjectItemCaseSensitive(record, "framed"));
	const cJSON *frame_id = cJSON_GetObjectItemCaseSensitive(record,
	    "frame_id");
	if (frame_id) {
		if (!cJSON_IsString(frame_id) || !*frame_id->valuestring)
			goto fail;
		c->restore_identifier = xstrdup(frame_id->valuestring);
	}
	const cJSON *frame_state = cJSON_GetObjectItemCaseSensitive(record,
	    "frame_state");
	if (frame_state) {
		struct cow_container_frame_state *s = &c->frame_state;
		int				  flags;
		if (!cJSON_IsObject(frame_state) ||
		    !state_box(cJSON_GetObjectItemCaseSensitive(frame_state,
				   "restore"),
			&s->restore) ||
		    !state_int(frame_state, "flags", &flags) || flags < 0 ||
		    !state_int(frame_state, "layer", &s->layer) ||
		    s->layer < 0 || s->layer > UINT8_MAX ||
		    !state_int(frame_state, "shade", &s->shade) ||
		    s->shade < COW_SHADE_NONE || s->shade >= COW_SHADE_ERROR)
			goto fail;
		s->flags       = (uint32_t)flags;
		s->maximized_h = cJSON_IsTrue(
		    cJSON_GetObjectItemCaseSensitive(frame_state,
			"maximized_h"));
		s->maximized_v = cJSON_IsTrue(
		    cJSON_GetObjectItemCaseSensitive(frame_state,
			"maximized_v"));
		s->ignore_h = cJSON_IsTrue(
		    cJSON_GetObjectItemCaseSensitive(frame_state, "ignore_h"));
		s->ignore_v = cJSON_IsTrue(
		    cJSON_GetObjectItemCaseSensitive(frame_state, "ignore_v"));
		s->keep_h = cJSON_IsTrue(
		    cJSON_GetObjectItemCaseSensitive(frame_state, "keep_h"));
		s->keep_v = cJSON_IsTrue(
		    cJSON_GetObjectItemCaseSensitive(frame_state, "keep_v"));
		if ((s->maximized_h && s->restore.width < 1) ||
		    (s->maximized_v && s->restore.height < 1))
			goto fail;
		s->pending = true;
	}
	const cJSON *name = cJSON_GetObjectItemCaseSensitive(record, "name");
	if (name != NULL) {
		if (!cJSON_IsString(name) || name->valuestring[0] == '\0')
			goto fail;
		replace = cow_container_find(name->valuestring);
		/* Config and mapping rules may already have populated this
		 * layout.
		 */
		if (replace != NULL && (!replace->persistent || !c->persistent))
			goto fail;
		c->name = xstrdup(name->valuestring);
	}
	struct container_restore state = { .container = c,
		.replace			      = replace,
		.remaining			      = 4096,
		.valid				      = true };
	c->root			       = pane_restore(&state,
				   cJSON_GetObjectItemCaseSensitive(record, "tree"), 0);
	const cJSON *active = cJSON_GetObjectItemCaseSensitive(record,
	    "active");
	if (!state.valid || c->root == NULL ||
	    cow_container_layout_hidden(&c->root->layout) ||
	    (c->n_members < 2 && c->name == NULL && !c->persistent &&
		!(c->framed && c->n_members == 1)) ||
	    !pane_collapse_valid(&c->root->layout) ||
	    (active != NULL && !cJSON_IsString(active)))
		goto fail;
	struct cow_container_pane *pane = NULL;
	while ((pane = cow_container_pane_next(c, pane)) != NULL) {
		if (pane->name != NULL &&
		    cow_container_pane_find(c, pane->name) != pane)
			goto fail;
	}

	struct cow_container_member *m;
	/* Do not discard clients added by rules but absent from saved state.
	 */
	if (replace != NULL) {
		struct cow_container_member *old;
		wl_list_for_each(old, &replace->members, link)
		{
			if (old->view->container_placeholder)
				continue;
			bool found = false;
			wl_list_for_each(m, &c->members, link)
			{
				if (m->view == old->view) {
					found = true;
					break;
				}
			}
			if (!found)
				goto fail;
		}
	}
	wl_list_for_each(m, &c->members, link)
	{
		if (c->active == NULL)
			c->active = m->pane->active;
		struct cow_container *child = cow_container_window_owner(
		    m->view);
		const char *id = child && child->restore_identifier ?
		    child->restore_identifier :
		    m->view->identifier;
		if (active != NULL && m == m->pane->active && id &&
		    strcmp(active->valuestring, id) == 0)
			c->active = m;
	}
	if (c->active != NULL) {
		c->output = c->active->view->output;
		c->desk	  = c->active->view->desk;
		c->flags  = c->active->view->flags;
	} else if (!c->persistent) {
		goto fail;
	} else {
		const cJSON *output = cJSON_GetObjectItemCaseSensitive(record,
		    "output");
		int	     desk;
		if (output != NULL) {
			if (!cJSON_IsString(output) ||
			    !state_int(record, "desk", &desk) || desk < 0 ||
			    desk > UINT8_MAX)
				goto fail;
			c->output = cow_output_find_by_name(
			    output->valuestring);
			if (c->output != NULL)
				c->desk = cow_desk_by_nr(c->output, desk);
		}
	}
	wl_list_for_each(m, &c->members, link)
	{
		if (m->view->output != c->output || m->view->desk != c->desk)
			goto fail;
	}
	/* Flat tab groups historically permit clients below their size hints.
	 */
	container_pane_normalize_columns(&c->root->layout);
	if ((c->n_members != 0 || (frame.width > 0 && frame.height > 0)) &&
	    (!container_pane_minimum(c->root,
		 c->root->layout.axis != COW_CONTAINER_LEAF) ||
		!cow_container_layout_arrange(&c->root->layout, frame, 0)))
		goto fail;

	c->shaded = c->n_members > 0 &&
	    c->root->layout.axis != COW_CONTAINER_LEAF &&
	    cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(record, "shaded"));
	/* Publish only after the complete record and layout have validated. */
	const cJSON *restore = cJSON_GetObjectItemCaseSensitive(record,
	    "restore_frame");
	if (restore != NULL && !state_box(restore, &c->restore_frame))
		goto fail;
	c->maximized_h = cJSON_IsTrue(
	    cJSON_GetObjectItemCaseSensitive(record, "maximized_h"));
	c->maximized_v = cJSON_IsTrue(
	    cJSON_GetObjectItemCaseSensitive(record, "maximized_v"));
	if ((c->maximized_h && c->restore_frame.width < 1) ||
	    (c->maximized_v && c->restore_frame.height < 1))
		goto fail;
	c->frame = frame;
	c->id	 = container_allocate_id();
	if (replace != NULL)
		cow_container_destroy(replace);
	wl_list_insert(&cow_wm.containers, &c->wm_link);
	wl_list_for_each(m, &c->members, link)
	{
		m->view->container_member = m;
	}
	container_apply_layout(c);
	return c;

fail:
	/* These members were never published; do not touch their live views. */
	{
		struct cow_container_member *m, *next;
		wl_list_for_each_safe(m, next, &c->members, link)
		{
			wl_list_remove(&m->link);
			xfree(m);
		}
	}
	container_pane_destroy(c->root);
	xfree(c->name);
	xfree(c->restore_identifier);
	xfree(c);
	return NULL;
}
