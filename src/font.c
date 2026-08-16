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
#include "cow/font.h"
#include "cow/log.h"
#include "cow/text.h"

#include <cairo/cairo.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "cow/draw.h"

static uint32_t
font_blend_over(uint32_t dst, uint32_t src)
{
	uint32_t a = (src >> 24) & 0xff;

	if (a == 0)
		return (dst);
	if (a == 0xff)
		return (src);

	uint32_t inv = 0xff - a;
	uint32_t sr = (src >> 16) & 0xff;
	uint32_t sg = (src >> 8) & 0xff;
	uint32_t sb = src & 0xff;
	uint32_t da = (dst >> 24) & 0xff;
	uint32_t dr = (dst >> 16) & 0xff;
	uint32_t dg = (dst >> 8) & 0xff;
	uint32_t db = dst & 0xff;

	uint32_t out_a = a + (da * inv + 127) / 255;
	uint32_t r = sr + (dr * inv + 127) / 255;
	uint32_t g = sg + (dg * inv + 127) / 255;
	uint32_t b = sb + (db * inv + 127) / 255;

	if (r > 0xff)
		r = 0xff;
	if (g > 0xff)
		g = 0xff;
	if (b > 0xff)
		b = 0xff;
	return (out_a << 24) | (r << 16) | (g << 8) | b;
}

static void
font_metrics(struct cow_font *font, const char *text, int *width, int *height)
{
	cairo_surface_t *surface =
	    cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 0, 0);

	cairo_t	    *cairo  = cairo_create(surface);
	cow_text_measure(cairo, font->desc, text, width, height);
	cairo_surface_destroy(surface);
	cairo_destroy(cairo);
}

void
cow_font_init(struct cow_font *font, const char *font_name)
{
	font->desc = pango_font_description_from_string(font_name);
	if (font->desc == NULL) {
		cow_log_warn("invalid font description '%s'; "
		    "falling back to 'monospace 10'",
		    font_name ? font_name : "(null)");
		font->desc = pango_font_description_from_string("monospace 10");
	}

	font_metrics(font, "W", &font->character_width, &font->height);

	/*
	 * Precalculate height for indicator bars since this is the only
	 * height we need.
	 */
	font->height += 8;
}

void
cow_font_fini(struct cow_font *font)
{
	pango_font_description_free(font->desc);
}

int
cow_font_text_width(struct cow_font *font, const char *text)
{
	int w = 0, h = 0;

	if (text == NULL || text[0] == '\0')
		return 0;
	font_metrics(font, text, &w, &h);
	return w;
}

void
cow_font_render_label(struct cow_pixbuf *dst, int dx, int dy, int w, int h,
    struct cow_font *font, const char *text, const float bg[4],
    const float fg[4], int reserve_px, PangoAlignment align, int *out_text_w)
{
	if (out_text_w != NULL)
		*out_text_w = 0;
	if (dst == NULL || dst->pixels == NULL || font == NULL || text == NULL)
		return;
	if (w <= 0 || h <= 0)
		return;

	cairo_surface_t *surface =
	    cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
	cairo_t *cr = cairo_create(surface);

	bool transparent_bg = bg[3] == 0.0f;
	if (!transparent_bg) {
		cairo_set_source_rgba(cr, bg[0], bg[1], bg[2], bg[3]);
		cairo_paint(cr);
	}

	int text_inset = 2 + reserve_px;
	if (text_inset < 0)
		text_inset = 0;

	int avail_w = w - 2 * text_inset;
	cairo_set_source_rgba(cr, fg[0], fg[1], fg[2], fg[3]);
	cow_text_render(cr, font->desc, text_inset, 0, avail_w, h, text, align,
	    out_text_w);
	cairo_surface_flush(surface);

	unsigned char *sdata   = cairo_image_surface_get_data(surface);
	int	       sstride = cairo_image_surface_get_stride(surface) / 4;
	int	       reserve = reserve_px > 0 ? reserve_px : 0;

	for (int py = reserve; py < h - reserve; py++) {
		int y = dy + py;
		if (y < 0 || y >= dst->height)
			continue;
		uint32_t *drow = dst->pixels + y * dst->stride_px;
		for (int px = reserve; px < w - reserve; px++) {
			int x = dx + px;
			if (x < 0 || x >= dst->width)
				continue;
			uint32_t c = ((uint32_t *)sdata)[py * sstride + px];
			if (transparent_bg)
				drow[x] = font_blend_over(drow[x], c);
			else
				drow[x] = c;
		}
	}

	cairo_destroy(cr);
	cairo_surface_destroy(surface);
}
