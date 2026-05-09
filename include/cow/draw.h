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
#if !defined(COW_DRAW_H)
#define COW_DRAW_H

#include <stdbool.h>
#include <stdint.h>

/*
 * 2-D ARGB32 software pixel buffer.
 *
 * 'pixels' points at the top-left of a w x h raster.  Stride is
 * expressed in pixels (not bytes) — i.e. row y starts at
 * pixels + y * stride_px.  The drawing primitives below all clip to
 * (0, 0)..(width, height) and silently ignore out-of-range coords.
 */
struct cow_pixbuf {
	uint32_t *pixels;
	int	  stride_px;
	int	  width;
	int	  height;
};

/* Sentinel: a "skip this layer" colour for relieve_rect_per_edge. */
#define COW_DRAW_TRANSPARENT_PX 0xFFFFFFFFu

/* Horizontal line of 'colour' at row y, columns [x1, x2] inclusive. */
void cow_draw_hline(struct cow_pixbuf *, int, int, int, uint32_t);

/* Vertical line of 'colour' at column x, rows [y1, y2] inclusive. */
void cow_draw_vline(struct cow_pixbuf *, int, int, int, uint32_t);

/* Solid fill of [x, x+w) x [y, y+h). */
void cow_draw_fill_rect(struct cow_pixbuf *, int, int, int, int, uint32_t);

/* Equivalent to cow_draw_fill_rect with colour=0 (transparent). */
void cow_draw_clear_rect(struct cow_pixbuf *, int, int, int, int);

/*
 * Per-edge bevel.  Draws a 'line_width' rectangular ring around
 * (x, y, w, h), with the upper/left edges painted in ul_*, and the
 * lower/right edges painted in br_*.  Pass COW_DRAW_TRANSPARENT_PX for
 * any edge that should be skipped (e.g. when the previous bevel ring
 * "owns" that corner).  The ring is drawn so that the upper-left
 * corners are owned by ul_left/ul_top and the lower-right corners by
 * br_bot/br_right -- this matches FVWM-style relief.
 */
void cow_draw_relieve_rect(struct cow_pixbuf *, int, int, int, int, int, uint32_t, uint32_t, uint32_t, uint32_t);

/*
 * Copy 'w' x 'h' pixels from src (with its own stride in pixels)
 * starting at (sx, sy) into dst at (dx, dy).  Pixels in src equal to
 * 'skip_px' are not written (used for the title-text composite).
 */
void cow_draw_blit_skip(struct cow_pixbuf *, int, int, const uint32_t *, int, int, int, int, int, uint32_t);

#endif /* COW_DRAW_H */
