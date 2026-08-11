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

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "cow/colour.h"

static unsigned int
brightness(uint32_t pixel)
{
	return (2 * ((pixel >> 16) & 0xff) + 3 * ((pixel >> 8) & 0xff) +
	    (pixel & 0xff));
}

static int
near(float actual, float expected)
{
	return fabsf(actual - expected) < 0.005f;
}

static int
test_parse(void)
{
	float c[4];

	if (!cow_colour_parse("0x112233", c) ||
	    !near(c[0], 0x11 / 255.0f) || !near(c[1], 0x22 / 255.0f) ||
	    !near(c[2], 0x33 / 255.0f) || !near(c[3], 1.0f)) {
		fprintf(stderr, "RGB colour parsing failed\n");
		return (1);
	}
	if (!cow_colour_parse("0x11223380", c) ||
	    !near(c[0], 0x11 / 255.0f) || !near(c[1], 0x22 / 255.0f) ||
	    !near(c[2], 0x33 / 255.0f) || !near(c[3], 0x80 / 255.0f)) {
		fprintf(stderr, "RGBA colour parsing failed\n");
		return (1);
	}
	if (cow_colour_parse("0x112233zz", c) ||
	    cow_colour_parse("0x1234567", c) ||
	    cow_colour_parse("0x100000000", c)) {
		fprintf(stderr, "invalid colour accepted\n");
		return (1);
	}

	return (0);
}

static int
test_pixel(void)
{
	const float in[4] = { 1.0f, 0.5f, 0.0f, 0.5f };
	float	    out[4];
	uint32_t    pixel = cow_colour_to_pixel(in);

	if (pixel != 0x80804000) {
		fprintf(stderr, "RGBA pixel is not premultiplied: 0x%08x\n",
		    pixel);
		return (1);
	}
	cow_colour_from_pixel(out, pixel);
	if (!near(out[0], in[0]) || !near(out[1], in[1]) ||
	    !near(out[2], in[2]) || !near(out[3], in[3])) {
		fprintf(stderr, "RGBA pixel round trip failed\n");
		return (1);
	}

	return (0);
}

int
main(void)
{
	if (test_parse() != 0 || test_pixel() != 0)
		return (1);

	const float near_threshold[4] = {
		0xea / 255.0f, 0xda / 255.0f, 0xb1 / 255.0f, 1.0f,
	};
	const float over_threshold[4] = {
		0xeb / 255.0f, 0xdb / 255.0f, 0xb2 / 255.0f, 1.0f,
	};
	const float gradient_average[4] = {
		0xc9 / 255.0f, 0xba / 255.0f, 0x9b / 255.0f, 1.0f,
	};
	struct cow_colour_triple near_ordinary, over_ordinary;
	struct cow_colour_triple near_gradient, over_gradient;

	cow_colour_make_triple(&near_ordinary, near_threshold);
	cow_colour_make_triple(&over_ordinary, over_threshold);
	cow_colour_make_triple_for_reference(&near_gradient, near_threshold,
	    gradient_average);
	cow_colour_make_triple_for_reference(&over_gradient, over_threshold,
	    gradient_average);

	/* Ordinary Motif shading flips at 85%; the gradient path must not. */
	if (brightness(over_ordinary.lt) >= brightness(near_ordinary.lt)) {
		fprintf(stderr, "test colours no longer cross the light threshold\n");
		return (1);
	}
	if (brightness(over_gradient.lt) < brightness(near_gradient.lt)) {
		fprintf(stderr, "gradient highlight is discontinuous\n");
		return (1);
	}
	if (brightness(over_gradient.lt) <= brightness(over_gradient.base)) {
		fprintf(stderr, "gradient highlight became darker than its base\n");
		return (1);
	}

	return (0);
}
