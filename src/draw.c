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

#include "cow/draw.h"

#include <stddef.h>

void
cow_draw_hline(struct cow_pixbuf *p, int x1, int x2, int y, uint32_t colour)
{
	if (p == NULL || p->pixels == NULL)
		return;
	if (y < 0 || y >= p->height)
		return;
	if (x1 < 0)
		x1 = 0;
	if (x2 >= p->width)
		x2 = p->width - 1;
	for (int x = x1; x <= x2; x++)
		p->pixels[y * p->stride_px + x] = colour;
}

void
cow_draw_vline(struct cow_pixbuf *p, int x, int y1, int y2, uint32_t colour)
{
	if (p == NULL || p->pixels == NULL)
		return;
	if (x < 0 || x >= p->width)
		return;
	if (y1 < 0)
		y1 = 0;
	if (y2 >= p->height)
		y2 = p->height - 1;
	for (int y = y1; y <= y2; y++)
		p->pixels[y * p->stride_px + x] = colour;
}

void
cow_draw_fill_rect(struct cow_pixbuf *p, int x, int y, int w, int h,
    uint32_t colour)
{
	if (p == NULL || p->pixels == NULL || w <= 0 || h <= 0)
		return;
	int x0 = x < 0 ? 0 : x;
	int y0 = y < 0 ? 0 : y;
	int x1 = x + w;
	int y1 = y + h;
	if (x1 > p->width)
		x1 = p->width;
	if (y1 > p->height)
		y1 = p->height;
	for (int yy = y0; yy < y1; yy++) {
		uint32_t *row = p->pixels + yy * p->stride_px;
		for (int xx = x0; xx < x1; xx++)
			row[xx] = colour;
	}
}

void
cow_draw_gradient_rect(struct cow_pixbuf *p, int x, int y, int w, int h,
    const struct cow_gradient *g)
{
	if (p == NULL || p->pixels == NULL || g == NULL ||
	    g->mode == COW_GRADIENT_NONE || w <= 0 || h <= 0)
		return;

	int x0 = x < 0 ? 0 : x;
	int y0 = y < 0 ? 0 : y;
	int x1 = x + w;
	int y1 = y + h;
	if (x1 > p->width)
		x1 = p->width;
	if (y1 > p->height)
		y1 = p->height;

	int denom = g->mode == COW_GRADIENT_H ? w - 1 : h - 1;
	if (denom <= 0)
		denom = 1;

	for (int yy = y0; yy < y1; yy++) {
		uint32_t *row = p->pixels + yy * p->stride_px;
		for (int xx = x0; xx < x1; xx++) {
			int pos = g->mode == COW_GRADIENT_H ? xx - x : yy - y;
			float t = (float)pos / (float)denom;
			float c[4];
			for (int i = 0; i < 4; i++)
				c[i] = g->from[i] + (g->to[i] - g->from[i]) * t;
			row[xx] = cow_colour_to_pixel(c);
		}
	}
}

void
cow_draw_clear_rect(struct cow_pixbuf *p, int x, int y, int w, int h)
{
	cow_draw_fill_rect(p, x, y, w, h, 0u);
}

void
cow_draw_relieve_rect(struct cow_pixbuf *p, int x, int y, int w, int h,
    int line_width, uint32_t ul_left, uint32_t ul_top, uint32_t br_bot,
    uint32_t br_right)
{
	if (p == NULL || p->pixels == NULL)
		return;
	if (w <= 0 || h <= 0 || line_width <= 0)
		return;

	int max_w = (w + 1) / 2;
	if (max_w > line_width)
		max_w = line_width;
	int max_h = (h + 1) / 2;
	if (max_h > line_width)
		max_h = line_width;

	for (int i = 0; i < max_w; i++) {
		if (ul_left != COW_DRAW_TRANSPARENT_PX)
			cow_draw_vline(p, x + i, y + i, y + h - 1 - i, ul_left);
	}
	for (int i = 0; i < max_h; i++) {
		if (ul_top != COW_DRAW_TRANSPARENT_PX)
			cow_draw_hline(p, x + i + 1, x + w - i, y + i, ul_top);
	}

	for (int i = 0; i < max_h; i++) {
		if (br_bot != COW_DRAW_TRANSPARENT_PX)
			cow_draw_hline(p, x + i, x + w - 1 - i, y + h - i,
			    br_bot);
	}
	for (int i = 0; i < max_w; i++) {
		if (br_right != COW_DRAW_TRANSPARENT_PX)
			cow_draw_vline(p, x + w - i, y + i + 1, y + h - i,
			    br_right);
	}
}

void
cow_draw_blit_skip(struct cow_pixbuf *dst, int dx, int dy, const uint32_t *src,
    int src_stride_px, int sx, int sy, int w, int h, uint32_t skip_px)
{
	if (dst == NULL || dst->pixels == NULL || src == NULL)
		return;
	if (w <= 0 || h <= 0)
		return;
	for (int row = 0; row < h; row++) {
		int y = dy + row;
		if (y < 0 || y >= dst->height)
			continue;
		const uint32_t *srow = src + (sy + row) * src_stride_px + sx;
		uint32_t       *drow = dst->pixels + y * dst->stride_px;
		for (int col = 0; col < w; col++) {
			int x = dx + col;
			if (x < 0 || x >= dst->width)
				continue;
			uint32_t px = srow[col];
			if (px != skip_px)
				drow[x] = px;
		}
	}
}
