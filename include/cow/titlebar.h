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

struct cow_box cow_titlebar_squeezed_rect_for(const struct cow_view *, int,
    int);
void cow_titlebar_frame_geometry(const struct cow_view *, int, int, int *, int *,
    int *, int *);
bool cow_titlebar_is_vertical(enum cow_titlebar_position);
const char *cow_titlebar_position_name(enum cow_titlebar_position);

/*
 * True if the point (lx, ly), in view-relative coordinates (i.e.
 * lx = pointer_x - view->x, ly = pointer_y - view->y), lies inside
 * the squeezed titlebar rectangle.  Use this instead of the
 * ad-hoc 'ly between -tt-bw and 0 && lx between 0 and content_w'
 * check so that hits in the cleared strip fall through as the
 * user expects.
 */
bool cow_titlebar_hit(const struct cow_view *, int, int);
bool cow_titlebar_hit_for(const struct cow_view *, int, int, int, int);

struct cow_titlebar_render_ctx {
	uint32_t       *pixels;
	int		stride_px;
	int		frame_w;
	int		frame_h;
	int		border_width;
	enum cow_border_style border_style;
	struct cow_box tb_rect;

	const char     *title;
	bool		active;
	struct cow_font *font;

	int  pressed_button;
	int  icon_inverted_button;
	bool title_pressed;

	const struct cow_titlebar_config	 *tb_cfg;
	const struct cow_container_config *container_cfg;
	const float			 *style_colour;
	const float			 *style_fg;
	bool				  sticky;

	const char **tab_titles;
	int	     n_tabs;
	int	     active_tab;
	bool	     drop_target;
};

void cow_titlebar_render(const struct cow_titlebar_render_ctx *);

#endif
