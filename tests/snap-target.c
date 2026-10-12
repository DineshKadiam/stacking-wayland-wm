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

#include "../src/pointer_op.c"
#include "cow/cmd.h"
#include "cow/frame.h"
#include "cow/style.h"

#define CHECK(x) do { \
	if (!(x)) { \
		fprintf(stderr, "%d: %s\n", __LINE__, #x); \
		abort(); \
	} \
} while (0)

static void
run(const char *text)
{
	struct cmd_list *list = NULL;
	char *cause = NULL;
	CHECK(cmd_string_parse(text, &list, NULL, 0, &cause) == 0);
	struct cmd_q *queue = cmdq_new();
	queue->client_fd = -1;
	cmdq_run(queue, list);
	CHECK(!queue->failed);
	cmdq_free(queue);
	cmd_list_free(list);
}

int
main(void)
{
	cow_config = cow_config_create();
	cow_config->snap_distance = 12;
	wl_list_init(&cow_wm.views);
	wl_list_init(&cow_wm.outputs);
	wl_list_init(&cow_wm.containers);
	wl_list_init(&cow_wm.seats);
	struct cow_output output = { .width = 1000, .height = 1000 };
	struct cow_view target = { .app_id = "wteddy", .output = &output,
	    .has_dimensions = true, .x = 300, .y = 300,
	    .content_width = 100, .content_height = 100 };
	struct cow_view moving = { .output = &output, .snap_skip = true };
	cow_view_cfg_init_from_global(&target.cfg);
	wl_list_insert(&cow_wm.views, &target.wm_link);
	for (int policy = 0; policy < 4; policy++) {
		if (policy == 1)
			run("style -g -n teddy -s %wteddy {\nsnap.target false\n}");
		else if (policy == 2)
			run("style -r -g -n teddy -s %wteddy {\nsnap.target true\n}");
		else if (policy == 3) {
			run("style -r -g -n teddy -s %wteddy {\nsnap.target false\n}");
			run("style -d -n teddy");
		}
		CHECK(target.snap_skip == (policy == 1));
		/* Keep the target borderless to isolate the edge positions. */
		target.dec.border_width = target.dec.title_thickness = 0;
		target.flags |= COW_VIEW_CIRCULATE_SKIP_FLAG;
		for (int resize = 0; resize < 2; resize++) {
			int l = 205, t = 310, r = 305, b = 410;
			snap_edges(&moving, &l, &t, &r, &b,
			    resize ? COW_EDGE_RIGHT : 0, NULL);
			CHECK(r == (policy == 1 ? 305 : 300));
		}
	}
	/* Squeezed frame axes follow the rendered title in every orientation. */
	target.dec.border_width = 4;
	target.dec.title_thickness = 20;
	target.cfg.titlebar.squeeze.mode = COW_SQUEEZE_FIXED;
	target.cfg.titlebar.squeeze.width = 40;
	target.cfg.titlebar.squeeze.justify = COW_JUST_CENTRE;
	enum cow_titlebar_position positions[] = { COW_TITLEBAR_TOP,
	    COW_TITLEBAR_BOTTOM, COW_TITLEBAR_LEFT, COW_TITLEBAR_RIGHT };
	for (size_t i = 0; i < sizeof positions / sizeof positions[0]; i++) {
		target.cfg.titlebar.position = positions[i];
		struct cow_box box;
		CHECK(cow_titlebar_snap_box(&target, &box));
		bool vertical = cow_titlebar_is_vertical(positions[i]);
		CHECK((vertical ? box.height : box.width) == 48);
		CHECK((vertical ? box.width : box.height) == 108);
		CHECK(box.x == (vertical ? 296 : 326));
		CHECK(box.y == (vertical ? 326 : 296));
		int l = box.x + 3, t = box.y + 3, r = l + 10, b = t + 10;
		snap_edges(&moving, &l, &t, &r, &b, false, NULL);
		CHECK(l == box.x && t == box.y);
		/* Approach the exposed side outside the title's span. */
		struct cow_box probe = { 300, 240, 20, 20 };
		uint32_t edge = COW_EDGE_BOTTOM;
		if (positions[i] == COW_TITLEBAR_BOTTOM) {
			probe.y = 440;
			edge = COW_EDGE_TOP;
		} else if (positions[i] == COW_TITLEBAR_LEFT) {
			probe.x = 240;
			probe.y = 300;
			edge = COW_EDGE_RIGHT;
		} else if (positions[i] == COW_TITLEBAR_RIGHT) {
			probe.x = 440;
			probe.y = 300;
			edge = COW_EDGE_LEFT;
		}
		CHECK(cow_frame_travel(&moving, &probe, edge, false, 200) == 36);
		CHECK(cow_frame_travel(&moving, &probe, edge, true, 200) == 36);
		struct cow_box outside = probe;
		if (vertical)
			outside.y = 500;
		else
			outside.x = 500;
		CHECK(cow_frame_travel(&moving, &outside, edge, false, 200) == 36);
		CHECK(cow_frame_travel(&moving, &outside, edge, true, 200) == 200);
		target.cfg.titlebar.squeeze.width = 100;
		CHECK(cow_frame_travel(&moving, &outside, edge, false, 200) == 200);
		target.cfg.titlebar.squeeze.width = 40;
		cow_frame_extend(&probe, edge, false, 36);
		CHECK(cow_frame_travel(&moving, &probe, edge, false, 200) == 200);
		CHECK(cow_frame_travel(&moving, &probe, edge, true, 200) == 0);
		target.cfg.titlebar.squeeze.width = 100;
		CHECK(!cow_titlebar_snap_box(&target, &box));
		target.cfg.titlebar.squeeze.width = 40;
	}
	struct cow_box shape[2], bounds = cow_view_frame_box(&target);
	target.sd = COW_SHADE_NORTH;
	CHECK(cow_titlebar_frame_rects(&target, &bounds, shape) == 1);
	target.sd = COW_SHADE_NONE;
	target.cfg.titlebar.squeeze.mode = COW_SQUEEZE_NONE;
	CHECK(cow_titlebar_frame_rects(&target, &bounds, shape) == 1);
	target.cfg.titlebar.squeeze.mode = COW_SQUEEZE_FIXED;
	/* Travel through the empty strip stops at the body, while travel
	 * beneath the title stops at its protruding edge instead.
	 */
	target.cfg.titlebar.position = COW_TITLEBAR_TOP;
	struct cow_box beside = { 300, 240, 20, 20 };
	struct cow_box below_title = { 330, 240, 20, 20 };
	for (int resize = 0; resize < 2; resize++) {
		CHECK(cow_frame_travel(&moving, &beside, COW_EDGE_BOTTOM,
		    resize, 200) == 36);
		CHECK(cow_frame_travel(&moving, &below_title, COW_EDGE_BOTTOM,
		    resize, 200) == 16);
	}
	/* Repeating a move passes the neighbour, including its body.
	 * Expansion remains blocked at contact.
	 */
	struct cow_box touching = below_title;
	cow_frame_extend(&touching, COW_EDGE_BOTTOM, false, 16);
	CHECK(cow_frame_travel(&moving, &touching, COW_EDGE_BOTTOM, false, 200) == 200);
	CHECK(cow_frame_travel(&moving, &touching, COW_EDGE_BOTTOM, true, 200) == 0);
	/* Full-width titles leave no passage beside them. */
	target.cfg.titlebar.squeeze.width = 100;
	CHECK(cow_frame_travel(&moving, &beside, COW_EDGE_BOTTOM, false, 200) == 16);
	target.cfg.titlebar.squeeze.width = 40;
	/* Align with the exposed body edge before moving into the gap. */
	struct cow_box away = { 500, 240, 20, 20 };
	CHECK(cow_frame_travel(&moving, &away, COW_EDGE_BOTTOM, false, 200) == 36);
	cow_frame_extend(&away, COW_EDGE_BOTTOM, false, 36);
	CHECK(cow_frame_travel(&moving, &away, COW_EDGE_LEFT, false, 200) == 126);
	CHECK(cow_frame_travel(&moving, &away, COW_EDGE_BOTTOM, true, 200) == 200);
	target.snap_skip = true;
	CHECK(cow_frame_travel(&moving, &away, COW_EDGE_LEFT, false, 200) == 200);
	CHECK(cow_frame_travel(&moving, &beside, COW_EDGE_BOTTOM, false, 200) == 200);
	target.snap_skip = false;
	/* The titlebar ends remain separate alignment targets as well. */
	struct cow_box end = { 276, 276, 20, 20 };
	CHECK(cow_frame_travel(&moving, &end, COW_EDGE_RIGHT, false, 200) == 30);
	/* Slide along the exposed north border straight to the titlebar,
	 * without stopping at the body's right edge or an unrelated window.
	 */
	struct cow_box gap = { 450, 276, 20, 20 };
	struct cow_view unrelated = { .output = &output, .has_dimensions = true,
	    .x = 420, .y = 500, .content_width = 20, .content_height = 20 };
	wl_list_insert(&cow_wm.views, &unrelated.wm_link);
	CHECK(cow_frame_travel(&moving, &gap, COW_EDGE_LEFT, false, 200) == 76);
	cow_frame_extend(&gap, COW_EDGE_LEFT, false, 76);
	CHECK(cow_frame_travel(&moving, &gap, COW_EDGE_LEFT, false, 200) == 200);
	wl_list_remove(&unrelated.wm_link);
	target.cfg.titlebar.squeeze.width = 100;
	CHECK(cow_frame_travel(&moving, &end, COW_EDGE_RIGHT, false, 200) == 200);
	target.cfg.titlebar.squeeze.width = 40;
	/* Expanding horizontally brings a previously diagonal window into
	 * the vertical path. Use the enlarged footprint for the second axis.
	 */
	cow_frame_extend(&beside, COW_EDGE_RIGHT, true, 40);
	CHECK(cow_frame_travel(&moving, &beside, COW_EDGE_BOTTOM, true, 200) == 16);
	/* A moving squeezed title can interlock with an ordinary neighbour. */
	struct cow_box start = cow_view_frame_box(&target);
	wl_list_remove(&target.wm_link);
	struct cow_view obstacle = { .output = &output, .has_dimensions = true,
	    .x = 300, .y = 240, .content_width = 20, .content_height = 20 };
	wl_list_insert(&cow_wm.views, &obstacle.wm_link);
	CHECK(cow_frame_travel(&target, &start, COW_EDGE_TOP, false, 200) == 36);
	/* Growth shifts a centred title into the obstacle before the body
	 * reaches it; do not merely test the final, potentially clear position.
	 */
	obstacle.x = 300;
	obstacle.y = 276;
	CHECK(cow_frame_travel(&target, &start, COW_EDGE_LEFT, true, 200) == 12);
	wl_list_remove(&obstacle.wm_link);
	cow_view_cfg_fini(&target.cfg);
	cow_style_fini();
	cow_config_destroy(cow_config);
	return 0;
}
