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

/* Exercise both the drag and the client's accepted dimensions. */
#include "../src/view.c"
#include <sys/socket.h>
#include <unistd.h>

#define CHECK(expr)                                                        \
	do {                                                               \
		if (!(expr)) {                                             \
			fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, \
			    #expr);                                        \
			abort();                                           \
		}                                                          \
	} while (0)

int
main(void)
{
	cow_config		    = cow_config_create();
	cow_config->geometry_window = COW_GEOMETRY_WINDOW_NONE;
	wl_list_init(&cow_wm.outputs);
	wl_list_init(&cow_wm.views);
	wl_list_init(&cow_wm.containers);
	wl_list_init(&cow_wm.seats);
	struct cow_desk	  desk	 = { 0 };
	struct cow_output output = {
		.width	      = 905,
		.height	      = 705,
		.usable_area  = { 0, 0, 905, 705 },
		.current_desk = &desk
	};
	wl_list_insert(&cow_wm.outputs, &output.wm_link);
	int sockets[2];
	CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
	struct wl_display *display = wl_display_connect_to_fd(sockets[0]);
	CHECK(display != NULL);
	struct river_window_v1 *window = (struct river_window_v1 *)
	    wl_proxy_create((struct wl_proxy *)display,
		&river_window_v1_interface);
	CHECK(window != NULL);
	const uint32_t edges[] = { COW_EDGE_LEFT, COW_EDGE_RIGHT, COW_EDGE_TOP,
		COW_EDGE_BOTTOM, COW_EDGE_LEFT | COW_EDGE_TOP,
		COW_EDGE_RIGHT | COW_EDGE_TOP, COW_EDGE_LEFT | COW_EDGE_BOTTOM,
		COW_EDGE_RIGHT | COW_EDGE_BOTTOM };
	for (size_t i = 0; i < sizeof(edges) / sizeof(edges[0]); i++) {
		for (int scenario = 0; scenario < 5; scenario++) {
			struct cow_view view = { .x = 100,
				.y		    = 100,
				.content_width	    = 800,
				.content_height	    = 600,
				.min_width	    = 300,
				.min_height	    = 200,
				.max_width	    = 1000,
				.max_height	    = 800,
				.has_dimensions	    = true,
				.on_map_fired	    = true,
				.river_window	    = window,
				.output		    = &output,
				.desk		    = &desk };
			struct cow_seat seat = {
				.op_in_progress = true,
				.op = { .type	  = COW_POINTER_OP_RESIZE,
				       .view	  = &view,
				       .start_x	  = 100,
				       .start_y	  = 100,
				       .start_width  = 800,
				       .start_height = 600,
				       .resize_edges = edges[i] }
			};
			bool horizontal = edges[i] &
			    (COW_EDGE_LEFT | COW_EDGE_RIGHT);
			bool vertical = edges[i] &
			    (COW_EDGE_TOP | COW_EDGE_BOTTOM);
			int delta  = scenario == 0 ? -1200 :
			     scenario == 1	   ? 1200 :
						     -100;
			seat.op_dx = edges[i] & COW_EDGE_LEFT ? -delta : delta;
			seat.op_dy = edges[i] & COW_EDGE_TOP ? -delta : delta;
			/* Snapping must not shrink past the advertised minimum.
			 */
			output.usable_area = (struct cow_box) { 0, 0, 905,
				705 };
			if (scenario == 4) {
				output.usable_area.x	 = edges[i] &
					    COW_EDGE_LEFT ?
					605 :
					0;
				output.usable_area.width = edges[i] &
					COW_EDGE_LEFT ?
				    300 :
				    395;
				output.usable_area.y = edges[i] & COW_EDGE_TOP ?
				    505 :
				    0;
				output.usable_area.height = edges[i] &
					COW_EDGE_TOP ?
				    200 :
				    295;
				seat.op_dx = edges[i] & COW_EDGE_LEFT ? 498 :
									-498;
				seat.op_dy = edges[i] & COW_EDGE_TOP ? 398 :
								       -398;
			}
			cow_config->snap_distance = scenario >= 2 ? 12 : 0;
			cow_pointer_op_process(&seat);
			int w = horizontal ? (scenario == 0	   ? 300 :
						     scenario == 1 ? 1000 :
								     700) :
					     800;
			int h = vertical ? (scenario == 0	 ? 200 :
						   scenario == 1 ? 800 :
								   500) :
					   600;
			if (scenario == 4) {
				w = horizontal ? 300 : 800;
				h = vertical ? 200 : 600;
			}
			if (view.proposed_width != w ||
			    view.proposed_height != h)
				fprintf(stderr,
				    "edges=%u scenario=%d: got %dx%d, expected %dx%d\n",
				    edges[i], scenario, view.proposed_width,
				    view.proposed_height, w, h);
			CHECK(view.proposed_width == w &&
			    view.proposed_height == h);
			CHECK(view.x == 100 && view.y == 100);
			/* The client may reject the proposal, even after
			 * release. */
			if (scenario == 3) {
				w += horizontal ? 23 : 0;
				h += vertical ? 17 : 0;
				seat.op_in_progress = false;
			}
			window_handle_dimensions(&view, NULL, w, h);
			CHECK(view.x ==
			    ((edges[i] & COW_EDGE_LEFT) ? 900 - w : 100));
			CHECK(view.y ==
			    ((edges[i] & COW_EDGE_TOP) ? 700 - h : 100));
			CHECK(!view.deferred_position);
			/* Cumulative pointer deltas must not accumulate drift.
			 */
			seat.op_in_progress = true;
			cow_pointer_op_process(&seat);
			window_handle_dimensions(&view, NULL, w, h);
			CHECK(view.x ==
			    ((edges[i] & COW_EDGE_LEFT) ? 900 - w : 100));
			CHECK(view.y ==
			    ((edges[i] & COW_EDGE_TOP) ? 700 - h : 100));
			cow_config->snap_distance = 0;
			seat.op_dx = seat.op_dy = 0;
			cow_pointer_op_process(&seat);
			CHECK(view.proposed_width == 800 &&
			    view.proposed_height == 600);
			window_handle_dimensions(&view, NULL, 800, 600);
			CHECK(view.x == 100 && view.y == 100);
		}
	}
	wl_proxy_destroy((struct wl_proxy *)window);
	wl_display_disconnect(display);
	close(sockets[1]);
	cow_config_destroy(cow_config);
	return 0;
}
