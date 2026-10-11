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

#include "cow/text.h"

#include <stdbool.h>

static PangoLayout *
text_layout(cairo_t *cr, const PangoFontDescription *font, const char *text,
    char **valid)
{
	*valid = g_utf8_make_valid(text, -1);
	PangoLayout *layout = pango_cairo_create_layout(cr);
	pango_layout_set_font_description(layout, font);
	pango_layout_set_text(layout, *valid, -1);
	pango_layout_set_single_paragraph_mode(layout, true);
	return (layout);
}

void
cow_text_measure(cairo_t *cr, const PangoFontDescription *font,
    const char *text, int *width, int *height)
{
	if (width != NULL)
		*width = 0;
	if (height != NULL)
		*height = 0;
	if (cr == NULL || font == NULL || text == NULL)
		return;

	char		*valid;
	PangoLayout	*layout = text_layout(cr, font, text, &valid);
	pango_layout_get_pixel_size(layout, width, height);
	g_object_unref(layout);
	g_free(valid);
}

int cow_text_baseline(cairo_t *cr, const PangoFontDescription *font,
    const char *text)
{
	char		*valid;
	PangoLayout	*layout = text_layout(cr, font, text, &valid);
	int baseline = pango_layout_get_baseline(layout) / PANGO_SCALE;
	g_object_unref(layout);
	g_free(valid);
	return baseline;
}

void
cow_text_render(cairo_t *cr, const PangoFontDescription *font, double x,
    double y, double width, double height, const char *text,
    PangoAlignment align, int *out_text_width)
{
	if (out_text_width != NULL)
		*out_text_width = 0;
	if (cr == NULL || font == NULL || text == NULL || width <= 0 ||
	    height <= 0)
		return;

	char		*valid;
	PangoLayout	*layout = text_layout(cr, font, text, &valid);
	pango_layout_set_width(layout, (int)(width * PANGO_SCALE));
	pango_layout_set_height(layout, -1);
	pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
	PangoRectangle logical;
	pango_layout_get_pixel_extents(layout, NULL, &logical);
	int text_width = logical.width, text_height = logical.height;
	if (out_text_width != NULL)
		*out_text_width = text_width;

	/* Keep Pango's bidi shaping, but place the resulting line ourselves:
	 * automatic direction can reverse Pango's left/right alignment.
	 */
	int offset = 0;
	if (align == PANGO_ALIGN_RIGHT)
		offset = width - text_width;
	else if (align == PANGO_ALIGN_CENTER)
		offset = (width - text_width) / 2;
	x += offset - logical.x;

	double text_y = y + (height - text_height) / 2.0;
	if (text_y < y)
		text_y = y;
	cairo_move_to(cr, x, text_y);
	pango_cairo_show_layout(cr, layout);

	g_object_unref(layout);
	g_free(valid);
}
