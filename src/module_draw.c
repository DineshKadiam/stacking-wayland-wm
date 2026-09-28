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
#include "cow/module_draw.h"

#include "cow/colour.h"

void
cow_module_cairo_set_source(cairo_t *cr, const float colour[4])
{
	cairo_set_source_rgba(cr, colour[0], colour[1], colour[2], colour[3]);
}

void
cow_module_cairo_fill(cairo_t *cr, const float colour[4], double x,
    double y, double width, double height)
{
	cairo_save(cr);
	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
	cow_module_cairo_set_source(cr, colour);
	cairo_rectangle(cr, x, y, width, height);
	cairo_fill(cr);
	cairo_restore(cr);
}

void
cow_module_cairo_relief(cairo_t *cr, double x, double y, double width,
    double height, bool sunken, const float base[4])
{
	float dark[4], light[4];

	cow_colour_darken(dark, base);
	cow_colour_lighten(light, base);
	cairo_save(cr);
	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
	cow_module_cairo_set_source(cr, sunken ? dark : light);
	cairo_move_to(cr, x + 0.5, y + height - 0.5);
	cairo_line_to(cr, x + 0.5, y + 0.5);
	cairo_line_to(cr, x + width - 0.5, y + 0.5);
	cairo_stroke(cr);

	cow_module_cairo_set_source(cr, sunken ? light : dark);
	cairo_move_to(cr, x + width - 0.5, y + 0.5);
	cairo_line_to(cr, x + width - 0.5, y + height - 0.5);
	cairo_line_to(cr, x + 0.5, y + height - 0.5);
	cairo_stroke(cr);
	cairo_restore(cr);
}

/* Signed frame widths reverse the relief, not the pressed colours. */
void
cow_module_cairo_frame(cairo_t *cr, double x, double y, double width,
    double height, int frame, bool pressed, const float base[4])
{
	if (frame == 0 || width <= 0 || height <= 0)
		return;
	/* Convert before negation so INT_MIN is safe. */
	double thickness = frame < 0 ? -(double)frame : frame;
	if (thickness > width / 2)
		thickness = width / 2;
	if (thickness > height / 2)
		thickness = height / 2;
	bool  sunken = pressed != (frame < 0);
	float dark[4], light[4];
	cow_colour_darken(dark, base);
	cow_colour_lighten(light, base);
	cairo_save(cr);
	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
	cow_module_cairo_set_source(cr, sunken ? dark : light);
	cairo_move_to(cr, x, y + height);
	cairo_line_to(cr, x, y);
	cairo_line_to(cr, x + width, y);
	cairo_line_to(cr, x + width - thickness, y + thickness);
	cairo_line_to(cr, x + thickness, y + thickness);
	cairo_line_to(cr, x + thickness, y + height - thickness);
	cairo_close_path(cr);
	cairo_fill(cr);
	cow_module_cairo_set_source(cr, sunken ? light : dark);
	cairo_move_to(cr, x + width, y);
	cairo_line_to(cr, x + width, y + height);
	cairo_line_to(cr, x, y + height);
	cairo_line_to(cr, x + thickness, y + height - thickness);
	cairo_line_to(cr, x + width - thickness, y + height - thickness);
	cairo_line_to(cr, x + width - thickness, y + thickness);
	cairo_close_path(cr);
	cairo_fill(cr);
	cairo_restore(cr);
}
