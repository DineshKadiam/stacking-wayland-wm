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

/* Exercise maximized refitting without a compositor. */
#include "../src/view.c"

#define CHECK(expr) do { \
	if (!(expr)) { \
		fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); \
		abort(); \
	} \
} while (0)

int
main(void)
{
	cow_config = cow_config_create();
	cow_config->page_cols = 3;
	cow_config->page_rows = 3;
	struct cow_output output = {
		.x = 1920, .y = -1200, .width = 1920, .height = 1200,
		.usable_area = { 1920, -1181, 1920, 1181 },
	};
	struct cow_desk desk = { 0 };
	struct cow_view view = {
		.output = &output, .desk = &desk,
		.content_width = 1896, .content_height = 1138,
		.maximized = true, .maximized_h = true, .maximized_v = true,
	};
	cow_view_cfg_init_from_global(&view.cfg);
	int fw, fh, cx, cy;
	cow_titlebar_frame_geometry(&view, view.content_width,
	    view.content_height, &fw, &fh, &cx, &cy);

	/* Refitting keeps the saved page, even with another page displayed. */
	for (int viewport = 0; viewport < 3; viewport++) {
		desk.page_vx = viewport * output.width;
		desk.page_vy = viewport * output.height;
		view.vx = output.width + cx;
		view.vy = output.height + 19 + cy;
		cow_view_sync_global_from_vxy(&view);
		int x = view.x, y = view.y;
		view_refit_maximized(&view);
		CHECK(view.x == x && view.y == y);
		CHECK(view.vx == output.width + cx);
		CHECK(view.vy == output.height + 19 + cy);

		/* A late work-area update must not replace the restored page. */
		cow_view_reflow_maximized(&view);
		CHECK(view.pending_x == x && view.pending_y == y);
	}

	/* Reservation changes and -R still use the window's own page. */
	view.maximized_v_ignore_reserved = true;
	view_refit_maximized(&view);
	CHECK(view.vy == output.height + cy);
	view.maximized_v_ignore_reserved = false;
	view_refit_maximized(&view);
	CHECK(view.vy == output.height + 19 + cy);

	/* Preserve the untouched axis and keep-geometry maximization. */
	view.maximized_h = false;
	view.vx += 100;
	cow_view_sync_global_from_vxy(&view);
	view_refit_maximized(&view);
	CHECK(view.vx == output.width + cx + 100);
	view.maximized_v_keep_geometry = true;
	view.vy += 100;
	cow_view_sync_global_from_vxy(&view);
	view_refit_maximized(&view);
	CHECK(view.vy == output.height + 19 + cy + 100);

	/* Page-sticky windows remain anchored to the current viewport. */
	view.maximized_h = true;
	view.maximized_v_keep_geometry = false;
	view.flags |= COW_VIEW_STICKY_PAGE_FLAG;
	view_refit_maximized(&view);
	CHECK(view.x == output.usable_area.x + cx);
	CHECK(view.y == output.usable_area.y + cy);

	cow_view_cfg_fini(&view.cfg);
	cow_config_destroy(cow_config);
	cow_config = NULL;
	return 0;
}
