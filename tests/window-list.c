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

#include "../src/cmd-window-list.c"

#define CHECK(expr)                                                        \
	do {                                                               \
		if (!(expr)) {                                             \
			fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, \
			    #expr);                                        \
			abort();                                           \
		}                                                          \
	} while (0)

static int
actions(struct cow_menu *menu)
{
	int		      count = 0;
	struct cow_menu_item *item;
	wl_list_for_each(item, &menu->items, link)
	{
		if (item->type == COW_MENU_ITEM_NORMAL)
			count++;
	}
	return count;
}

int
main(void)
{
	wl_list_init(&cow_wm.containers);
	wl_list_init(&cow_wm.menus);
	wl_list_init(&cow_wm.open_menus);
	struct cow_view frame = { .identifier = "frame", .title = "Work" };
	struct cow_view a     = { .identifier = "a", .title = "Editor" };
	struct cow_view b     = { .identifier = "b", .title = "Terminal" };
	struct cow_container_pane   pane = { .id = 1, .name = "Code & tools" };
	struct cow_container	    c	 = { .id = 1,
			  .root			 = &pane };
	cow_container_set_frame(&c, &frame);
	struct cow_container_member ma	 = { .container = &c, .pane = &pane };
	struct cow_container_member mb	 = { .container = &c, .pane = &pane };
	a.container_member		 = &ma;
	b.container_member		 = &mb;
	wl_list_insert(&cow_wm.containers, &c.wm_link);
	struct window_list_entry entries[] = { { &a }, { &b }, { &frame } };
	struct cow_view		 plain	   = { 0 };
	struct window_list_entry mixed[]   = { { &b }, { &plain }, { &frame },
		  { &a } };
	struct window_list_group_entry *groups = group_entries(mixed, 4, NULL);
	size_t				first  = group_start(groups, 4, &c);
	CHECK(first + 2 < 4);
	CHECK(groups[first].index == 0 && groups[first + 1].index == 2 &&
	    groups[first + 2].index == 3);
	xfree(groups);
	struct cow_menu *menu = container_menu(&c, entries, 3, "%n %t", false,
	    true, true, 0);
	CHECK(actions(menu) == 3);
	CHECK(cow_container_top_group(&a) == &c);
	CHECK(menu->typeahead);
	struct cow_menu_item *last = wl_container_of(menu->items.prev, last,
	    link);
	CHECK(strcmp(last->action, "window-list -s b -w") == 0);
	cow_menu_destroy(menu);
	/* A filter excluding the frame and other tab must not expose either.
	 */
	menu = container_menu(&c, entries, 1, "%t", false, false, false, 0);
	CHECK(actions(menu) == 1);
	CHECK(!menu->typeahead);
	cow_menu_destroy(menu);
	/* Descendants still introduce a nested branch when its frame is
	 * filtered.
	 */
	struct cow_container_pane   parent_pane = { .id = 2 };
	struct cow_container	    parent = { .id = 2, .root = &parent_pane };
	struct cow_container_member mf	   = { .container = &parent,
		    .pane				  = &parent_pane };
	frame.container_member		   = &mf;
	wl_list_insert(&cow_wm.containers, &parent.wm_link);
	menu = container_menu(&parent, entries, 2, "%t", false, true, false, 0);
	CHECK(actions(menu) == 1);
	CHECK(cow_container_top_group(&a) == &parent);
	CHECK(actions(cow_menu_find(WINDOW_LIST_MENU ":1")) == 2);
	cow_menu_destroy_all();
	return 0;
}
