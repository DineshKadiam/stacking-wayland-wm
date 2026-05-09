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
#include "cow/vector.h"

/*
 * Draw a single line between two points using Bresenham's algorithm.
 *
 * This is how fvwm does it...
 */
static void
draw_line(uint32_t *pixels, int stride, int w, int h, int x0, int y0, int x1,
    int y1, uint32_t colour)
{
	int dx = x1 - x0;
	int dy = y1 - y0;
	int sx = (dx > 0) ? 1 : -1;
	int sy = (dy > 0) ? 1 : -1;
	dx     = (dx > 0) ? dx : -dx;
	dy     = (dy > 0) ? dy : -dy;

	if (dx == 0 && dy == 0) {
		if (x0 >= 0 && x0 < w && y0 >= 0 && y0 < h)
			pixels[y0 * stride + x0] = colour;
		return;
	}

	int err;
	if (dx >= dy) {
		err = 2 * dy - dx;
		for (int i = 0; i <= dx; i++) {
			if (x0 >= 0 && x0 < w && y0 >= 0 && y0 < h)
				pixels[y0 * stride + x0] = colour;
			if (err > 0) {
				y0 += sy;
				err -= 2 * dx;
			}
			err += 2 * dy;
			x0 += sx;
		}
	} else {
		err = 2 * dx - dy;
		for (int i = 0; i <= dy; i++) {
			if (x0 >= 0 && x0 < w && y0 >= 0 && y0 < h)
				pixels[y0 * stride + x0] = colour;
			if (err > 0) {
				x0 += sx;
				err -= 2 * dy;
			}
			err += 2 * dx;
			y0 += sy;
		}
	}
}

void
cow_vector_button_draw(const struct cow_vector_button *vector, uint32_t *pixels,
    int stride, int w, int h, int btn_size, const uint32_t *colors,
    bool pressed)
{
	for (int i = 1; i < vector->num_points; i++) {
		int c = vector->points[i].colour;
		if (c < 0 || c >= 5)
			continue;
		if (c == COW_VECTOR_INVISIBLE)
			continue;

		/* When pressed, swap shadow/highlight */
		int effective_c = c;
		if (pressed) {
			if (c == COW_VECTOR_SHADOW)
				effective_c = COW_VECTOR_HIGHLIGHT;
			else if (c == COW_VECTOR_HIGHLIGHT)
				effective_c = COW_VECTOR_SHADOW;
		}

		int x0 = btn_size * vector->points[i - 1].x / 100 +
		    vector->points[i - 1].xoff;
		int y0 = btn_size * vector->points[i - 1].y / 100 +
		    vector->points[i - 1].yoff;
		int x1 = btn_size * vector->points[i].x / 100 +
		    vector->points[i].xoff;
		int y1 = btn_size * vector->points[i].y / 100 +
		    vector->points[i].yoff;

		draw_line(pixels, stride, w, h, x0, y0, x1, y1,
		    colors[effective_c]);
	}
}
