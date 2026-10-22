/* Exercise title events without a running compositor. */
#include "../src/view.c"
#include "cow/menu.h"

#define CHECK(expr) do { \
	if (!(expr)) { \
		fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); \
		abort(); \
	} \
} while (0)

static void
run(const char *text)
{
	struct cmd_list *list = NULL;
	char *cause = NULL;
	CHECK(cmd_string_parse(text, &list, NULL, 0, &cause) == 0);
	struct cmd_q *q = cmdq_new();
	q->client_fd = -1;
	cmdq_run(q, list);
	CHECK(!q->failed);
	cmdq_free(q);
	cmd_list_free(list);
	xfree(cause);
}

int
main(void)
{
	cow_config = cow_config_create();
	wl_list_init(&cow_wm.views);
	wl_list_init(&cow_wm.outputs);
	wl_list_init(&cow_wm.seats);
	wl_list_init(&cow_wm.containers);
	wl_list_init(&cow_wm.menus);
	wl_list_init(&cow_wm.open_menus);
	struct cow_view view = { .app_id = "editor", .identifier = "editor",
		.has_dimensions = true, .on_map_fired = true };
	cow_view_cfg_init_from_global(&view.cfg);
	wl_list_init(&view.selections);
	wl_list_insert(&cow_wm.views, &view.wm_link);
	run("style -g -n editor -s %editor {\ndecor {\nborder.width 7\n}\n}");
	run("style -g -n special -s /special/ {\ndecor {\nborder.width 9\n}\n}");
	CHECK(view.cfg.border == 7);
	view.dec.dirty = false;
	cow_update_reset();
	for (int i = 0; i < 360; i++) {
		char title[32];
		snprintf(title, sizeof(title), "OMG %d", i);
		window_handle_title(&view, NULL, title);
		CHECK(strcmp(view.title, title) == 0);
		CHECK(!view.dec.dirty);
		CHECK(cow_update_take() == COW_UPDATE_TITLE);
	}
	cow_view_flush_titles();
	CHECK(view.dec.dirty && !view.title_dirty);
	CHECK(cow_update_take() == (COW_UPDATE_MANAGE | COW_UPDATE_STATUS));
	view.dec.dirty = false;
	window_handle_title(&view, NULL, "OMG 359");
	CHECK(!view.title_dirty && !view.dec.dirty);
	CHECK(cow_update_take() == COW_UPDATE_NONE);
	window_handle_title(&view, NULL, "special 1");
	CHECK(view.cfg.border == 9);
	window_handle_title(&view, NULL, "ordinary");
	CHECK(view.cfg.border == 7);
	run("rule -g -T on-title-change -n record script { "
	    "menu-add Events #{self.title} nop }");
	window_handle_title(&view, NULL, "first");
	window_handle_title(&view, NULL, "second");
	window_handle_title(&view, NULL, "second");
	struct cow_menu *menu = cow_menu_find("Events");
	CHECK(menu && wl_list_length(&menu->items) == 2);
	struct cow_menu_item *item = wl_container_of(menu->items.prev, item, link);
	CHECK(strcmp(item->label, "second") == 0);
	/* A window closed before the refresh must not be redrawn. */
	view.closed = true;
	view.dec.dirty = false;
	cow_update_reset();
	cow_view_flush_titles();
	CHECK(!view.dec.dirty && cow_update_take() == COW_UPDATE_NONE);
	wl_list_remove(&view.wm_link);
	cow_rule_fini();
	cow_style_fini();
	cow_menu_destroy_all();
	xfree(view.title);
	cow_view_cfg_fini(&view.cfg);
	cow_config_destroy(cow_config);
	return 0;
}
