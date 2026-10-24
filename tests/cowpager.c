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

#define main cowpager_program_main
#include "../modules/cowpager/src/cowpager.c"
#undef main

#define CHECK(expr) do { \
	if (!(expr)) { \
		fprintf(stderr, "%s:%d: check failed: %s\n", \
		    __FILE__, __LINE__, #expr); \
		abort(); \
	} \
} while (0)

int
main(void)
{
	state_t ordered = { 0 };
	CHECK(cow_module_snapshot_parse(&ordered,
	    "{\"outputs\":[],\"windows\":[{\"id\":\"top\",\"stack_order\":3},"
	    "{\"id\":\"bottom\",\"stack_order\":1},"
	    "{\"id\":\"unknown\"},{\"id\":\"equal\",\"stack_order\":1}]}"));
	qsort(ordered.wins, ordered.n_wins, sizeof(*ordered.wins),
	    window_stack_compare);
	CHECK(ordered.n_wins == 4);
	CHECK(strcmp(ordered.wins[0].id, "unknown") == 0);
	CHECK(strcmp(ordered.wins[1].id, "bottom") == 0);
	CHECK(strcmp(ordered.wins[2].id, "equal") == 0);
	CHECK(strcmp(ordered.wins[3].id, "top") == 0);
	cow_module_snapshot_fini(&ordered);
	state_t containers = { 0 };
	CHECK(cow_module_snapshot_parse(&containers,
	    "{\"outputs\":[],\"windows\":["
	    "{\"id\":\"pane\",\"container_window_id\":\"outer\",\"focused\":true},"
	    "{\"id\":\"nested\",\"container_window_id\":\"outer\"},"
	    "{\"id\":\"outer\",\"container_window_id\":\"outer\"},"
	    "{\"id\":\"ordinary\"}]}"));
	fold_container_windows(&containers);
	CHECK(containers.wins[0].container_hidden);
	CHECK(!containers.wins[0].focused);
	CHECK(containers.wins[1].container_hidden);
	CHECK(!containers.wins[2].container_hidden);
	CHECK(containers.wins[2].focused);
	CHECK(!containers.wins[3].container_hidden);
	CHECK(!containers.wins[3].focused);
	cow_module_snapshot_fini(&containers);
	pout_t outputs[] = {
		{ .name = "HDMI-A-1", .number = 1, .current = true,
		  .output_w = 1920, .output_h = 1080 },
		{ .name = "DP-1", .number = 2 },
	};
	state_t st = { .outs = outputs, .n_outs = 2 };
	char *selectors[] = { "DP-1", "@1", "HDMI-A-1", "@current",
	    "DP-1", "missing" };
	opt_monitors = selectors;
	opt_n_monitors = sizeof(selectors) / sizeof(selectors[0]);
	size_t count;
	const pout_t **tabs = tab_outputs(&st, &count);
	CHECK(count == 2);
	CHECK(tabs[0] == &outputs[1] && tabs[1] == &outputs[0]);
	free(tabs);

	opt_n_monitors = 0;
	opt_monitors = NULL;
	CHECK(build_visible_desks(&outputs[0], 10, &count) == NULL);
	CHECK(count == 0);
	CHECK(visible_desk_at(&outputs[0], 0, 10) == NULL);

	st.n_outs = 1;
	pdesk_t desk = { 0 };
	/* Matching the stack takes precedence over focus on the first click. */
	pager_config_init(&cfg);
	cfg.match_stacking = 1;
	outputs[0].desks = &desk;
	outputs[0].n_desks = 1;
	pwin_t windows[2] = {
		{ .id = "lower", .output = "HDMI-A-1", .w = 1920, .h = 1080,
		  .focused = true, .stack_order = 1 },
		{ .id = "upper", .output = "HDMI-A-1", .w = 1920, .h = 1080,
		  .stack_order = 2 }
	};
	st.wins = windows;
	st.n_wins = 2;
	win_w = win_h = 200;
	layout_t hit_layout;
	compute_layout(&st, &hit_layout);
	double hx = hit_layout.cell_w / 2;
	double hy = hit_layout.chrome_h + (hit_layout.cell_h - hit_layout.chrome_h) / 2;
	CHECK(hit_test_window(&st, hx, hy, NULL) == &windows[1]);
	CHECK(hit_test_window(&st, hx, hy, NULL) == &windows[0]);
	st.wins = NULL;
	st.n_wins = 0;
	outputs[0].desks = NULL;
	outputs[0].n_desks = 0;
	pager_config_init(&cfg);
	layout_t layout = { 0 };
	canvas_t canvas = { .w = 1920, .h = 1080,
	    .page_cols = 1, .page_rows = 1 };
	cairo_surface_t *image = cairo_image_surface_create(
	    CAIRO_FORMAT_ARGB32, 100, 100);
	cairo_t *cr = cairo_create(image);
	draw_desk_pages(cr, &st, &layout, &outputs[0], &canvas, &desk,
	    0, 0, 100, 100);
	CHECK(cairo_status(cr) == CAIRO_STATUS_SUCCESS);

	pager_config_init(&cfg);
	CHECK(!cfg.page_separator_set);
	const char *why = NULL;
	char *separator[] = { "style", "page.separator", "invalid" };
	CHECK(!load_pager_style(separator, 3, &why));
	CHECK(!cfg.page_separator_set);
	separator[2] = "0xFF0000";
	CHECK(load_pager_style(separator, 3, &why));
	CHECK(cfg.page_separator_set);
	CHECK(cfg.page_separator[0] == 1 && cfg.page_separator[1] == 0 &&
	    cfg.page_separator[2] == 0 && cfg.page_separator[3] == 1);
	outputs[0].page_cols = 2;
	outputs[0].page_rows = 2;
	canvas.page_cols = canvas.page_rows = 2;
	canvas.w = 3840;
	canvas.h = 2160;
	for (int visible = 1; visible >= 0; visible--) {
		if (!visible) {
			separator[2] = "0x00000000";
			CHECK(load_pager_style(separator, 3, &why));
			CHECK(cfg.page_separator[3] == 0);
		}
		draw_desk_pages(cr, &st, &layout, &outputs[0], &canvas,
		    &desk, 0, 0, 100, 100);
		cairo_surface_flush(image);
		unsigned char *pixels = cairo_image_surface_get_data(image);
		int stride = cairo_image_surface_get_stride(image);
		int red_vertical = 0, red_horizontal = 0;
		for (int i = 60; i < 90; i++) {
			red_vertical += ((uint32_t *)(pixels + i * stride))[50]
			    == 0xFFFF0000;
			red_horizontal += ((uint32_t *)(pixels + 50 * stride))[i]
			    == 0xFFFF0000;
		}
		CHECK((red_vertical > 0) == visible);
		CHECK((red_horizontal > 0) == visible);
	}
	/* Page images overlay inherited colours, but explicit pager colours win. */
	char tile[] = "/tmp/cowpager-tile.XXXXXX.xpm";
	int fd = mkstemps(tile, 4);
	CHECK(fd >= 0);
	FILE *file = fdopen(fd, "w");
	CHECK(file != NULL);
	CHECK(fputs("/* XPM */\nstatic char *tile[] = {\n"
	    "\"2 1 2 1\",\n\"g c #00ff00\",\n\"b c #0000ff\",\n\"gb\"};\n", file) >= 0);
	CHECK(fclose(file) == 0);
	struct cow_module_page_background page = {
		.desk = 0, .col = 0, .row = 0, .has_colour = true,
		.colour = { 1, 0, 0, 1 }, .image = tile,
	};
	outputs[0].page_backgrounds = &page;
	outputs[0].n_page_backgrounds = 1;
	fill_output_backgrounds(cr, &st, &canvas, 0, 0, 0, 0, 0, 100, 100);
	cairo_surface_flush(image);
	uint32_t *pixel = (uint32_t *)cairo_image_surface_get_data(image);
	CHECK(pixel[0] == 0xff00ff00);
	CHECK(pixel[1] == 0xff0000ff && pixel[2] == 0xff00ff00);
	CHECK(unlink(tile) == 0);
	page.image = "none";
	fill_output_backgrounds(cr, &st, &canvas, 0, 0, 0, 0, 0, 100, 100);
	cairo_surface_flush(image);
	CHECK(pixel[0] == 0xffff0000);
	page.image = tile;
	cfg.page_bg_set = true;
	fill_output_backgrounds(cr, &st, &canvas, 0, 0, 0, 0, 0, 100, 100);
	cairo_surface_flush(image);
	CHECK(pixel[0] == 0xffff0000);
	cow_image_cache_clear(&background_cache);
	cairo_destroy(cr);
	cairo_surface_destroy(image);
	free(overlay_handles);
	return 0;
}
