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
#if !defined(COW_COLOUR_H)
#define COW_COLOUR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void cow_colour_convert(float *, uint32_t);

/* Unpack a premultiplied ARGB8888 pixel into straight RGBA components. */
void cow_colour_from_pixel(float *, uint32_t);

void cow_colour_lighten(float *, const float *);

void cow_colour_darken(float *, const float *);

/*
 * Pack straight float[4] RGBA components into a premultiplied ARGB8888
 * pixel, as required by Cairo and Wayland WL_SHM_FORMAT_ARGB8888.
 */
uint32_t cow_colour_to_pixel(const float *);

/*
 * Parse an RGB or RGBA colour literal.  Hexadecimal 0xRRGGBB values are
 * opaque; 0xRRGGBBAA values carry an explicit alpha component.  Decimal
 * and octal values up to 0xffffff are RGB, while larger values are RGBA.
 * Returns true on success.
 */
bool cow_colour_parse(const char *, float *);

/* Format straight RGBA components as 0xRRGGBB or 0xRRGGBBAA. */
void cow_colour_format(char *, size_t, const float *);

enum cow_gradient_mode {
	COW_GRADIENT_NONE,
	COW_GRADIENT_H,
	COW_GRADIENT_V,
};

struct cow_gradient {
	enum cow_gradient_mode mode;
	float		       from[4];
	float		       to[4];
};

bool cow_gradient_parse(struct cow_gradient *, const char *);

/*
 * A "triple" of pre-packed pixels for a single base colour: the colour
 * itself and its standard light/dark shades.  Used everywhere that
 * draws a 3-D bevel from a single source colour.
 */
struct cow_colour_triple {
	uint32_t base;
	uint32_t lt;
	uint32_t dk;
};

void cow_colour_make_triple(struct cow_colour_triple *, const float *);

/*
 * Make shades for colour using reference to choose the Motif brightness
 * class.  This keeps relief shading continuous when colour is one sample
 * from a gradient that crosses a brightness threshold.
 */
void cow_colour_make_triple_for_reference(struct cow_colour_triple *,
    const float *, const float *);

/*
 * Theme constants used by drop-target highlighting.  These are kept in
 * one place so that the various decoration / titlebar / border paths
 * agree on the exact pixel values.
 */
extern const float COW_COLOUR_DROP_ORANGE[4];
extern const float COW_COLOUR_WHITE[4];

#endif
