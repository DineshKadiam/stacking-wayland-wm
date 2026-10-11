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
	wl_list_remove(&target.wm_link);
	cow_view_cfg_fini(&target.cfg);
	cow_style_fini();
	cow_config_destroy(cow_config);
	return 0;
}
