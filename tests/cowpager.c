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
	layout_t layout = { 0 };
	canvas_t canvas = { .w = 1920, .h = 1080,
	    .page_cols = 1, .page_rows = 1 };
	cairo_surface_t *image = cairo_image_surface_create(
	    CAIRO_FORMAT_ARGB32, 100, 100);
	cairo_t *cr = cairo_create(image);
	draw_desk_pages(cr, &st, &layout, &outputs[0], &canvas, &desk,
	    0, 0, 100, 100);
	CHECK(cairo_status(cr) == CAIRO_STATUS_SUCCESS);
	cairo_destroy(cr);
	cairo_surface_destroy(image);
	free(overlay_handles);
	return 0;
}
