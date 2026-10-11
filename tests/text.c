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

static int
check_alignment(const PangoFontDescription *font, const char *text, int width)
{
	cairo_surface_t *surface =
	    cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width + 16, 120);
	cairo_t *cr = cairo_create(surface);
	cairo_set_source_rgb(cr, 1, 1, 1);
	const PangoAlignment alignments[] = {
		PANGO_ALIGN_LEFT, PANGO_ALIGN_CENTER, PANGO_ALIGN_RIGHT
	};
	int text_width;
	for (int i = 0; i < 3; i++)
		cow_text_render(cr, font, 8, 40 * i, width, 40, text,
		    alignments[i], &text_width);
	cairo_surface_flush(surface);
	uint32_t *pixels = (uint32_t *)cairo_image_surface_get_data(surface);
	int stride = cairo_image_surface_get_stride(surface) / 4;
	int first[3] = { width, width, width }, last[3] = { -1, -1, -1 };
	for (int i = 0; i < 3; i++) {
		for (int y = 40 * i; y < 40 * (i + 1); y++) {
			for (int x = 0; x < width + 16; x++) {
				if (pixels[y * stride + x] == 0)
					continue;
				if (x < first[i])
					first[i] = x;
				if (x > last[i])
					last[i] = x;
			}
		}
	}
	int failed = last[0] < 0 || first[0] > 16;
	for (int i = 1; i < 3; i++) {
		int shift = i == 1 ? (width - text_width) / 2 : width - text_width;
		if (first[i] - first[0] != shift || last[i] - last[0] != shift)
			failed = 1;
	}
	if (failed)
		fprintf(stderr, "incorrect physical alignment: %s (%d/%d: %d..%d %d..%d %d..%d)\n",
		    text, text_width, width, first[0], last[0], first[1], last[1],
		    first[2], last[2]);
	cairo_destroy(cr);
	cairo_surface_destroy(surface);
	return failed;
}

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

	const char *titles[] = { "Terminal", "مرحبا بالعالم", "שלום עולם",
		"مرحبا Terminal 123", "123 Terminal مرحبا" };
	for (size_t i = 0; i < sizeof(titles) / sizeof(titles[0]); i++) {
		failed |= check_alignment(font, titles[i], 320);
		failed |= check_alignment(font, titles[i], 60);
	}
	pango_font_description_free(font);
	cairo_destroy(cr);
	cairo_surface_destroy(surface);
	return (failed);
}
