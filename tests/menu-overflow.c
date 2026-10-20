#include "../src/menu.c"

#define CHECK(expr) do { \
	if (!(expr)) { \
		fprintf(stderr, "%d: %s\n", __LINE__, #expr); \
		abort(); \
	} \
} while (0)

int
main(void)
{
	wl_list_init(&cow_wm.outputs);
	wl_list_init(&cow_wm.seats);
	wl_list_init(&cow_wm.views);
	wl_list_init(&cow_wm.menus);
	wl_list_init(&cow_wm.open_menus);
	wl_list_init(&cow_wm.menu_graveyard);
	struct cow_output out = { .x = 800, .y = 200,
	    .width = 400, .height = 240 };
	wl_list_insert(&cow_wm.outputs, &out.wm_link);
	cow_wm.current_output = &out;
	struct cow_menu *menu = cow_menu_create("Overflow");
	cow_menu_enable_typeahead(menu);
	cow_menu_enable_multi_select(menu);
	cow_menu_item_add(menu, COW_MENU_ITEM_TITLE, "Applications", NULL,
	    false, false, NULL);
	for (int i = 0; i < 40; i++) {
		char label[32];
		snprintf(label, sizeof(label), "Application %02d", i);
		cow_menu_item_add(menu, COW_MENU_ITEM_NORMAL, label, "nop",
		    false, false, NULL);
	}
	struct cow_menu_popup *p = popup_alloc(menu);
	p->x = 1190;
	p->y = 435;
	popup_layout(p);
	CHECK(p->height <= out.height);
	CHECK(p->next_item > 0);
	CHECK(p->rows[p->row_count - 1].item == &p->more);
	CHECK(wl_list_length(&menu->items) == 41);
	/* Passive notifications cannot activate a More submenu.
	 */
	struct cow_menu_popup *notice = popup_alloc(menu);
	notice->passive = true;
	notice->max_height = out.height;
	popup_layout(notice);
	CHECK(notice->next_item == -1);
	CHECK(notice->row_count == 41);
	CHECK(notice->height <= out.height);
	popup_destroy(notice);
	int x = p->x, y = p->y;
	popup_fit_to_output(p, &x, &y, NULL, &out);
	CHECK(x >= out.x && x + p->width <= out.x + out.width);
	CHECK(y >= out.y && y + p->height <= out.y + out.height);
	p->x = x;
	p->y = y;
	int more_y = y + p->height - p->style.border_width - p->item_h / 2;
	CHECK(item_under(p, x + 10, more_y) == p->row_count - 1);
	p->selected = p->row_count - 2;
	nav_move(p, 1);
	CHECK(item_at_index(p, p->selected) == &p->more);

	/* Opening More shares selection state but has its own visible rows.
	 * Surface creation is deferred, so no compositor is needed here.
	 */
	wl_list_insert(cow_wm.open_menus.prev, &p->link);
	cow_wm.compositor = (void *)1;
	cow_wm.manager = (void *)1;
	dispatch_item(p, &p->more);
	CHECK(p->child != NULL && p->child->continuation);
	CHECK(p->child->first_item == p->next_item);
	CHECK(p->child->checked == p->checked);
	menu_multi_toggle(p->child, 0);
	CHECK(p->checked[p->next_item]);
	memset(p->checked, 0, p->checked_count * sizeof(*p->checked));
	strcpy(menu->typeahead_query, "Application 39");
	nav_typeahead_match(p->child);
	CHECK(p->child == NULL && p->row_count == 2);
	struct cow_menu_popup *closed =
	    wl_container_of(cow_wm.menu_graveyard.next, closed, link);
	wl_list_remove(&closed->link);
	popup_destroy(closed);
	menu->typeahead_query[0] = '\0';
	popup_layout(p);
	wl_list_remove(&p->link);
	cow_wm.compositor = NULL;
	cow_wm.manager = NULL;

	/* Every original item appears once, in order, across continuations. */
	int expected = 0;
	for (;;) {
		for (int row = 0; row < p->row_count; row++) {
			if (p->rows[row].item != &p->more)
				CHECK(p->rows[row].source_index == expected++);
		}
		if (p->next_item < 0)
			break;
		CHECK(p->next_item == expected);
		p->first_item = p->next_item;
		popup_layout(p);
		CHECK(p->height <= out.height);
	}
	CHECK(expected == 41);
	int checked_source = p->rows[0].source_index;
	menu_multi_toggle(p, 0);
	CHECK(p->checked[checked_source]);

	/* Filtering starts from the whole definition, even from its last page. */
	strcpy(menu->typeahead_query, "Application 39");
	nav_typeahead_match(p);
	CHECK(p->next_item == -1);
	CHECK(p->row_count == 2);
	CHECK(p->rows[1].source_index == 40);
	CHECK(p->selected == 1);
	CHECK(p->checked[checked_source]);

	menu->typeahead_query[0] = '\0';
	out.height = 2000;
	popup_layout(p);
	CHECK(p->next_item == -1 && p->row_count == 41);
	CHECK(p->height <= out.height);
	popup_destroy(p);
	out.height = 240;
	cow_wm.compositor = (void *)1;
	cow_wm.manager = (void *)1;
	p = cow_menu_popup_open_above(menu, out.x, out.y + 180, NULL);
	CHECK(p != NULL);
	CHECK(p->height <= 180);
	CHECK(p->y + p->height <= out.y + 180);
	CHECK(p->next_item > 0);
	p->selected = p->row_count - 1;
	dispatch_item(p, &p->more);
	CHECK(p->child != NULL);
	CHECK(p->child->y + p->child->height <= out.y + 180);
	closed = p->child;
	cow_menu_popup_close(closed);
	wl_list_remove(&closed->link);
	popup_destroy(closed);
	wl_list_remove(&p->link);
	popup_destroy(p);
	struct cow_view launcher = { .identifier = "launcher" };
	struct cow_view other = { .identifier = "other" };
	p = cow_menu_popup_open_above(menu, out.x, out.y + 180, &launcher);
	p->toggle = true;
	cow_menu_dismiss_for_window(&launcher);
	CHECK(!cow_menu_is_open());
	CHECK(cow_menu_toggle(menu, &launcher));
	CHECK(!cow_menu_toggle(menu, &launcher));
	wl_list_remove(&p->link);
	popup_destroy(p);

	p = cow_menu_popup_open_above(menu, out.x, out.y + 180, &launcher);
	p->toggle = true;
	cow_menu_dismiss_for_window(&other);
	CHECK(!cow_menu_toggle(menu, &launcher));
	wl_list_remove(&p->link);
	popup_destroy(p);

	p = cow_menu_popup_open_above(menu, out.x, out.y + 180, &launcher);
	CHECK(cow_menu_toggle(menu, &launcher));
	CHECK(!cow_menu_is_open());
	wl_list_remove(&p->link);
	popup_destroy(p);
	cow_wm.compositor = NULL;
	cow_wm.manager = NULL;
	cow_menu_destroy(menu);
	wl_list_remove(&out.wm_link);
	if (g_font_desc != NULL)
		pango_font_description_free(g_font_desc);
	xfree(g_font_name);
	xfree(g_default_style.icon_path);
	xfree(g_default_style.font);
	return 0;
}
