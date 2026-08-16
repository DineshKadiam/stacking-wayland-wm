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
#include <stdint.h>

#include "cow/text.h"

int
main(void)
{
	cairo_surface_t *surface =
	    cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 160, 48);
	cairo_t *cr = cairo_create(surface);
	PangoFontDescription *font =
	    pango_font_description_from_string("Sans 12");
	const char truncated[] = "title \xf0\x9f";

	cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
	cow_text_render(cr, font, 0, 0, 80, 24, truncated,
	    PANGO_ALIGN_LEFT, NULL);
	cow_text_render(cr, font, 0, 24, 160, 24, "following row",
	    PANGO_ALIGN_LEFT, NULL);

	int failed = 0;
	if (cairo_status(cr) != CAIRO_STATUS_SUCCESS) {
		fprintf(stderr, "invalid UTF-8 poisoned the cairo context: %s\n",
		    cairo_status_to_string(cairo_status(cr)));
		failed = 1;
	}

	cairo_surface_flush(surface);
	uint32_t *pixels = (uint32_t *)cairo_image_surface_get_data(surface);
	int stride = cairo_image_surface_get_stride(surface) / 4;
	int drawn = 0;
	for (int y = 24; y < 48 && !drawn; y++) {
		for (int x = 0; x < 160; x++) {
			if (pixels[y * stride + x] != 0) {
				drawn = 1;
				break;
			}
		}
	}
	if (!drawn) {
		fprintf(stderr, "text following invalid UTF-8 was not drawn\n");
		failed = 1;
	}

	pango_font_description_free(font);
	cairo_destroy(cr);
	cairo_surface_destroy(surface);
	return (failed);
}
