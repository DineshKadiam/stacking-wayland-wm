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
	struct cow_container_member *m;
	wl_list_for_each(m, &pane->tabs, pane_link)
	{
		if (m->view->closed || m->view->identifier == NULL)
			continue;
		cJSON *tab = cJSON_CreateObject();
		cJSON_AddStringToObject(tab, "id", m->view->identifier);
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
	cJSON		      *record = cJSON_CreateObject();
	struct cow_box	       frame  = c->frame;
	const struct cow_view *active = c->active->view;

	if (c->root->layout.axis == COW_CONTAINER_LEAF) {
		/* Preserve full content dimensions when the frame is shaded. */
		int cx, cy;
		cow_titlebar_frame_geometry(active, active->content_width,
		    active->content_height, &frame.width, &frame.height, &cx,
		    &cy);
		frame.x = active->x - cx;
		frame.y = active->y - cy;
	} else {
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
	cow_box_json_add(cJSON_AddObjectToObject(record, "frame"), &frame);
	if (active->identifier != NULL)
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
	unsigned	      remaining;
	bool		      valid;
};

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
	const cJSON *axis     = cJSON_GetObjectItemCaseSensitive(node, "axis");
	const cJSON *tabs     = cJSON_GetObjectItemCaseSensitive(node, "tabs");
	const cJSON *children = cJSON_GetObjectItemCaseSensitive(node,
	    "children");

	if (axis != NULL) {
		int share;
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
		if (view == NULL)
			continue;
		if (view->container_member != NULL)
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
	if (pane->n_tabs == 0) {
		xfree(pane);
		return NULL;
	}
	return pane;

invalid:
	state->valid = false;
	container_pane_destroy(pane);
	return NULL;
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
	struct container_restore state = { .container = c,
		.remaining			      = 4096,
		.valid				      = true };
	c->root			       = pane_restore(&state,
				   cJSON_GetObjectItemCaseSensitive(record, "tree"), 0);
	const cJSON *active = cJSON_GetObjectItemCaseSensitive(record,
	    "active");
	if (!state.valid || c->root == NULL ||
	    (active != NULL && !cJSON_IsString(active)))
		goto fail;

	struct cow_container_member *m;
	wl_list_for_each(m, &c->members, link)
	{
		if (c->active == NULL)
			c->active = m->pane->active;
		if (active != NULL && m == m->pane->active &&
		    strcmp(active->valuestring, m->view->identifier) == 0)
			c->active = m;
	}
	c->output = c->active->view->output;
	c->desk	  = c->active->view->desk;
	c->flags  = c->active->view->flags;
	wl_list_for_each(m, &c->members, link)
	{
		if (m->view->output != c->output || m->view->desk != c->desk)
			goto fail;
	}
	/* Flat tab groups historically permit clients below their size hints.
	 */
	if (!container_pane_minimum(c->root,
		c->root->layout.axis != COW_CONTAINER_LEAF) ||
	    !cow_container_layout_arrange(&c->root->layout, frame, 0))
		goto fail;

	/* Publish only after the complete record and layout have validated. */
	c->frame = frame;
	c->id	 = container_allocate_id();
	wl_list_insert(&cow_wm.containers, &c->wm_link);
	wl_list_for_each(m, &c->members, link) m->view->container_member = m;
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
	xfree(c);
	return NULL;
}
