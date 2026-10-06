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
#include "cow/decoration.h"
#include "cow/log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <wayland-client.h>

#include "cow/border.h"
#include "cow/colour.h"
#include "cow/config.h"
#include "cow/font.h"
#include "cow/memory.h"
#include "cow/pointer_op.h"
#include "cow/shm.h"
#include "cow/titlebar.h"
#include "cow/view.h"
#include "cow/wm.h"

#include "river-window-management-v1-client-protocol.h"

/* Global font for titlebar rendering.
 * Rebuilt whenever the font name changes (tracked via last_font_name). */
static struct cow_font titlebar_font;
static char	      *last_font_name = NULL;

static void
render_resize_indicator(uint32_t *pixels, int stride_px, int frame_w,
    int frame_h, int border_width, int corner_length, uint32_t edges,
    const float *colour)
{
	uint32_t pixel = cow_colour_to_pixel(colour);
	bool horizontal = (edges & (COW_EDGE_TOP | COW_EDGE_BOTTOM)) != 0;
	bool vertical = (edges & (COW_EDGE_LEFT | COW_EDGE_RIGHT)) != 0;
	bool corner = horizontal && vertical;
	int corner_x = corner_length < border_width ? border_width : corner_length;
	int corner_y = corner_x;

	if (corner_x > frame_w / 2)
		corner_x = frame_w / 2;
	if (corner_y > frame_h / 2)
		corner_y = frame_h / 2;

	for (int y = 0; y < frame_h; y++) {
		bool in_horizontal_border = ((edges & COW_EDGE_TOP) != 0 &&
		    y < border_width) || ((edges & COW_EDGE_BOTTOM) != 0 &&
		    y >= frame_h - border_width);

		for (int x = 0; x < frame_w; x++) {
			bool in_horizontal_span = corner ?
			    (((edges & COW_EDGE_LEFT) != 0 && x < corner_x) ||
				((edges & COW_EDGE_RIGHT) != 0 &&
				    x >= frame_w - corner_x)) :
			    (x >= corner_x && x < frame_w - corner_x);
			bool in_vertical_border = ((edges & COW_EDGE_LEFT) != 0 &&
			    x < border_width) ||
			    ((edges & COW_EDGE_RIGHT) != 0 &&
				x >= frame_w - border_width);
			bool in_vertical_span = corner ?
			    (((edges & COW_EDGE_TOP) != 0 && y < corner_y) ||
				((edges & COW_EDGE_BOTTOM) != 0 &&
				    y >= frame_h - corner_y)) :
			    (y >= corner_y && y < frame_h - corner_y);

			if ((in_horizontal_border && in_horizontal_span) ||
			    (in_vertical_border && in_vertical_span))
				pixels[y * stride_px + x] = pixel;
		}
	}
}

static struct cow_font *
get_titlebar_font(void)
{
	const char *name = (cow_config && cow_config->font_name) ?
	    cow_config->font_name :
	    "monospace 10";

	/* Reinitialise whenever the configured font name changes. */
	if (last_font_name == NULL || strcmp(last_font_name, name) != 0) {
		if (last_font_name != NULL)
			cow_font_fini(&titlebar_font);
		cow_font_init(&titlebar_font, name);
		xstrset(&last_font_name, name);
	}

	return &titlebar_font;
}

static char *
make_display_title(const struct cow_view *view)
{
	const char *fmt = view->cfg.title_format;
	struct cow_container_pane *pane = cow_view_pane(view);
	const char *pane_name = pane && pane->name ? pane->name : "";
	if (cow_container_is_split(view) && !cow_container_window_owner(view)) {
		const char *pane_fmt = view->cfg.pane_title_format;
		if (pane_fmt && strcmp(pane_fmt, "auto") != 0)
			fmt = pane_fmt;
		else if (*pane_name)
			fmt = view->container_placeholder ? "%p" : "[%p] %n";
		else if (view->container_placeholder)
			fmt = "%n";
	}
	return cow_view_make_display_title(view, fmt);
}

static void
restore_shaded_side_borders(uint32_t *pixels, int stride_px, int frame_w,
    int frame_h, const struct cow_border_colour *bc, int bw, int style)
{
	size_t	  nbytes = (size_t)stride_px * (size_t)frame_h * 4;
	uint32_t *scratch = xmalloc(nbytes);

	cow_border_style_render(scratch, stride_px, frame_w, frame_h, bc, bw, 0,
	    style);

	int right_x = frame_w - bw;
	if (right_x < 0)
		right_x = 0;
	if (right_x + bw > frame_w)
		right_x = frame_w - bw;

	for (int y = bw; y < frame_h - bw; y++) {
		memcpy(&pixels[y * stride_px], &scratch[y * stride_px],
		    (size_t)bw * 4);
		memcpy(&pixels[y * stride_px + right_x],
		    &scratch[y * stride_px + frame_w - bw], (size_t)bw * 4);
	}

	xfree(scratch);
}

struct cow_font *
cow_decoration_titlebar_font(void)
{
	return get_titlebar_font();
}

char *
cow_decoration_display_title(const struct cow_view *view)
{
	return make_display_title(view);
}

struct squeeze_paint_ctx {
	uint32_t *buf;
	int	  stride_px;
	int	  frame_w;
	int	  frame_h;
	int	  bw;
	int	  cl;
	int	  tt;
	int	  top_band;
	int	  marker_w;
	int	  tbr_near_x;
	bool	  left_side;
	const float *corner_colour;
	const struct cow_border_colour *bc;
	const uint32_t			*scratch;
	enum cow_border_style		 border_style;
};

/*
 * Shared post-processing for one side of a squeezed titlebar.
 *
 * 'left_side' is true when we're painting the titlebar's LEFT edge
 * (the relocated NW) and false for the titlebar's RIGHT edge (the
 * relocated NE).  LEFT-justify calls this once with left_side=false,
 * RIGHT-justify once with left_side=true, and CENTRE calls both.
 */
static void
squeeze_paint_side(const struct squeeze_paint_ctx *ctx)
{
	uint32_t			    *buf = ctx->buf;
	int			     stride_px = ctx->stride_px;
	int			     frame_w = ctx->frame_w;
	int			     frame_h = ctx->frame_h;
	int			     bw = ctx->bw;
	int			     cl = ctx->cl;
	int			     tt = ctx->tt;
	int			     top_band = ctx->top_band;
	int			     marker_w = ctx->marker_w;
	int			     tbr_near_x = ctx->tbr_near_x;
	bool			     left_side = ctx->left_side;
	const float		    *corner_colour = ctx->corner_colour;
	const struct cow_border_colour *bc = ctx->bc;
	const uint32_t		    *scratch = ctx->scratch;
	enum cow_border_style	     border_style = ctx->border_style;

	/*
	 * Geometry per side:
	 *
	 *   RIGHT side (left_side=false, e.g. LEFT/CENTRE justify):
	 *     corner apex at tbr_near_x (= tbr.x + tbr.width).
	 *     L opens to the right (┐), side arm flush at apex.
	 *     Stepped N border at y ∈ [tt, tt+bw), x ∈ [apex+bw, frame_w).
	 *
	 *   LEFT side  (left_side=true, e.g. RIGHT/CENTRE justify):
	 *     corner apex at tbr_near_x (= tbr.x).
	 *     L opens to the left  (┌), side arm flush at apex.
	 *     Stepped N border at y ∈ [tt, tt+bw), x ∈ [0, apex-bw).
	 */

	int seg_x0, seg_x1;
	int mx;
	int clr_x0, clr_x1;
	if (!left_side) {
		mx     = tbr_near_x + bw - marker_w;
		seg_x0 = mx + marker_w;
		seg_x1 = frame_w;
		clr_x0 = tbr_near_x;
		clr_x1 = frame_w;
	} else {
		mx     = tbr_near_x - bw;
		seg_x0 = 0;
		seg_x1 = mx;
		clr_x0 = 0;
		clr_x1 = tbr_near_x;
	}

	/* Clear the top-border strip past the titlebar edge. */
	for (int y = 0; y < bw && y < frame_h; y++) {
		for (int x = clr_x0; x < clr_x1 && x < frame_w; x++) {
			if (x < 0)
				continue;
			buf[y * stride_px + x] = 0;
		}
	}

	/*
	 * Erase the frame's original NW/NE corner L-shape so it doesn't
	 * peek up above the stepped-down border.  Only wipe the two
	 * arm-strips of the L (the top-strip at y<bw and the side-strip
	 * at x<bw or x>=frame_w-bw), NOT the whole cl x cl bounding
	 * box -- the interior of that box is content owned by the
	 * titlebar (e.g. its left/right-most button when the squeeze
	 * places the titlebar close to the frame edge) and wiping it
	 * would erase buttons rendered by cow_titlebar_render.
	 */
	if (cl > 0) {
		int side_x0 = !left_side ? (frame_w - bw) : 0;
		int side_x1 = !left_side ? frame_w : bw;
		int top_x0  = !left_side ? (frame_w - cl) : 0;
		int top_x1  = !left_side ? frame_w : cl;
		/* Side strip of the L (x-column(s) at the frame edge). */
		for (int y = 0; y < cl && y < frame_h; y++) {
			for (int x = side_x0; x < side_x1 && x < frame_w; x++) {
				if (x < 0)
					continue;
				buf[y * stride_px + x] = 0;
			}
		}
		/* Top strip of the L (at y < bw). */
		for (int y = 0; y < bw && y < frame_h; y++) {
			for (int x = top_x0; x < top_x1 && x < frame_w; x++) {
				if (x < 0)
					continue;
				buf[y * stride_px + x] = 0;
			}
		}
	}
	if (cl == 0) {
		int side_x0 = !left_side ? (frame_w - bw) : 0;
		int side_x1 = !left_side ? frame_w : bw;
		for (int y = 0; y < top_band && y < frame_h; y++) {
			for (int x = side_x0; x < side_x1 && x < frame_w; x++) {
				if (x < 0)
					continue;
				buf[y * stride_px + x] = 0;
			}
		}
	}

	/*
	 * Render a mini scratch frame with the corner colour stuffed
	 * into NE (right-side) or NW (left-side) slot, then blit just
	 * the L-shape (top arm + side arm).
	 */
	/*
	 * Mirror the caller's has_nw / has_ne.  When the user has NOT
	 * set an explicit corner colour, we leave has_* false so the
	 * mini-frame renders the corner with edge gradients (matching
	 * the rest of the border).  Otherwise fill the slot with the
	 * user's explicit colour and set has_* so the mini paints a
	 * solid corner swatch matching the frame's fixed corners.
	 */
	struct cow_border_colour cm_bc;
	memset(&cm_bc, 0, sizeof(cm_bc));
	memcpy(cm_bc.n, bc->n, sizeof(cm_bc.n));
	memcpy(cm_bc.s, bc->s, sizeof(cm_bc.s));
	memcpy(cm_bc.e, bc->e, sizeof(cm_bc.e));
	memcpy(cm_bc.w, bc->w, sizeof(cm_bc.w));
	if (!left_side) {
		if (bc->has_ne) {
			memcpy(cm_bc.ne, corner_colour, sizeof(cm_bc.ne));
			cm_bc.has_ne = true;
		}
	} else {
		if (bc->has_nw) {
			memcpy(cm_bc.nw, corner_colour, sizeof(cm_bc.nw));
			cm_bc.has_nw = true;
		}
	}

	int	  mw	   = marker_w;
	int	  mh	   = top_band;
	int	  mini_w   = mw * 4;
	int	  mini_h   = (mh > mw ? mh : mw) * 4;
	int	  mini_str = mini_w;
	uint32_t *mini = xcalloc((size_t)mini_str * mini_h, sizeof(uint32_t));
	cow_border_style_render(mini, mini_str, mini_w, mini_h,
	    &cm_bc, bw, cl, border_style);

	int src_x0 = !left_side ? (mini_w - mw) : 0;
	/* Marks straddle the corner boundary: one relief pixel in MWM,
	 * two in FVWM. Include the neighbouring edge's half as well.
	 */
	int bevel = cl > bw ?
	    (border_style == COW_BORDER_STYLE_MWM ? 1 :
	    border_style == COW_BORDER_STYLE_FVWM ? 2 : 0) : 0;
	int col_start = left_side ? 0 : -bevel;
	int col_end = mw + (left_side ? bevel : 0);

	/* Top arm: bw rows along the top. */
	for (int row = 0; row < bw && row < mh; row++) {
		for (int col = col_start; col < col_end; col++) {
			int dx = mx + col;
			if (dx < 0 || dx >= frame_w)
				continue;
			uint32_t src = mini[row * mini_str + (src_x0 + col)];
			if ((src >> 24) == 0)
				continue;
			buf[row * stride_px + dx] = src;
		}
	}
	/* Fit the side mark and both halves of its relief before the client
	 * starts, even when the configured corner is taller than the title.
	 */
	int side_offset = bevel > 0 ? cl - mh + bevel : 0;
	if (side_offset < 0)
		side_offset = 0;
	int arm_col = !left_side ? (mw - bw) : 0;
	for (int col = arm_col; col < arm_col + bw; col++) {
		for (int row = bw; row < mh; row++) {
			int dx = mx + col;
			if (dx < 0 || dx >= frame_w)
				continue;
			if (row >= frame_h)
				break;
			uint32_t src = mini[(row + side_offset) * mini_str +
			    (src_x0 + col)];
			if ((src >> 24) == 0)
				continue;
			buf[row * stride_px + dx] = src;
		}
	}
	xfree(mini);

	/*
	 * Stepped-down N border: copy y=[0, bw) of the scratch render
	 * down to y=[tt, tt+bw).
	 */
	for (int row = 0; row < bw; row++) {
		int dy = tt + row;
		if (dy >= frame_h)
			break;
		for (int x = seg_x0; x < seg_x1 && x < frame_w; x++) {
			if (x < 0)
				continue;
			buf[dy * stride_px + x] = scratch[row * stride_px + x];
		}
	}

	/*
	 * Relocate the frame's far corner (NE for right-side, NW for
	 * left-side) down by tt pixels, so its vertical return into the
	 * east/west border lands at y=[tt, tt+cls) instead of [0, cls).
	 */
	if (cl > 0) {
		int cb_x0 = !left_side ? (frame_w - cl) : 0;
		int cb_x1 = !left_side ? frame_w : cl;
		for (int row = bw; row < cl; row++) {
			int dy = tt + row;
			if (dy >= frame_h)
				break;
			for (int x = cb_x0; x < cb_x1 && x < frame_w; x++) {
				if (x < 0)
					continue;
				buf[dy * stride_px + x] =
				    scratch[row * stride_px + x];
			}
		}
	}
}

static void
clear_pixels(uint32_t *buf, int stride_px, int frame_w, int frame_h, int x0,
    int y0, int w, int h)
{
	int x1 = x0 + w;
	int y1 = y0 + h;

	if (x0 < 0)
		x0 = 0;
	if (y0 < 0)
		y0 = 0;
	if (x1 > frame_w)
		x1 = frame_w;
	if (y1 > frame_h)
		y1 = frame_h;
	for (int y = y0; y < y1; y++) {
		for (int x = x0; x < x1; x++)
			buf[y * stride_px + x] = 0;
	}
}

static void
squeeze_paint_south_side(const struct squeeze_paint_ctx *ctx)
{
	uint32_t			    *buf = ctx->buf;
	int			     stride_px = ctx->stride_px;
	int			     frame_w = ctx->frame_w;
	int			     frame_h = ctx->frame_h;
	int			     bw = ctx->bw;
	int			     cl = ctx->cl;
	int			     tt = ctx->tt;
	int			     bottom_band = ctx->top_band;
	int			     marker_w = ctx->marker_w;
	int			     tbr_near_x = ctx->tbr_near_x;
	bool			     left_side = ctx->left_side;
	const float		    *corner_colour = ctx->corner_colour;
	const struct cow_border_colour *bc = ctx->bc;
	const uint32_t		    *scratch = ctx->scratch;
	enum cow_border_style	     border_style = ctx->border_style;
	int			     bottom_y = frame_h - bw;
	int			     step_y = frame_h - bw - tt;

	int seg_x0, seg_x1;
	int mx;
	int clr_x0, clr_x1;
	if (!left_side) {
		mx     = tbr_near_x + bw - marker_w;
		seg_x0 = mx + marker_w;
		seg_x1 = frame_w;
		clr_x0 = tbr_near_x;
		clr_x1 = frame_w;
	} else {
		mx     = tbr_near_x - bw;
		seg_x0 = 0;
		seg_x1 = mx;
		clr_x0 = 0;
		clr_x1 = tbr_near_x;
	}

	clear_pixels(buf, stride_px, frame_w, frame_h, clr_x0, bottom_y,
	    clr_x1 - clr_x0, bw);

	if (cl > 0) {
		int side_x0 = !left_side ? (frame_w - bw) : 0;
		int top_x0  = !left_side ? (frame_w - cl) : 0;
		clear_pixels(buf, stride_px, frame_w, frame_h, side_x0,
		    frame_h - cl, bw, cl);
		clear_pixels(buf, stride_px, frame_w, frame_h, top_x0,
		    bottom_y, cl, bw);
	} else {
		int side_x0 = !left_side ? (frame_w - bw) : 0;
		clear_pixels(buf, stride_px, frame_w, frame_h, side_x0,
		    step_y, bw, bw + tt);
	}

	struct cow_border_colour cm_bc;
	memset(&cm_bc, 0, sizeof(cm_bc));
	memcpy(cm_bc.n, bc->n, sizeof(cm_bc.n));
	memcpy(cm_bc.s, bc->s, sizeof(cm_bc.s));
	memcpy(cm_bc.e, bc->e, sizeof(cm_bc.e));
	memcpy(cm_bc.w, bc->w, sizeof(cm_bc.w));
	if (!left_side) {
		if (bc->has_se) {
			memcpy(cm_bc.se, corner_colour, sizeof(cm_bc.se));
			cm_bc.has_se = true;
		}
	} else {
		if (bc->has_sw) {
			memcpy(cm_bc.sw, corner_colour, sizeof(cm_bc.sw));
			cm_bc.has_sw = true;
		}
	}

	int	  mw	   = marker_w;
	int	  mh	   = bottom_band;
	int	  mini_w   = mw * 4;
	int	  mini_h   = mh * 4;
	int	  mini_str = mini_w;
	uint32_t *mini = xcalloc((size_t)mini_str * mini_h, sizeof(uint32_t));
	cow_border_style_render(mini, mini_str, mini_w, mini_h, &cm_bc, bw, cl,
	    border_style);

	int src_x0 = !left_side ? (mini_w - mw) : 0;
	int src_y0 = mini_h - mh;

	for (int row = 0; row < bw && row < mh; row++) {
		int dy = frame_h - bw + row;
		if (dy < 0 || dy >= frame_h)
			continue;
		for (int col = 0; col < mw; col++) {
			int dx = mx + col;
			if (dx < 0 || dx >= frame_w)
				continue;
			uint32_t src =
			    mini[(src_y0 + mh - bw + row) * mini_str +
				(src_x0 + col)];
			if ((src >> 24) == 0)
				continue;
			buf[dy * stride_px + dx] = src;
		}
	}

	int arm_col = !left_side ? (mw - bw) : 0;
	for (int col = arm_col; col < arm_col + bw; col++) {
		for (int row = 0; row < mh - bw; row++) {
			int dx = mx + col;
			int dy = frame_h - mh + row;
			if (dx < 0 || dx >= frame_w || dy < 0 || dy >= frame_h)
				continue;
			uint32_t src =
			    mini[(src_y0 + row) * mini_str + (src_x0 + col)];
			if ((src >> 24) == 0)
				continue;
			buf[dy * stride_px + dx] = src;
		}
	}
	xfree(mini);

	for (int row = 0; row < bw; row++) {
		int dy = step_y + row;
		int sy = frame_h - bw + row;
		if (dy < 0 || dy >= frame_h || sy < 0 || sy >= frame_h)
			continue;
		for (int x = seg_x0; x < seg_x1 && x < frame_w; x++) {
			if (x < 0)
				continue;
			buf[dy * stride_px + x] = scratch[sy * stride_px + x];
		}
	}

	if (cl > 0) {
		int cb_x0 = !left_side ? (frame_w - cl) : 0;
		int cb_x1 = !left_side ? frame_w : cl;
		for (int row = bw; row < cl; row++) {
			int dy = frame_h - tt - row - 1;
			int sy = frame_h - bw - row - 1;
			if (dy < 0 || dy >= frame_h || sy < 0 || sy >= frame_h)
				continue;
			for (int x = cb_x0; x < cb_x1 && x < frame_w; x++) {
				if (x < 0)
					continue;
				buf[dy * stride_px + x] =
				    scratch[sy * stride_px + x];
			}
		}
	}
}

static void
copy_vertical_border(uint32_t *buf, const uint32_t *scratch, int stride_px,
    int src_x, int dst_x, int y0, int y1, int bw, int frame_w, int frame_h)
{
	if (y0 < 0)
		y0 = 0;
	if (y1 > frame_h)
		y1 = frame_h;
	for (int y = y0; y < y1; y++) {
		for (int x = 0; x < bw; x++) {
			int sx = src_x + x;
			int dx = dst_x + x;
			if (sx < 0 || sx >= frame_w || dx < 0 || dx >= frame_w)
				continue;
			buf[y * stride_px + dx] = scratch[y * stride_px + sx];
		}
	}
}

static void
squeeze_paint_vertical_corner(uint32_t *buf, int stride_px, int frame_w,
    int frame_h, int x0, int y0, int bw, int side_w,
    const struct cow_border_colour *bc, enum cow_border_style border_style,
    bool east, bool bottom)
{
	struct cow_border_colour cm_bc;
	memset(&cm_bc, 0, sizeof(cm_bc));
	memcpy(cm_bc.n, bc->n, sizeof(cm_bc.n));
	memcpy(cm_bc.s, bc->s, sizeof(cm_bc.s));
	memcpy(cm_bc.e, bc->e, sizeof(cm_bc.e));
	memcpy(cm_bc.w, bc->w, sizeof(cm_bc.w));
	if (east && bottom && bc->has_se) {
		memcpy(cm_bc.se, bc->se, sizeof(cm_bc.se));
		cm_bc.has_se = true;
	} else if (east && !bottom && bc->has_ne) {
		memcpy(cm_bc.ne, bc->ne, sizeof(cm_bc.ne));
		cm_bc.has_ne = true;
	} else if (!east && bottom && bc->has_sw) {
		memcpy(cm_bc.sw, bc->sw, sizeof(cm_bc.sw));
		cm_bc.has_sw = true;
	} else if (!east && !bottom && bc->has_nw) {
		memcpy(cm_bc.nw, bc->nw, sizeof(cm_bc.nw));
		cm_bc.has_nw = true;
	}

	int	  mini_w = side_w * 4;
	int	  mini_h = side_w * 4;
	int	  mini_s = mini_w;
	uint32_t *mini = xcalloc((size_t)mini_s * mini_h, sizeof(uint32_t));
	cow_border_style_render(mini, mini_s, mini_w, mini_h, &cm_bc, bw,
	    side_w, border_style);

	int src_x = east ? mini_w - side_w : 0;
	int src_y = bottom ? mini_h - side_w : 0;
	for (int y = 0; y < side_w; y++) {
		int dy = y0 + y;
		if (dy < 0 || dy >= frame_h)
			continue;
		for (int x = 0; x < side_w; x++) {
			int dx = x0 + x;
			if (dx < 0 || dx >= frame_w)
				continue;
			uint32_t src = mini[(src_y + y) * mini_s + src_x + x];
			if ((src >> 24) != 0)
				buf[dy * stride_px + dx] = src;
		}
	}
	xfree(mini);
}

static void
squeeze_paint_vertical_side(uint32_t *buf, const uint32_t *scratch,
    int stride_px, int frame_w, int frame_h, int bw, int tt,
    struct cow_box tbr, enum cow_squeeze_just justify, bool east,
    const struct cow_border_colour *bc, enum cow_border_style border_style)
{
	int src_x = east ? frame_w - bw : 0;
	int dst_x = east ? frame_w - bw - tt : tt;
	int clr_x = east ? dst_x : 0;
	int side_w = bw + tt;

	if (justify == COW_JUST_LEFT || justify == COW_JUST_CENTRE) {
		int apex = tbr.y + tbr.height;
		int corner_y = apex - tt;

		clear_pixels(buf, stride_px, frame_w, frame_h, clr_x, apex,
		    side_w, frame_h - apex);
		copy_vertical_border(buf, scratch, stride_px, src_x, dst_x,
		    apex + bw, frame_h, bw, frame_w, frame_h);
		squeeze_paint_vertical_corner(buf, stride_px, frame_w,
		    frame_h, clr_x, corner_y, bw, side_w, bc, border_style,
		    east, true);
	}
	if (justify == COW_JUST_RIGHT || justify == COW_JUST_CENTRE) {
		int apex = tbr.y;
		int corner_y = apex - bw;

		clear_pixels(buf, stride_px, frame_w, frame_h, clr_x, 0,
		    side_w, apex);
		copy_vertical_border(buf, scratch, stride_px, src_x, dst_x,
		    0, apex - bw, bw, frame_w, frame_h);
		squeeze_paint_vertical_corner(buf, stride_px, frame_w,
		    frame_h, clr_x, corner_y, bw, side_w, bc, border_style,
		    east, false);
	}
}

void
cow_decoration_sync_metrics(struct cow_view *view)
{
	struct cow_view_decoration *dec = &view->dec;
	dec->border_width = view->cfg.border;

	int h = view->cfg.titlebar.height;
	if (h > 0) {
		dec->nominal_title_thickness = h;
	} else {
		struct cow_font font;
		const char *name = view->cfg.font_name != NULL ?
		    view->cfg.font_name : "monospace 10";
		cow_font_init(&font, name);
		dec->nominal_title_thickness =
		    cow_titlebar_compute_thickness(&font);
		cow_font_fini(&font);
	}
	dec->title_thickness = cow_container_title_visible(view) ?
	    dec->nominal_title_thickness :
	    0;
}

void
cow_decoration_init(struct cow_view *view)
{
	struct cow_view_decoration *dec = &view->dec;
	memset(dec, 0, sizeof(*dec));
	dec->dirty = true;
	cow_decoration_sync_metrics(view);
}

void
cow_decoration_fini(struct cow_view_decoration *dec)
{
	cow_decoration_tiles_fini(dec);
	if (dec->buffer != NULL) {
		cow_shm_buffer_destroy(dec->buffer);
		dec->buffer = NULL;
	}
}

void
cow_decoration_update(struct cow_view *view, bool active)
{
	struct cow_view_decoration *dec = &view->dec;

	if (!view->use_ssd || !view->has_dimensions)
		return;

	/*
	 * Apply visual settings from the view's per-view cfg snapshot.
	 * cmd-set writes here when a target window is resolved (whether
	 * by an explicit -t or by the rule subsystem fanning out an
	 * event).  Reads on cow_config don't appear in the decoration
	 * code path at all -- a view sees only its own snapshot.
	 */
	cow_decoration_sync_metrics(view);

	int bw	       = dec->border_width;
	int tt	       = dec->title_thickness;
	uint32_t shared_edges = cow_container_shared_edges(view);

	int cw, ch;
	bool shaded = view->sd != COW_SHADE_NONE ||
	    (cow_view_container(view) != NULL &&
		cow_view_container(view)->shaded);
	cow_view_effective_content_size(view, &cw, &ch);

	int frame_w, frame_h, content_x, content_y;
	cow_titlebar_frame_geometry(view, cw, ch, &frame_w, &frame_h,
	    &content_x, &content_y);

	if (frame_w <= 0 || frame_h <= 0)
		return;

	bool was_compact = dec->tiles != NULL;
	bool compact = cow_decoration_tiles_prepare(view, active, frame_w,
	    frame_h);
	/* Keep the original titlebar coordinates, cropping its strip at commit. */
	int buffer_h = compact ? bw + tt : frame_h;

	/* Check if we need to regenerate */
	bool needs_regen = dec->dirty || compact != was_compact ||
	    active != dec->last_active ||
	    cw != dec->last_content_width ||
	    (!compact && ch != dec->last_content_height);
	dec->frame_width = frame_w;
	dec->frame_height = frame_h;

	if (!needs_regen && dec->buffer != NULL)
		goto commit;

	/* Free old buffer if dimensions changed or it's busy */
	if (dec->buffer != NULL &&
	    (dec->buffer->width != frame_w || dec->buffer->height != buffer_h ||
		dec->buffer->busy)) {
		cow_shm_buffer_destroy(dec->buffer);
		dec->buffer = NULL;
	}

	/* Allocate buffer if needed */
	if (dec->buffer == NULL) {
		dec->buffer = cow_shm_buffer_create(frame_w, buffer_h);
		if (dec->buffer == NULL) {
			cow_log_error("failed to create decoration buffer");
			return;
		}
	}

	/* Render the frame using the view's resolved style */
	{
		int stride_px = dec->buffer->stride / 4;

		/* Border colours come from the view's snapshot. */
		const struct cow_border_colour *bc;
		bc = active ? &view->cfg.border_active :
			      &view->cfg.border_inactive;

		/* Render border: fill_border_base always runs for
		 * per-edge active/inactive colors; FVWM/MWM add 3D
		 * relief on top; NONE = flat fill only. */
		int cl = cow_decoration_corner_length(view);
		/*
		 * When this view is a container drop target, override the
		 * entire frame (borders + titlebar) with solid orange so the
		 * user sees a clear highlight indicating where the drop will
		 * land.
		 */
		bool is_drop_target = cow_view_is_drop_target(view);
		struct cow_border_colour drop_bc;
		if (is_drop_target) {
			drop_bc		   = *bc;
			uint32_t edge	   = cow_wm.drag_drop_edge;
			float	*colours[] = { drop_bc.n, drop_bc.s, drop_bc.w,
				  drop_bc.e, drop_bc.nw, drop_bc.ne, drop_bc.sw,
				  drop_bc.se };
			const uint32_t masks[] = { 1, 2, 4, 8, 5, 9, 6, 10 };
			for (size_t i = 0; i < 8; i++)
				if (edge == 0 || (edge & masks[i]) != 0)
					memcpy(colours[i],
					    COW_COLOUR_DROP_ORANGE,
					    sizeof(float) * 4);
			drop_bc.has_nw = drop_bc.has_ne = true;
			drop_bc.has_sw = drop_bc.has_se = true;
			drop_bc.has_corners		= true;
			bc				= &drop_bc;
		}

		if (compact)
			memset(dec->buffer->data, 0, dec->buffer->size);
		else
			cow_border_style_render_pane(dec->buffer->data, stride_px,
			    frame_w, frame_h, bc, bw, cl, view->cfg.border_style,
			    cow_container_outer_edges(view));

		/* Render titlebar if enabled */
		if (tt > 0) {
			struct cow_font *font = get_titlebar_font();
			/* Per-view titlebar colour overrides used to live
			 * here; cow_titlebar_render now picks the colour
			 * from the global config when these are NULL. */
			const float *tb_colour = NULL;
			const float *tb_fg     = NULL;
			bool is_sticky = (view->flags & COW_VIEW_STICKY_FLAG) !=
			    0;

			/* Override titlebar to orange when this is a drop
			 * target */
			if (is_drop_target && cow_wm.drag_drop_edge == 0) {
				tb_colour = (float *)COW_COLOUR_DROP_ORANGE;
				tb_fg	  = (float *)COW_COLOUR_WHITE;
			}

			/* Build tab info if this view is in a container */
			const char	    **tab_titles = NULL;
			int		      n_tabs	 = 0;
			int		      active_tab = 0;
			struct cow_container_pane *pane = cow_view_pane(view);
			if (pane != NULL && pane->n_tabs > 1) {
				n_tabs	   = pane->n_tabs;
				tab_titles = xcalloc(n_tabs, sizeof(char *));
				int			     idx = 0;
				struct cow_container_member *mem;
				wl_list_for_each(mem, &pane->tabs, pane_link)
				{
					tab_titles[idx] =
					    make_display_title(mem->view);
					if (mem == pane->active)
						active_tab = idx;
					idx++;
				}
			}

			char *display_title = make_display_title(view);
			/*
			 * For unsqueezed views this is the full-width strip;
			 * for squeezed ones it is the cow_titlebar_squeezed
			 * rect.  The renderer clears any strip pixels
			 * outside the rect so they fall through to whatever
			 * is below.
			 */
			struct cow_box tb_rect = cow_titlebar_squeezed_rect_for(
			    view, cw, ch);
			struct cow_titlebar_config tb;
			cow_titlebar_effective_config(view, &tb);
			struct cow_titlebar_render_ctx tbctx = {
				.pixels		= dec->buffer->data,
				.stride_px	= stride_px,
				.frame_w	= frame_w,
				.frame_h	= buffer_h,
				.border_width	= bw,
				.border_style	= view->cfg.border_style,
				.tb_rect	= tb_rect,
				.title		= display_title,
				.active		= active,
				.font		= font,
				.pressed_button = view->pressed_button,
				.icon_inverted_button =
				    view->icon_inverted_button,
				.title_pressed = view->title_pressed,
				.tb_cfg	       = &tb,
				.container_cfg = &view->cfg.container,
				.style_colour  = tb_colour,
				.style_fg      = tb_fg,
				.sticky	       = is_sticky,
				.tab_titles    = tab_titles,
				.n_tabs	       = n_tabs,
				.active_tab    = active_tab,
				.drop_target   = is_drop_target &&
				    cow_wm.drag_drop_edge == 0,
			};
			cow_titlebar_render(&tbctx);
			xfree(display_title);
			for (int i = 0; i < n_tabs; i++)
				xfree((char *)tab_titles[i]);
			xfree(tab_titles);

			if (shaded && ch == 0 && bw > 0 &&
			    !cow_titlebar_is_vertical(
				view->cfg.titlebar.position))
				restore_shaded_side_borders(dec->buffer->data,
				    stride_px, frame_w, frame_h, bc, bw,
				    view->cfg.border_style);
		}

		/*
		 * CTWM-style squeezed titlebar re-shaping.  Runs AFTER the
		 * titlebar render so it overwrites the titlebar's own
		 * strip-clear.
		 *
		 * Geometry:
		 *   - North border above the titlebar stays where it is
		 *     (bc->nw covers the frame's top-left corner; drawn
		 *     by cow_border_style_render).
		 *   - A relocated NE corner is painted at the titlebar's
		 *     outer edge, spanning the full top_band (bw + tt)
		 *     height.  Colour: bc->ne if set, else the plain N
		 *     border colour.
		 *   - Past the corner, the north border continues one
		 *     titlebar-height lower (y ∈ [tt, tt+bw)) and meets
		 *     the east border without another corner handle.
		 *
		 * The continued segment's relief is borrowed from a
		 * scratch render with the same edge colours, but no handles.
		 *
		 *   LEFT justify  -- relocated NE at tbr.x + tbr.width.
		 *   RIGHT justify -- mirror (NW marker): relocated at
		 *                    tbr.x - cl.
		 *   CENTRE        -- step down on both sides.
		 */
		if (tt > 0 && bw > 0 && !shaded &&
		    (view->cfg.titlebar.position == COW_TITLEBAR_TOP ||
			view->cfg.titlebar.position == COW_TITLEBAR_BOTTOM ||
			view->cfg.titlebar.position == COW_TITLEBAR_LEFT ||
			view->cfg.titlebar.position == COW_TITLEBAR_RIGHT)) {
			/*
			 * Shaded windows collapse their content height to
			 * zero, leaving frame_h barely larger than the
			 * top_band (= bw + tt).  The stepped-N-border and
			 * frame-corner-relocation writes would spill past
			 * the bottom border, corrupting the decoration.
			 * Skip the squeeze post-pass while shaded.
			 */
			struct cow_titlebar_squeeze sq;
			cow_titlebar_effective_squeeze(view, &sq);
			if (sq.mode != COW_SQUEEZE_NONE) {
				bool bottom =
				    view->cfg.titlebar.position ==
				    COW_TITLEBAR_BOTTOM;
				bool vertical = cow_titlebar_is_vertical(
				    view->cfg.titlebar.position);
				bool east = view->cfg.titlebar.position ==
				    COW_TITLEBAR_RIGHT;
				struct cow_box tbr =
				    cow_titlebar_squeezed_rect_for(view, cw,
					ch);
				int	  marker_w = cl > 0 ? cl : bw;
				int	  top_band = bw + tt;

				/* The stepped body border has no corner handles;
				 * those stay at the ends of the titlebar.
				 */
				size_t nbytes = (size_t)stride_px *
				    (size_t)frame_h * 4;
				uint32_t *scratch = xmalloc(nbytes);
				cow_border_style_render(scratch, stride_px,
				    frame_w, frame_h, bc, bw, 0,
				    view->cfg.border_style);

				/* Corner colours (fall back to plain N/S if
				 * not explicitly configured). */
				/* bc was resolved above from the view's
				 * snapshot. */
				const float *edge_colour = bottom ? bc->s : bc->n;
				const float *ne_colour = bc->has_ne ? bc->ne :
								      edge_colour;
				const float *nw_colour = bc->has_nw ? bc->nw :
								      edge_colour;
				const float *se_colour = bc->has_se ? bc->se :
								      edge_colour;
				const float *sw_colour = bc->has_sw ? bc->sw :
								      edge_colour;

				if (vertical) {
					squeeze_paint_vertical_side(
					    dec->buffer->data, scratch,
					    stride_px, frame_w, frame_h, bw,
					    tt, tbr, sq.justify, east, bc,
					    view->cfg.border_style);
				} else {
					/*
					 * LEFT  -- paint right side only (NE at
					 *          tbr's right edge).
					 * RIGHT -- paint left side only (NW at
					 *          tbr's left edge).
					 * CENTRE -- paint both (NW at left edge,
					 *          NE at right edge).
					 */
					if (sq.justify == COW_JUST_LEFT ||
					    sq.justify == COW_JUST_CENTRE) {
						struct squeeze_paint_ctx sctx = {
							.buf = dec->buffer->data,
							.stride_px = stride_px,
							.frame_w = frame_w,
							.frame_h = frame_h,
							.bw = bw,
							.cl = cl,
							.tt = tt,
							.top_band = top_band,
							.marker_w = marker_w,
							.tbr_near_x =
							    tbr.x + tbr.width,
							.left_side = false,
							.corner_colour =
							    bottom ? se_colour :
								     ne_colour,
							.bc = bc,
							.scratch = scratch,
							.border_style =
							    view->cfg
								.border_style,
						};
						if (bottom)
							squeeze_paint_south_side(
							    &sctx);
						else
							squeeze_paint_side(&sctx);
					}
					if (sq.justify == COW_JUST_RIGHT ||
					    sq.justify == COW_JUST_CENTRE) {
						struct squeeze_paint_ctx sctx = {
							.buf = dec->buffer->data,
							.stride_px = stride_px,
							.frame_w = frame_w,
							.frame_h = frame_h,
							.bw = bw,
							.cl = cl,
							.tt = tt,
							.top_band = top_band,
							.marker_w = marker_w,
							.tbr_near_x = tbr.x,
							.left_side = true,
							.corner_colour =
							    bottom ? sw_colour :
								     nw_colour,
							.bc = bc,
							.scratch = scratch,
							.border_style =
							    view->cfg
								.border_style,
						};
						if (bottom)
							squeeze_paint_south_side(
							    &sctx);
						else
							squeeze_paint_side(&sctx);
					}
				}

				/* Finish both title-to-body elbows after all corner
				 * blits, using clean strips rather than handle pixels.
				 * The same miter works for every titlebar orientation.
				 */
				for (int end = 0; end < 2; end++) {
					int start = vertical ? tbr.y : tbr.x;
					int length = vertical ? tbr.height : tbr.width;
					int extent = vertical ? frame_h : frame_w;
					if ((!end && start <= bw) ||
					    (end && start + length >= extent - bw))
						continue;
					int along = end ? start + length : start - bw;
					int across = (vertical ? east : bottom) ?
					    (vertical ? frame_w : frame_h) - tt - bw : tt;
					cow_border_join_return(dec->buffer->data, scratch,
					    stride_px, frame_w, frame_h,
					    vertical ? across : along,
					    vertical ? along : across, bw,
					    vertical ? east : end != 0,
					    vertical ? end != 0 : bottom);
				}
				xfree(scratch);
			}
		}

		if (bw > 0 && view->resize_indicator_edges != 0 &&
		    view->cfg.has_border_resize)
			render_resize_indicator(dec->buffer->data, stride_px,
			    frame_w, frame_h, bw, cl,
			    view->resize_indicator_edges,
			    view->cfg.border_resize);

		/* The neighbouring pane owns these dividers, including input.
		 */
		uint32_t *pixels = dec->buffer->data;
		if (shared_edges & COW_EDGE_TOP)
			memset(pixels, 0, (size_t)bw * dec->buffer->stride);
		if (shared_edges & COW_EDGE_LEFT) {
			for (int y = 0; y < frame_h; y++)
				memset(pixels + y * stride_px, 0,
				    (size_t)bw * sizeof(*pixels));
		}
	}

	dec->frame_width	 = frame_w;
	dec->frame_height	 = frame_h;
	dec->dirty		 = false;
	dec->last_active	 = active;
	dec->last_content_width	 = cw;
	dec->last_content_height = ch;

commit:
	/* Ensure we have a decoration surface */
	if (view->decoration_surface == NULL) {
		view->decoration_surface = wl_compositor_create_surface(
		    cow_wm.compositor);
		if (view->decoration_surface == NULL)
			return;
	}

	if (view->decoration == NULL) {
		view->decoration = river_window_v1_get_decoration_below(
		    view->river_window, view->decoration_surface);
		if (view->decoration == NULL)
			return;
	}

	/* Set offset: position the frame so the client content area
	 * aligns with the window's top-left corner. */
	river_decoration_v1_set_offset(view->decoration, -content_x,
	    -content_y);

	/*
	 * Input region: set to the union of the borders and the
	 * (possibly squeezed) titlebar rectangle, so pointer events
	 * landing in the cleared pixels of the titlebar strip pass
	 * through the decoration surface to whatever is beneath.
	 *
	 * Regions live entirely in the decoration buffer coordinate
	 * system where (0, 0) is the top-left of the outer frame.
	 */
	struct wl_region *region = wl_compositor_create_region(
	    cow_wm.compositor);
	if (region != NULL) {
		struct cow_box body, title;
		cow_decoration_frame_rects(view, cw, ch, &body, &title);
		wl_region_add(region, body.x, body.y, body.width, body.height);
		if (title.width > 0 && title.height > 0)
			wl_region_add(region, title.x, title.y, title.width,
			    title.height);
		wl_region_subtract(region, content_x, content_y, cw, ch);

		if (shared_edges & COW_EDGE_TOP)
			wl_region_subtract(region, 0, 0, frame_w, bw);
		if (shared_edges & COW_EDGE_LEFT)
			wl_region_subtract(region, 0, 0, bw, frame_h);
		wl_surface_set_input_region(view->decoration_surface, region);
		wl_region_destroy(region);
	}

	/* Attach buffer and commit */
	if (dec->buffer != NULL) {
		if (compact)
			cow_decoration_tiles_commit(view, content_x, content_y);
		wl_surface_attach(view->decoration_surface, dec->buffer->buffer,
		    0, 0);
		wl_surface_damage_buffer(view->decoration_surface, 0, 0,
		    frame_w, buffer_h);
		river_decoration_v1_sync_next_commit(view->decoration);
		wl_surface_commit(view->decoration_surface);
		dec->buffer->busy = true;
		dec->mapped = true;
	}
}

void
cow_decoration_hide(struct cow_view *view)
{
	if (view == NULL || view->decoration_surface == NULL ||
	    !view->dec.mapped)
		return;

	wl_surface_attach(view->decoration_surface, NULL, 0, 0);
	cow_decoration_tiles_hide(view);
	if (view->decoration != NULL)
		river_decoration_v1_sync_next_commit(view->decoration);
	wl_surface_commit(view->decoration_surface);
	view->dec.mapped = false;
}
