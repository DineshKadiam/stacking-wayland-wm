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
#if !defined(COW_BORDER_H)
#define COW_BORDER_H

#include <stdbool.h>
#include <stdint.h>

struct cow_border_colour;

/*
 * Render an FVWM or MWM style border frame directly into a pixel buffer.
 *
 * This is the adapter for the new decoration system. It calls the same
 * rendering pipeline as the original border_style.c but writes directly
 * into a caller-provided uint32_t buffer instead of creating a wlr_texture.
 *
 * `pixels` must be frame_w * frame_h pixels in ARGB8888 format.
 * `stride_px` is the stride in uint32_t units (typically frame_w).
 * `style` is COW_BORDER_STYLE_FVWM (1) or COW_BORDER_STYLE_MWM (0).
 *
 * `top_left_x` / `top_right_x` restrict the TOP border strip (and its
 * NW/NE corner markers) to the x-range [top_left_x, top_right_x).
 * Normally these are 0 and frame_w.  For a squeezed titlebar they
 * shrink so:
 *   - NW marker relocates to top_left_x (when > 0)
 *   - NE marker relocates to top_right_x (when < frame_w)
 * The left/right side borders continue full-height as usual; the area
 * above the side borders in the squeeze-away region (x < top_left_x
 * or x >= top_right_x, in the top-left/top-right corner cells) is
 * painted with the side border colour instead of the corner colour,
 * so the vertical strip reaches all the way to y=0.
 *
 * The buffer is cleared to transparent and the frame ring with relief
 * and handle marks is rendered.
 */
void cow_border_style_render(uint32_t *pixels, int stride_px, int frame_w,
    int frame_h, const struct cow_border_colour *colors, int border_width,
    int corner_length, int style, int top_left_x, int top_right_x);

#endif
