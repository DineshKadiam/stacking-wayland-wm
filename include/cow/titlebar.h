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
#if !defined(COW_TITLEBAR_H)
#define COW_TITLEBAR_H

#include <stdbool.h>
#include <stdint.h>

#include "cow/config.h"
#include "cow/cow_box.h"

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
int cow_titlebar_compute_thickness(struct cow_font *);

/*
 * Resolve the effective squeeze configuration for 'view'.
 *
 * Walks per-rule overrides first, then falls back to the global
 * cow_config.  Views that are members of a multi-window container
 * always resolve to COW_SQUEEZE_NONE -- squeeze and tab strips do
 * not compose cleanly.  Output is written into '*out'.
 */
void cow_titlebar_effective_squeeze(const struct cow_view *,
    struct cow_titlebar_squeeze *);

/*
 * Return the rectangle, in the view's decoration-buffer coordinate
 * system, where the titlebar is actually painted.  For unsqueezed
 * views this is the full-width strip at the top of the frame.  For
 * squeezed views it is narrower and positioned per the effective
 * justify + offset.
 *
 * Safe to call on any view with title_thickness > 0; returns a
 * zero-size box if the view has no titlebar.
 */
struct cow_box cow_titlebar_squeezed_rect(const struct cow_view *);

/*
 * True if the point (lx, ly), in view-relative coordinates (i.e.
 * lx = pointer_x - view->x, ly = pointer_y - view->y), lies inside
 * the squeezed titlebar rectangle.  Use this instead of the
 * ad-hoc 'ly between -tt-bw and 0 && lx between 0 and content_w'
 * check so that hits in the cleared strip fall through as the
 * user expects.
 */
bool cow_titlebar_hit(const struct cow_view *, int, int);

/*
 * Render the titlebar (background + relief + text + vector buttons) directly
 * into the decoration frame pixel buffer.
 *
 * `pixels` is the full frame buffer (frame_w x frame_h).
 * `stride_px` is stride in uint32_t units.
 * `frame_w` is the total frame width.
 * `border_width` is the border around the frame.
 * `tb_rect` is the titlebar drawing rectangle inside the decoration
 *    buffer (see cow_titlebar_squeezed_rect()).  For unsqueezed views
 *    this spans the full inner width; for squeezed views it is
 *    narrower, and pixels outside this rect (but inside the
 *    full-width strip) are cleared to transparent.
 * `title` is the window title string (can be NULL).
 * `active` selects active or inactive colors.
 * `font` is the pango font to use for title text.
 *
 * `pressed_button`: -1 = none, 0/1/2 = button with sunken frame relief
 * `icon_inverted_button`: -1 = none, 0/1/2 = button with swapped icon relief
 * `title_pressed`: true = title area drawn sunken (during drag)
 *
 * tab_titles: array of n_tabs strings (app_id or title per member).
 *             NULL means single-window mode (normal rendering).
 * n_tabs:     number of tab labels (>1 means container tab strip).
 * active_tab: index of the active member in tab_titles.
 * drop_target: when true, draw a highlight border on the title strip
 *              to indicate this window is the current drop target.
 */
void cow_titlebar_render(uint32_t *, int, int, int, struct cow_box,
    const char *, bool, struct cow_font *, int, int, bool,
    const struct cow_titlebar_config *, const struct cow_container_config *,
    const float *, const float *, bool, const char **, int, int, bool);

#endif
