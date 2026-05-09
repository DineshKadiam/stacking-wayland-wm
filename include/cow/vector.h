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
#if !defined(COW_VECTOR_H)
#define COW_VECTOR_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Maximum number of vector points in a button definition.
 */
#define COW_VECTOR_MAX_POINTS 32

/*
 * Vector button colour indices (matching FVWM3):
 *   0 = shadow (dark)
 *   1 = highlight (light)
 *   2 = background
 *   3 = foreground
 *   4 = invisible (pen move, no line drawn)
 */
enum cow_vector_colour {
	COW_VECTOR_SHADOW    = 0,
	COW_VECTOR_HIGHLIGHT = 1,
	COW_VECTOR_BG	     = 2,
	COW_VECTOR_FG	     = 3,
	COW_VECTOR_INVISIBLE = 4,
};

/*
 * A point in a vector button definition.
 * Coordinates are in a 0-100 abstract space, scaled to the actual
 * button size at render time.
 */
struct cow_vector_point {
	int x;	    /* 0-100 */
	int y;	    /* 0-100 */
	int xoff;   /* pixel offset (+ or -) */
	int yoff;   /* pixel offset (+ or -) */
	int colour; /* cow_vector_colour index, or -1 for pen move */
};

/*
 * A vector button definition: a series of connected line segments
 * drawn in a 0-100 coordinate space.
 */
struct cow_vector_button {
	struct cow_vector_point points[COW_VECTOR_MAX_POINTS];
	int			num_points;
};

/*
 * Draw a vector button into a pixel buffer.
 *
 * `pixels` is a buffer of size w x h with stride `stride` (in uint32_t units).
 * `colors` is a 5-element array indexed by cow_vector_colour.
 * When `pressed` is true, shadow and highlight colors are swapped.
 * The button size `btn_size` is used to scale the 0-100 coordinates.
 */
void cow_vector_button_draw(const struct cow_vector_button *, uint32_t *, int, int, int, int, const uint32_t[static 5], bool);

#endif
