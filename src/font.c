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

#include <cairo/cairo.h>
#include <stdio.h>
#include <string.h>

static void
font_metrics(struct cow_font *font, const char *text, int *width, int *height)
{
	cairo_surface_t *surface =
	    cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 0, 0);

	cairo_t	    *cairo  = cairo_create(surface);
	PangoLayout *layout = pango_cairo_create_layout(cairo);

	pango_layout_set_font_description(layout, font->desc);
	pango_layout_set_text(layout, text, -1);
	pango_cairo_update_layout(cairo, layout);
	pango_cairo_show_layout(cairo, layout);
	pango_layout_get_pixel_size(layout, width, height);

	g_object_unref(layout);
	cairo_surface_destroy(surface);
	cairo_destroy(cairo);
}

void
cow_font_init(struct cow_font *font, const char *font_name)
{
	font->desc = pango_font_description_from_string(font_name);
	if (font->desc == NULL) {
		fprintf(stderr,
		    "cow: invalid font description '%s'; "
		    "falling back to 'monospace 10'\n",
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
