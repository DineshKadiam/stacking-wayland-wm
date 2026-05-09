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
#if !defined(COW_FONT_H)
#define COW_FONT_H

#include <pango/pangocairo.h>

struct cow_font {
	PangoFontDescription *desc;

	int height;
	int character_width;
};

void cow_font_init(struct cow_font *font, const char *font_name);

void cow_font_fini(struct cow_font *font);

/*
 * Return the pango-rendered pixel width of 'text' in 'font'.
 * Returns 0 if text is NULL or empty.
 */
int cow_font_text_width(struct cow_font *font, const char *text);

/*
 * Render 'text' centred (with end-ellipsis) inside a w x h temporary
 * ARGB32 surface filled with 'bg', then composite the resulting glyphs
 * into 'dst' starting at (dx, dy).  Pixels with alpha=0 in the
 * intermediate surface are skipped, and the 'reserve_px' border of dst
 * (where the bevel sits) is preserved.
 *
 * Sets *out_text_w (when non-NULL) to the pixel-width pango actually
 * rendered, useful for callers that need to know how much horizontal
 * space the glyphs occupied.
 *
 * No-op if any of: dst NULL, font NULL, w <= 0, h <= 0, text NULL.
 */
struct cow_pixbuf;
void cow_font_render_label(struct cow_pixbuf *dst, int dx, int dy, int w, int h,
    struct cow_font *font, const char *text, const float bg[4],
    const float fg[4], int reserve_px, int *out_text_w);

#endif
