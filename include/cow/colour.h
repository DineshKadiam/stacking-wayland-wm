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
#include <stdint.h>

void cow_colour_convert(float dst[static 4], uint32_t colour);

void cow_colour_lighten(float dst[static 4], const float src[static 4]);

void cow_colour_darken(float dst[static 4], const float src[static 4]);

/*
 * Pack a float[4] RGBA colour to a uint32_t ARGB8888 pixel.
 */
uint32_t cow_colour_to_pixel(const float colour[static 4]);

/*
 * Parse a colour literal (decimal, 0-prefixed octal or 0x-prefixed hex)
 * and store the unpacked RGBA components in out.  Returns true on
 * success, false if str contains no parseable digits.
 */
bool cow_colour_parse(const char *str, float out[static 4]);

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

void cow_colour_make_triple(struct cow_colour_triple *out,
    const float					      colour[static 4]);

/*
 * Theme constants used by drop-target highlighting.  These are kept in
 * one place so that the various decoration / titlebar / border paths
 * agree on the exact pixel values.
 */
extern const float COW_COLOUR_DROP_ORANGE[4];
extern const float COW_COLOUR_WHITE[4];

#endif
