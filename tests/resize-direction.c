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


/* Keep the command's private helpers private; exercise them directly. */
#include "../src/cmd-resize.c"
#include <limits.h>
#include <stdio.h>

#define CHECK(expr) do { \
	if (!(expr)) { \
		fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); \
		abort(); \
	} \
} while (0)

int
main(void)
{
	static const struct {
		int size, x, y;
		const char *corner, *automatic;
	} cases[] = {
		{ 100, 0, 0, "nw", "nw" }, { 100, 99, 0, "ne", "ne" },
		{ 100, 0, 99, "sw", "sw" }, { 100, 99, 99, "se", "se" },
		{ 100, 49, 49, "nw", "" }, { 100, 50, 49, "ne", "" },
		{ 100, 49, 50, "sw", "" }, { 100, 50, 50, "se", "" },
		{ 101, 50, 50, "nw", "" }, { 101, 51, 51, "se", "" },
		{ 100, 50, 0, "ne", "n" }, { 100, 50, 99, "se", "s" },
		{ 100, 0, 50, "sw", "w" }, { 100, 99, 50, "se", "e" },
		{ 100, 33, 50, "sw", "w" }, { 100, 34, 50, "sw", "" },
		{ 100, 66, 50, "se", "" }, { 100, 67, 50, "se", "e" },
		{ 100, -1, 0, NULL, NULL }, { 100, 0, -1, NULL, NULL },
		{ 100, 100, 0, NULL, NULL }, { 100, 0, 100, NULL, NULL },
		{ INT_MAX, INT_MAX - 1, INT_MAX - 1, "se", "se" },
	};
	struct cmd command = {0};
	struct cmd_q *queue = cmdq_new();
	queue->cmd = &command;
	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		/* Nonzero decor extents and a negative output origin. */
		struct cow_view view = { .x = -298, .y = -188,
		    .content_width = cases[i].size - 4,
		    .content_height = cases[i].size - 14 };
		view.dec.border_width = 2;
		view.dec.title_thickness = 10;
		struct cow_seat seat = { .has_pointer_position = true,
		    .pointer_x = -300 + cases[i].x,
		    .pointer_y = -200 + cases[i].y };
		for (int corners = 0; corners <= 1; corners++) {
			const char *spec = corners ? cases[i].corner : cases[i].automatic;
			uint32_t edges = 0, expected = 0;
			CHECK(automatic_edges(&view, &seat, &edges, queue, corners) ==
			    (spec != NULL));
			if (spec == NULL)
				continue;
			if (*spec != '\0')
				CHECK(parse_edges(spec, &expected));
			CHECK(edges == expected);
			if (corners) {
				int x, y;
				resize_warp_position(&view, &seat, edges, &x, &y);
				CHECK(x == -300 + ((edges & COW_EDGE_LEFT) ? 0 : cases[i].size - 1));
				CHECK(y == -200 + ((edges & COW_EDGE_TOP) ? 0 : cases[i].size - 1));
			}
		}
		seat.has_pointer_position = false;
		uint32_t edges;
		CHECK(!automatic_edges(&view, &seat, &edges, queue, true));
	}
	cmdq_free(queue);
	return 0;
}
