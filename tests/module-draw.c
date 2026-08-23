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
#include <stdint.h>
#include <stdio.h>

#include <cairo.h>

#include "cow/module_draw.h"

int
main(void)
{
	const float colour[4] = { 0.2f, 0.4f, 0.6f, 0.5f };
	cairo_surface_t *surface =
	    cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 16, 16);
	cairo_t *cr = cairo_create(surface);

	cow_module_cairo_fill(cr, colour, 0, 0, 16, 16);
	cow_module_cairo_relief(cr, 0, 0, 16, 16, false, colour);
	cairo_surface_flush(surface);

	uint32_t *pixels =
	    (uint32_t *)cairo_image_surface_get_data(surface);
	int stride = cairo_image_surface_get_stride(surface) / 4;
	uint8_t center_alpha = pixels[8 * stride + 8] >> 24;
	uint8_t edge_alpha = pixels[0] >> 24;
	int failed = center_alpha != 128 || edge_alpha != 128;

	if (failed) {
		fprintf(stderr, "module drawing lost alpha: center=%u edge=%u\n",
		    center_alpha, edge_alpha);
	}
	cairo_destroy(cr);
	cairo_surface_destroy(surface);
	return (failed);
}
