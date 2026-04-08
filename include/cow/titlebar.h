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
#if !defined(COW_TITLEBAR_NEW_H)
#define COW_TITLEBAR_NEW_H

#include <stdbool.h>
#include <stdint.h>

struct cow_font;
struct cow_view;

/*
 * Titlebar constants.
 */
#define COW_TITLEBAR_PADDING 4
#define COW_TITLEBAR_NUM_BUTTONS 3
#define COW_TITLEBAR_LEFT_BUTTONS 1
#define COW_TITLEBAR_RIGHT_BUTTONS 2

/*
 * Compute titlebar thickness from a font.
 */
int cow_titlebar_compute_thickness(struct cow_font *font);

/*
 * Render the titlebar (background + relief + text + vector buttons) directly
 * into the decoration frame pixel buffer.
 *
 * `pixels` is the full frame buffer (frame_w x frame_h).
 * `stride_px` is stride in uint32_t units.
 * `border_width` is the border around the frame.
 * `title_thickness` is the height of the titlebar area.
 * `frame_w` is the total frame width.
 * `title` is the window title string (can be NULL).
 * `active` selects active or inactive colors.
 * `font` is the pango font to use for title text.
 */
/*
 * `pressed_button`: -1 = none, 0/1/2 = button with sunken frame relief
 * `icon_inverted_button`: -1 = none, 0/1/2 = button with swapped icon relief
 * `title_pressed`: true = title area drawn sunken (during drag)
 */
/*
 * tab_titles: array of n_tabs strings (app_id or title per member).
 *             NULL means single-window mode (normal rendering).
 * n_tabs:     number of tab labels (>1 means container tab strip).
 * active_tab: index of the active member in tab_titles.
 * drop_target: when true, draw a highlight border on the title strip
 *              to indicate this window is the current drop target.
 */
void cow_titlebar_render(uint32_t *pixels, int stride_px, int frame_w,
                         int border_width, int title_thickness,
                         const char *title, bool active, struct cow_font *font,
                         int pressed_button, int icon_inverted_button,
                         bool title_pressed, const float *style_colour,
                         const float *style_fg, bool sticky,
                         const char **tab_titles, int n_tabs, int active_tab,
                         bool drop_target);

#endif
