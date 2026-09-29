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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cow/state.h"
#include "cow/view.h"
#include "cow/wm.h"

#define CHECK(expr)                                                        \
	do {                                                               \
		if (!(expr)) {                                             \
			fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, \
			    #expr);                                        \
			abort();                                           \
		}                                                          \
	} while (0)

static struct cow_view a, b, d;

static void
init_view(struct cow_view *view, char *id)
{
	view->identifier	  = id;
	view->use_ssd		  = true;
	view->content_width	  = 800;
	view->content_height	  = 600;
	view->cfg.titlebar.height = view->dec.title_thickness = 20;
	view->dec.nominal_title_thickness		      = 20;
	view->cfg.titlebar.enabled			      = true;
	view->cfg.border = view->dec.border_width = 4;
	wl_list_init(&view->selections);
	wl_list_insert(cow_wm.views.prev, &view->wm_link);
}

static void
reject(const cJSON *record)
{
	a.pending_resize = b.pending_resize = d.pending_resize = false;
	CHECK(cow_container_restore(record) == NULL);
	CHECK(a.container_member == NULL && b.container_member == NULL &&
	    d.container_member == NULL);
	CHECK(!a.pending_resize && !b.pending_resize && !d.pending_resize);
	CHECK(wl_list_empty(&cow_wm.containers));
}

int
main(void)
{
	wl_list_init(&cow_wm.containers);
	wl_list_init(&cow_wm.views);
	wl_list_init(&cow_wm.outputs);
	wl_list_init(&cow_wm.seats);
	init_view(&a, "a");
	init_view(&b, "b");
	init_view(&d, "d");
	struct cow_container *c = cow_container_create(&a, &b);
	CHECK(cow_container_add(c, &d));
	CHECK(cow_container_split(&a, &d, COW_CONTAINER_HORIZONTAL, false));
	c->root->layout.share = 6300;
	CHECK(cow_container_arrange(c, c->frame));
	cow_container_activate(c, b.container_member);
	cow_container_swap_prev(c);
	CHECK(cow_container_title_hide(&d));
	cJSON *record = cow_container_save(c);

	/* Exercise the actual state-file entry points, not just the decoder. */
	char directory[] = "/tmp/cow-container-state.XXXXXX";
	CHECK(mkdtemp(directory) != NULL);
	CHECK(setenv("XDG_RUNTIME_DIR", directory, 1) == 0);
	cow_wm.focused_view = &b;
	CHECK(cow_state_save());
	cow_container_destroy(c);
	CHECK(cow_state_restore());
	c = cow_view_container(&a);
	CHECK(c != NULL && c->n_members == 3 && c->active->view == &b);
	CHECK(c->root->layout.share == 6300);
	CHECK(cow_view_pane(&a)->tabs.next == &b.container_member->pane_link);
	CHECK(cow_view_is_active_tab(&b) && cow_view_is_active_tab(&d));
	CHECK(!cow_view_is_active_tab(&a) && cow_wm.pending_focus == &b);
	CHECK(a.container_member->pre_width == 800);
	CHECK(cow_view_pane(&d)->title_hidden);
	CHECK(d.dec.title_thickness == 0);
	cJSON *roundtrip = cow_container_save(c);
	CHECK(cJSON_Compare(record, roundtrip, true));
	cJSON_Delete(roundtrip);

	/* Restart retains group iconification and a single representative. */
	a.cfg.icon_enabled = b.cfg.icon_enabled = d.cfg.icon_enabled = true;
	cow_view_iconify(&d);
	CHECK(cow_state_save());
	cow_container_destroy(c);
	a.flags &= ~COW_VIEW_ICONIFIED_FLAG;
	b.flags &= ~COW_VIEW_ICONIFIED_FLAG;
	d.flags &= ~COW_VIEW_ICONIFIED_FLAG;
	CHECK(cow_state_restore());
	c = cow_view_container(&a);
	CHECK(c != NULL && cow_view_is_iconified(&a) &&
	    cow_view_is_iconified(&b) && cow_view_is_iconified(&d));
	CHECK(!cow_view_has_icon(&a) && cow_view_has_icon(&b) &&
	    !cow_view_has_icon(&d));
	cow_view_deiconify(&b);
	cow_container_destroy(c);

	/* The selected window, a whole pane, or every client may disappear. */
	b.closed = true;
	c	 = cow_container_restore(record);
	CHECK(c != NULL && c->n_members == 2 && c->active->view == &a);
	CHECK(cow_view_is_active_tab(&a) && cow_view_is_active_tab(&d));
	cow_container_destroy(c);
	a.closed = true;
	/* A lone survivor must not regain container chrome on restart. */
	reject(record);
	d.closed = true;
	reject(record);
	a.closed = b.closed = d.closed = false;

	/* A record must never steal a view from an existing container. */
	c = cow_container_restore(record);
	CHECK(c != NULL);
	CHECK(cow_container_restore(record) == NULL);
	CHECK(cow_view_container(&a) == c && c->n_members == 3);
	cow_container_destroy(c);

	const struct {
		const char *parent, *key;
		double value;
	} invalid_numbers[] = {
		{ "tree", "share", 0 },
		{ "tree", "share", 5000.5 },
		{ "frame", "width", 1e100 },
		{ NULL, "version", 2 },
		{ "frame", "width", 1 },
	};
	for (size_t i = 0; i < sizeof(invalid_numbers) / sizeof(*invalid_numbers); i++) {
		cJSON *bad = cJSON_Duplicate(record, true);
		cJSON *parent = invalid_numbers[i].parent ?
		    cJSON_GetObjectItem(bad, invalid_numbers[i].parent) : bad;
		cJSON_SetNumberValue(cJSON_GetObjectItem(parent,
		    invalid_numbers[i].key), invalid_numbers[i].value);
		reject(bad);
		cJSON_Delete(bad);
	}

	/* A view cannot belong to two panes. */
	cJSON *bad = cJSON_Duplicate(record, true);
	cJSON *right = cJSON_GetArrayItem(cJSON_GetObjectItem(
	    cJSON_GetObjectItem(bad, "tree"), "children"), 1);
	cJSON *tab = cJSON_GetArrayItem(cJSON_GetObjectItem(right, "tabs"), 0);
	cJSON_ReplaceItemInObject(tab, "id", cJSON_CreateString("a"));
	reject(bad);
	cJSON_Delete(bad);

	/* Bound recursion even for a syntactically valid JSON tree. */
	cJSON *deep = cJSON_Duplicate(record, true);
	cJSON *tree = cJSON_DetachItemFromObject(deep, "tree");
	for (int i = 0; i < 64; i++) {
		cJSON *parent = cJSON_CreateObject();
		cJSON_AddStringToObject(parent, "axis", "horizontal");
		cJSON_AddNumberToObject(parent, "share", 5000);
		cJSON *children = cJSON_AddArrayToObject(parent, "children");
		cJSON_AddItemToArray(children, tree);
		cJSON *empty = cJSON_CreateObject();
		cJSON_AddArrayToObject(empty, "tabs");
		cJSON_AddItemToArray(children, empty);
		tree = parent;
	}
	cJSON_AddItemToObject(deep, "tree", tree);
	reject(deep);
	cJSON_Delete(deep);

	/* Detach still restores the original floating size after restart. */
	c = cow_container_restore(record);
	CHECK(c != NULL);
	cow_container_remove_view(&b);
	CHECK(b.pending_width == 800 && b.pending_height == 600);
	cow_container_destroy(c);
	cJSON_Delete(record);

	/* Nested branches survive a round trip, and prune independently. */
	c = cow_container_create(&a, &b);
	CHECK(cow_container_add(c, &d));
	CHECK(cow_container_split(&a, &d, COW_CONTAINER_HORIZONTAL, false));
	CHECK(cow_container_split(&a, &b, COW_CONTAINER_VERTICAL, true));
	CHECK(cow_container_collapse(&b));
	CHECK(cow_container_shade(&a, true));
	record = cow_container_save(c);
	cow_container_destroy(c);
	c = cow_container_restore(record);
	CHECK(c != NULL);
	CHECK(cow_view_pane(&b)->layout.collapsed == COW_CONTAINER_VERTICAL);
	CHECK(c->shaded);
	roundtrip = cow_container_save(c);
	CHECK(cJSON_Compare(record, roundtrip, true));
	cJSON_Delete(roundtrip);
	cow_container_destroy(c);
	b.closed = true;
	c	 = cow_container_restore(record);
	CHECK(c != NULL && c->n_members == 2);
	CHECK(cow_view_pane(&a)->layout.parent == &c->root->layout);
	cow_container_destroy(c);
	b.closed    = false;
	a.min_width = 10000;
	reject(record);
	a.min_width = 0;
	cJSON_Delete(record);

	/* Shading and restrictive inactive-tab hints must not break flat
	 * groups. */
	a.content_width = a.content_height = 40;
	a.sd				   = COW_SHADE_NORTH;
	b.min_width			   = 2000;
	c				   = cow_container_create(&a, &b);
	record				   = cow_container_save(c);
	cow_container_destroy(c);
	c = cow_container_restore(record);
	CHECK(c != NULL && a.pending_width == 40 && a.pending_height == 40);
	CHECK(b.pending_width == 40);
	cow_container_destroy(c);
	cJSON_Delete(record);
	a.sd		 = COW_SHADE_NONE;
	a.content_width	 = 800;
	a.content_height = 600;
	b.min_width	 = 0;

	/* Accept the former per-window flat membership format. */
	char path[512];
	CHECK(snprintf(path, sizeof(path), "%s/cow-state.json", directory) > 0);
	FILE *file = fopen(path, "w");
	CHECK(file != NULL);
	CHECK(
	    fputs(
		"{\"views\":[{\"id\":\"a\",\"container_id\":\"a\"},"
		"{\"id\":\"b\",\"container_id\":\"a\",\"container_active\":true}]}",
		file) >= 0);
	CHECK(fclose(file) == 0);
	CHECK(cow_state_restore());
	c = cow_view_container(&a);
	CHECK(c != NULL && c->n_members == 2 && c->active->view == &b);
	cow_container_destroy(c);
	CHECK(rmdir(directory) == 0);
	return 0;
}
