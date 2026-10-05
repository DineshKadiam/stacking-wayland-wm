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
#include <stdio.h>
#include <stdlib.h>
#include "../src/decoration-tiles.c"

static void
check_border(const struct cow_border_colour *bc, int style, int bw, int cl)
{
	int	  k    = (cl > bw ? cl : bw) + 3;
	int	  side = 2 * k + 1;
	int	  w = 4 * k + 17, h = 3 * k + 11;
	uint32_t *tiles	 = xcalloc(side * side, sizeof(*tiles));
	uint32_t *full	 = xcalloc(w * h, sizeof(*full));
	uint32_t *pieces = xcalloc(w * h, sizeof(*pieces));
	cow_border_style_render(tiles, side, side, side, bc, bw, cl, style);
	cow_border_style_render(full, w, w, h, bc, bw, cl, style);
	struct cow_box src[8], dst[8];
	piece_rectangles(k, bw, w, h, src, dst);

	/* Linear filtering must not blend handle marks into stretched edges. */
	for (int i = 4; i < 8; i++) {
		for (int y = 0; y < src[i].height; y++) {
			for (int x = 0; x < src[i].width; x++) {
				int offset = (src[i].y + y) * side + src[i].x + x;
				int step = i < 6 ? 1 : side;
				if (tiles[offset] != tiles[offset - step] ||
				    tiles[offset] != tiles[offset + step]) {
					fprintf(stderr, "tiles filtering bleed: style=%d bw=%d cl=%d\n",
					    style, bw, cl);
					abort();
				}
			}
		}
	}

	/* Reassemble the exact crops and stretches used by viewporter. */
	for (int i = 0; i < 8; i++) {
		for (int y = 0; y < dst[i].height; y++) {
			for (int x = 0; x < dst[i].width; x++) {
				int sx = src[i].x +
				    x * src[i].width / dst[i].width;
				int sy = src[i].y +
				    y * src[i].height / dst[i].height;
				int offset = (dst[i].y + y) * w + dst[i].x + x;
				pieces[offset] = tiles[sy * side + sx];
			}
		}
	}
	if (memcmp(full, pieces, w * h * sizeof(*full)) != 0) {
		fprintf(stderr, "border mismatch: style=%d bw=%d cl=%d\n",
		    style, bw, cl);
		abort();
	}
	xfree(tiles);
	xfree(full);
	xfree(pieces);
}

int
main(void)
{
	struct cow_border_colour bc = { 0 };
	float *colours[] = { bc.n, bc.s, bc.e, bc.w, bc.nw, bc.ne, bc.sw,
		bc.se };
	for (size_t i = 0; i < 8; i++) {
		colours[i][0] = (i + 1) / 9.0f;
		colours[i][1] = 0.3f;
		colours[i][2] = 0.6f;
		colours[i][3] = 0.7f;
	}
	for (int corners = 0; corners < 2; corners++) {
		bc.has_corners = bc.has_nw = bc.has_ne = bc.has_sw = bc.has_se =
		    corners;
		for (int style = -1; style <= 1; style++)
			for (int bw = 1; bw <= 16; bw++)
				for (int cl = 0; cl <= 40; cl += 10)
					check_border(&bc, style, bw, cl);
	}
	return 0;
}
