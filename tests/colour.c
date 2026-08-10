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
#include <stdlib.h>

#include "cow/colour.h"

static unsigned int
brightness(uint32_t pixel)
{
	return (2 * ((pixel >> 16) & 0xff) + 3 * ((pixel >> 8) & 0xff) +
	    (pixel & 0xff));
}

int
main(void)
{
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
