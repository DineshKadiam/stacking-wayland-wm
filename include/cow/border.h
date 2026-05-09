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
 * The buffer is cleared to transparent and the full frame ring with
 * relief and handle marks is rendered.
 */
void cow_border_style_render(uint32_t *, int, int, int, const struct cow_border_colour *, int, int, int);

#endif
