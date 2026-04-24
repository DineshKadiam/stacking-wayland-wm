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
#include "cow/border.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "cow/colour.h"
#include "cow/config.h"

/* Sentinel: transparent / skip drawing. */
#define TRANSPARENT_PX 0xFFFFFFFF

static inline void
hline(uint32_t *pixels, int stride, int img_w, int img_h, int x1, int x2, int y,
    uint32_t colour)
{
	if (y < 0 || y >= img_h)
		return;
	if (x1 < 0)
		x1 = 0;
	if (x2 >= img_w)
		x2 = img_w - 1;
	for (int x = x1; x <= x2; x++)
		pixels[y * stride + x] = colour;
}

static inline void
vline(uint32_t *pixels, int stride, int img_w, int img_h, int x, int y1, int y2,
    uint32_t colour)
{
	if (x < 0 || x >= img_w)
		return;
	if (y1 < 0)
		y1 = 0;
	if (y2 >= img_h)
		y2 = img_h - 1;
	for (int y = y1; y <= y2; y++)
		pixels[y * stride + x] = colour;
}

static void
relieve_rect_per_edge(uint32_t *pixels, int stride, int img_w, int img_h, int x,
    int y, int w, int h, int line_width, uint32_t ul_left, uint32_t ul_top,
    uint32_t br_bot, uint32_t br_right)
{
	int max_w, max_h;

	if (w <= 0 || h <= 0 || line_width <= 0)
		return;

	max_w = (w + 1) / 2;
	if (max_w > line_width)
		max_w = line_width;
	max_h = (h + 1) / 2;
	if (max_h > line_width)
		max_h = line_width;

	for (int i = 0; i < max_w; i++) {
		if (ul_left != TRANSPARENT_PX)
			vline(pixels, stride, img_w, img_h, x + i, y + i,
			    y + h - 1 - i, ul_left);
	}
	for (int i = 0; i < max_h; i++) {
		if (ul_top != TRANSPARENT_PX)
			hline(pixels, stride, img_w, img_h, x + i + 1,
			    x + w - i, y + i, ul_top);
	}

	for (int i = 0; i < max_h; i++) {
		if (br_bot != TRANSPARENT_PX)
			hline(pixels, stride, img_w, img_h, x + i,
			    x + w - 1 - i, y + h - i, br_bot);
	}
	for (int i = 0; i < max_w; i++) {
		if (br_right != TRANSPARENT_PX)
			vline(pixels, stride, img_w, img_h, x + w - i,
			    y + i + 1, y + h - i, br_right);
	}
}

static void
draw_x_mark(uint32_t *pixels, int stride, int img_w, int img_h, int x, int y,
    int mark_length, int mark_thickness, int offset_tl, bool do_draw_shadow,
    uint32_t colour)
{
	x += offset_tl;
	for (int k = 0; k < mark_thickness; k++) {
		int len = mark_length - 1 - k;
		if (len < 0)
			break;
		int x1, y1;
		if (do_draw_shadow) {
			x1 = x;
			y1 = y - 1 - k;
		} else {
			x1 = x - k;
			y1 = y + k;
		}
		hline(pixels, stride, img_w, img_h, x1, x1 + len, y1, colour);
	}
}

static void
draw_y_mark(uint32_t *pixels, int stride, int img_w, int img_h, int x, int y,
    int mark_length, int mark_thickness, int offset_tl, bool do_draw_shadow,
    uint32_t colour)
{
	y += offset_tl;
	for (int k = 0; k < mark_thickness; k++) {
		int len = mark_length - k;
		if (len <= 0)
			break;
		int x1, y1;
		if (do_draw_shadow) {
			x1 = x - 1 - k;
			y1 = y + k;
		} else {
			x1 = x + k;
			y1 = y;
		}
		vline(pixels, stride, img_w, img_h, x1, y1, y1 + len - 1,
		    colour);
	}
}

struct edge_colours {
	uint32_t n[3], s[3], w[3], e[3];
};

static void
edge_colours_init(struct edge_colours *ec, const struct cow_border_colour *bc)
{
	float lt[4], dk[4];

	cow_colour_lighten(lt, bc->n);
	cow_colour_darken(dk, bc->n);
	ec->n[0] = cow_colour_to_pixel(dk);
	ec->n[1] = cow_colour_to_pixel(lt);
	ec->n[2] = cow_colour_to_pixel(bc->n);

	cow_colour_lighten(lt, bc->s);
	cow_colour_darken(dk, bc->s);
	ec->s[0] = cow_colour_to_pixel(dk);
	ec->s[1] = cow_colour_to_pixel(lt);
	ec->s[2] = cow_colour_to_pixel(bc->s);

	cow_colour_lighten(lt, bc->w);
	cow_colour_darken(dk, bc->w);
	ec->w[0] = cow_colour_to_pixel(dk);
	ec->w[1] = cow_colour_to_pixel(lt);
	ec->w[2] = cow_colour_to_pixel(bc->w);

	cow_colour_lighten(lt, bc->e);
	cow_colour_darken(dk, bc->e);
	ec->e[0] = cow_colour_to_pixel(dk);
	ec->e[1] = cow_colour_to_pixel(lt);
	ec->e[2] = cow_colour_to_pixel(bc->e);
}

/* FVWM 7-layer relief tables. */
static const int fvwm_ul[] = { 0, 1, 1, -1, 3, 0, 0 };
static const int fvwm_br[] = { 0, 0, 3, -1, 1, 1, 0 };

static void
compute_layer_widths(int w[7], int bws, int style)
{
	if (style == COW_BORDER_STYLE_MWM) {
		w[0]	= 0;
		w[1]	= 2;
		w[2]	= 0;
		w[3]	= 0;
		w[4]	= 0;
		w[5]	= 1;
		w[6]	= 0;
		int sum = 3;
		if (bws < sum) {
			int trim = sum - bws;
			if (trim > 0 && w[1] > 1) {
				w[1]--;
				trim--;
				sum--;
			}
			if (trim > 0 && w[5] > 0) {
				w[5] = 0;
				trim--;
				sum--;
			}
			if (trim > 0 && w[1] > 0) {
				w[1] = 0;
				trim--;
				sum--;
			}
		}
		w[3] = bws - sum;
		if (w[3] < 0)
			w[3] = 0;
	} else {
		w[0]	= 1;
		w[1]	= 1;
		w[2]	= 1;
		w[3]	= 0;
		w[4]	= 1;
		w[5]	= 1;
		w[6]	= 1;
		int sum = 6;
		if (bws < sum) {
			int trim = sum - bws;
			if (trim > 0 && w[1] > 1) {
				w[1]--;
				trim--;
				sum--;
			}
			if (trim > 0 && w[5] > 0) {
				w[5] = 0;
				trim--;
				sum--;
			}
			if (trim > 0 && w[1] > 0) {
				w[1] = 0;
				trim--;
				sum--;
			}
			if (trim > 0 && w[2] > 0) {
				w[2] = 0;
				trim--;
				sum--;
			}
			if (trim > 0 && w[4] > 0) {
				w[4] = 0;
				trim--;
				sum--;
			}
			if (trim > 0 && w[6] > 0) {
				w[6] = 0;
				trim--;
				sum--;
			}
		}
		w[3] = bws - sum;
		if (w[3] < 0)
			w[3] = 0;
	}
}

static void
fill_border_base(uint32_t *pixels, int stride, int fw_s, int fh_s, int bws,
    int cls, const struct edge_colours *ec,
    const struct cow_border_colour *colors, int top_left_x, int top_right_x)
{
	/*
	 * NW/NE corner cells are computed relative to the top strip's
	 * near ends (top_left_x / top_right_x) rather than the frame's
	 * absolute (0 / fw_s) corners.  Outside the top strip the cells
	 * fall back to the left/right edge colour so the side borders
	 * reach all the way up to y=0 (needed by squeezed titlebars).
	 */
	for (int y = 0; y < fh_s; y++) {
		for (int x = 0; x < fw_s; x++) {
			int dt = y, db = fh_s - 1 - y, dl = x,
			    dr	 = fw_s - 1 - x;
			int dmin = dt;
			if (db < dmin)
				dmin = db;
			if (dl < dmin)
				dmin = dl;
			if (dr < dmin)
				dmin = dr;
			if (dmin >= bws)
				continue;

			bool in_top_strip = (x >= top_left_x &&
			    x < top_right_x);

			/*
			 * Distances to the NW / NE corners of the (possibly
			 * clipped) top strip.  When top_left_x == 0 and
			 * top_right_x == fw_s these coincide with dl / dr.
			 */
			int  dl_tb   = x - top_left_x;
			int  dr_tb   = (top_right_x - 1) - x;
			bool near_nw = in_top_strip && dt < cls &&
			    dl_tb < cls && dl_tb >= 0;
			bool near_ne = in_top_strip && dt < cls &&
			    dr_tb < cls && dr_tb >= 0;

			bool	 in_bot	  = (db < cls);
			bool	 in_left  = (dl < cls);
			bool	 in_right = (dr < cls);
			uint32_t px;

			if (near_nw) {
				px = colors->has_nw ?
				    cow_colour_to_pixel(colors->nw) :
				    ((dt <= dl_tb) ? ec->n[2] : ec->w[2]);
			} else if (near_ne) {
				px = colors->has_ne ?
				    cow_colour_to_pixel(colors->ne) :
				    ((dt <= dr_tb) ? ec->n[2] : ec->e[2]);
			} else if (in_bot && in_left) {
				px = colors->has_sw ?
				    cow_colour_to_pixel(colors->sw) :
				    ((db <= dl) ? ec->s[2] : ec->w[2]);
			} else if (in_bot && in_right) {
				px = colors->has_se ?
				    cow_colour_to_pixel(colors->se) :
				    ((db <= dr) ? ec->s[2] : ec->e[2]);
			} else if (dt == dmin && in_top_strip) {
				px = ec->n[2];
			} else if (db == dmin) {
				px = ec->s[2];
			} else if (dl == dmin) {
				px = ec->w[2];
			} else if (dr == dmin) {
				px = ec->e[2];
			} else {
				/*
				 * dt == dmin but outside the top strip --
				 * the titlebar will paint this area, or it
				 * should fall through.  Leave transparent.
				 */
				continue;
			}
			pixels[y * stride + x] = px;
		}
	}
}

static void
draw_relief_rings(uint32_t *pixels, int stride, int fw_s, int fh_s,
    const int w[7], const struct edge_colours *ec)
{
	int off = 0, rw = fw_s - 1, rh = fh_s - 1;

	for (int i = 0; i < 7; i++) {
		if (w[i] <= 0)
			continue;
		if (fvwm_ul[i] < 0) {
			off += w[i];
			rw -= 2 * w[i];
			rh -= 2 * w[i];
			continue;
		}
		if (rw <= 0 || rh <= 0)
			break;

		uint32_t ul_w = (fvwm_ul[i] == 3) ? TRANSPARENT_PX :
						    ec->w[fvwm_ul[i]];
		uint32_t ul_n = (fvwm_ul[i] == 3) ? TRANSPARENT_PX :
						    ec->n[fvwm_ul[i]];
		uint32_t br_s = (fvwm_br[i] == 3) ? TRANSPARENT_PX :
						    ec->s[fvwm_br[i]];
		uint32_t br_e = (fvwm_br[i] == 3) ? TRANSPARENT_PX :
						    ec->e[fvwm_br[i]];

		for (int k = 0; k < w[i]; k++) {
			relieve_rect_per_edge(pixels, stride, fw_s, fh_s, off,
			    off, rw, rh, 1, ul_w, ul_n, br_s, br_e);
			off++;
			rw -= 2;
			rh -= 2;
		}
	}
}

/* ---- overdraw_explicit_corners ---- */

static void
overdraw_explicit_corners(uint32_t *pixels, int stride, int fw_s, int fh_s,
    int bws, int cls, const int w[7], const struct cow_border_colour *colors,
    int top_left_x, int top_right_x)
{
	/*
	 * NW and NE corners relocate to the clipped top strip's ends
	 * when the caller requested squeeze; SW and SE stay at the
	 * frame's bottom corners.
	 *
	 * The relocated NW/NE markers sit inside the top border only,
	 * not the full cls-thick decoration area, because the titlebar
	 * itself occupies the vertical range below bws on the other
	 * side.  We scale nh down to the top-border thickness so the
	 * relocated marker looks like a small corner handle instead of
	 * a tall block overhanging the titlebar.
	 */
	int nw_x = top_left_x;
	int ne_x = top_right_x - cls;
	if (ne_x < nw_x + cls)
		ne_x = nw_x + cls;

	bool squeezed	  = (top_left_x > 0 || top_right_x < fw_s);
	int  top_marker_h = squeezed ? bws : cls;

	struct {
		bool	     has;
		const float *colour;
		int	     cx, cy, cw, ch;
		bool	     ul_vert, ul_horiz, br_horiz, br_vert;
	} corners[4] = {
		{ colors->has_nw, colors->nw, nw_x,	    0,	       cls, top_marker_h, true,
		    true,										 false, false },
		{ colors->has_ne, colors->ne, ne_x,	    0,	       cls, top_marker_h, false,
		    true,										 false, true  },
		{ colors->has_sw, colors->sw, 0,		 fh_s - cls, cls, cls,	       true,
		    false,										true,	 false },
		{ colors->has_se, colors->se, fw_s - cls, fh_s - cls, cls, cls,
		    false,									 false, true,	true  },
	};

	for (int ci = 0; ci < 4; ci++) {
		if (!corners[ci].has)
			continue;

		float clt[4], cdk[4];
		cow_colour_lighten(clt, corners[ci].colour);
		cow_colour_darken(cdk, corners[ci].colour);
		uint32_t cc[3] = { cow_colour_to_pixel(cdk),
			cow_colour_to_pixel(clt),
			cow_colour_to_pixel(corners[ci].colour) };

		int ccx = corners[ci].cx, ccy = corners[ci].cy;
		int ccw = corners[ci].cw, cch = corners[ci].ch;

		int off = 0, rw = fw_s - 1, rh = fh_s - 1;

		for (int i = 0; i < 7; i++) {
			if (w[i] <= 0)
				continue;
			if (fvwm_ul[i] < 0) {
				off += w[i];
				rw -= 2 * w[i];
				rh -= 2 * w[i];
				continue;
			}
			if (rw <= 0 || rh <= 0)
				break;

			uint32_t ul = (fvwm_ul[i] == 3) ? TRANSPARENT_PX :
							  cc[fvwm_ul[i]];
			uint32_t br = (fvwm_br[i] == 3) ? TRANSPARENT_PX :
							  cc[fvwm_br[i]];

			for (int k = 0; k < w[i]; k++) {
				int rx = off + k, ry = off + k;
				int rrw = rw - 2 * k, rrh = rh - 2 * k;

				if (corners[ci].ul_vert &&
				    ul != TRANSPARENT_PX && rx >= ccx &&
				    rx < ccx + ccw) {
					int y1 = ry, y2 = ry + rrh - 1;
					if (y1 < ccy)
						y1 = ccy;
					if (y2 >= ccy + cch)
						y2 = ccy + cch - 1;
					if (y1 <= y2)
						vline(pixels, stride, fw_s,
						    fh_s, rx, y1, y2, ul);
				}
				if (corners[ci].ul_horiz &&
				    ul != TRANSPARENT_PX && ry >= ccy &&
				    ry < ccy + cch) {
					int x1 = rx + 1, x2 = rx + rrw;
					if (x1 < ccx)
						x1 = ccx;
					if (x2 >= ccx + ccw)
						x2 = ccx + ccw - 1;
					if (x1 <= x2)
						hline(pixels, stride, fw_s,
						    fh_s, x1, x2, ry, ul);
				}
				if (corners[ci].br_horiz &&
				    br != TRANSPARENT_PX) {
					int by = ry + rrh;
					if (by >= ccy && by < ccy + cch) {
						int x1 = rx, x2 = rx + rrw - 1;
						if (x1 < ccx)
							x1 = ccx;
						if (x2 >= ccx + ccw)
							x2 = ccx + ccw - 1;
						if (x1 <= x2)
							hline(pixels, stride,
							    fw_s, fh_s, x1, x2,
							    by, br);
					}
				}
				if (corners[ci].br_vert &&
				    br != TRANSPARENT_PX) {
					int bx = rx + rrw;
					if (bx >= ccx && bx < ccx + ccw) {
						int y1 = ry + 1, y2 = ry + rrh;
						if (y1 < ccy)
							y1 = ccy;
						if (y2 >= ccy + cch)
							y2 = ccy + cch - 1;
						if (y1 <= y2)
							vline(pixels, stride,
							    fw_s, fh_s, bx, y1,
							    y2, br);
					}
				}
			}
			off += w[i];
			rw -= 2 * w[i];
			rh -= 2 * w[i];
		}
	}
}

static void
draw_handle_marks(uint32_t *pixels, int stride, int fw_s, int fh_s, int bws,
    int cls, const int w[7], int style, const struct edge_colours *ec,
    const struct cow_border_colour *colors)
{
	int w_dout     = w[0];
	int inset      = (w[5] > 0 || w[6] > 0) ? 1 : 0;
	int mark_len   = bws - w_dout - inset;
	int mark_thick = (style == COW_BORDER_STYLE_MWM) ? 1 : 2;
	if (mark_thick > mark_len)
		mark_thick = mark_len;
	int offset_tl = w_dout;
	int offset_br = -2 * w_dout - mark_len;

	if (mark_len <= 0 || cls <= bws)
		return;

	int sbw = fw_s - 2 * cls;
	int sbh = fh_s - 2 * cls;

	/* Sidebar marks */
	if (sbw > 0) {
		draw_y_mark(pixels, stride, fw_s, fh_s, cls, 0, mark_len,
		    mark_thick, offset_tl, false, ec->n[1]);
		draw_y_mark(pixels, stride, fw_s, fh_s, cls + sbw, 0, mark_len,
		    mark_thick, offset_tl, true, ec->n[0]);
		draw_y_mark(pixels, stride, fw_s, fh_s, cls,
		    fh_s - bws + bws + offset_br, mark_len, mark_thick,
		    offset_tl, false, ec->s[1]);
		draw_y_mark(pixels, stride, fw_s, fh_s, cls + sbw,
		    fh_s - bws + bws + offset_br, mark_len, mark_thick,
		    offset_tl, true, ec->s[0]);
	}
	if (sbh > 0) {
		draw_x_mark(pixels, stride, fw_s, fh_s, 0, cls, mark_len,
		    mark_thick, offset_tl, false, ec->w[1]);
		draw_x_mark(pixels, stride, fw_s, fh_s, 0, cls + sbh, mark_len,
		    mark_thick, offset_tl, true, ec->w[0]);
		draw_x_mark(pixels, stride, fw_s, fh_s,
		    fw_s - bws + bws + offset_br, cls, mark_len, mark_thick,
		    offset_tl, false, ec->e[1]);
		draw_x_mark(pixels, stride, fw_s, fh_s,
		    fw_s - bws + bws + offset_br, cls + sbh, mark_len,
		    mark_thick, offset_tl, true, ec->e[0]);
	}

	/* Corner marks */
	struct {
		bool	     has_explicit;
		const float *colour;
		int	     cx, cy;
		int	     xm_x, xm_y;
		bool	     xm_shadow;
		int	     ym_x, ym_y;
		bool	     ym_shadow;
		uint32_t     def_xm, def_ym;
	} cm[4] = {
		{ colors->has_nw, colors->nw, 0,		 0,	    0,	       cls, true,  cls, 0,		  true,
		    ec->w[0],														ec->n[0] },
		{ colors->has_ne, colors->ne, fw_s - cls, 0,	     cls + offset_br,
		    cls,								       true,  0,	  0,		     false, ec->e[0], ec->n[1] },
		{ colors->has_sw, colors->sw, 0,		 fh_s - cls, 0,		0,   false, cls,
		    cls + offset_br,											true,  ec->w[1], ec->s[0] },
		{ colors->has_se, colors->se, fw_s - cls, fh_s - cls,
		    cls + offset_br,						      0,	false, 0,	  cls + offset_br, false,
		    ec->e[1],														ec->s[1] },
	};

	for (int ci = 0; ci < 4; ci++) {
		uint32_t xm_colour, ym_colour;
		if (cm[ci].has_explicit) {
			float clt[4], cdk[4];
			cow_colour_lighten(clt, cm[ci].colour);
			cow_colour_darken(cdk, cm[ci].colour);
			xm_colour = cm[ci].xm_shadow ?
			    cow_colour_to_pixel(cdk) :
			    cow_colour_to_pixel(clt);
			ym_colour = cm[ci].ym_shadow ?
			    cow_colour_to_pixel(cdk) :
			    cow_colour_to_pixel(clt);
		} else {
			xm_colour = cm[ci].def_xm;
			ym_colour = cm[ci].def_ym;
		}
		draw_x_mark(pixels, stride, fw_s, fh_s, cm[ci].cx + cm[ci].xm_x,
		    cm[ci].cy + cm[ci].xm_y, mark_len, mark_thick, offset_tl,
		    cm[ci].xm_shadow, xm_colour);
		draw_y_mark(pixels, stride, fw_s, fh_s, cm[ci].cx + cm[ci].ym_x,
		    cm[ci].cy + cm[ci].ym_y, mark_len, mark_thick, offset_tl,
		    cm[ci].ym_shadow, ym_colour);
	}
}

void
cow_border_style_render(uint32_t *pixels, int stride_px, int frame_w,
    int frame_h, const struct cow_border_colour *colors, int border_width,
    int corner_length, int style, int top_left_x, int top_right_x)
{
	if (frame_w < 2 || frame_h < 2 || border_width < 1)
		return;

	int  bws	     = border_width;
	int  cls	     = corner_length;
	bool corners_enabled = (cls > 0);
	if (cls < bws)
		cls = bws;

	/* Clamp the top-strip range. */
	if (top_left_x < 0)
		top_left_x = 0;
	if (top_right_x > frame_w)
		top_right_x = frame_w;
	if (top_right_x < top_left_x)
		top_right_x = top_left_x;

	/* Clear to transparent */
	memset(pixels, 0, (size_t)stride_px * frame_h * 4);

	struct edge_colours ec;
	edge_colours_init(&ec, colors);

	/* Fill base colors for the border ring.
	 * When corners are disabled, use cls=0 to skip diagonal splits. */
	fill_border_base(pixels, stride_px, frame_w, frame_h, bws,
	    corners_enabled ? cls : 0, &ec, colors, top_left_x, top_right_x);

	/* NONE = flat fill only, no 3D relief */
	if (style == COW_BORDER_STYLE_NONE)
		return;

	/* Compute layer widths for the selected style */
	int w[7];
	compute_layer_widths(w, bws, style);

	/* Draw concentric relief rectangles */
	draw_relief_rings(pixels, stride_px, frame_w, frame_h, w, &ec);

	if (corners_enabled) {
		/* Overdraw corners with explicit colors */
		overdraw_explicit_corners(pixels, stride_px, frame_w, frame_h,
		    bws, cls, w, colors, top_left_x, top_right_x);

		/* Draw handle marks on all 8 frame parts */
		draw_handle_marks(pixels, stride_px, frame_w, frame_h, bws, cls,
		    w, style, &ec, colors);
	}

	/*
	 * Post-process: if the top-border strip has been clipped
	 * (squeezed titlebar), wipe any pixels the relief / corner
	 * overdraw passes wrote outside the strip's x-range in the
	 * top bws rows.  Side borders (x < bws or x >= fw_s - bws)
	 * are preserved so they still reach up to y=0.
	 */
	if (top_left_x > 0 || top_right_x < frame_w) {
		for (int y = 0; y < bws; y++) {
			for (int x = 0; x < frame_w; x++) {
				if (x >= top_left_x && x < top_right_x)
					continue;
				if (x < bws || x >= frame_w - bws)
					continue; /* keep side-border cap */
				pixels[y * stride_px + x] = 0;
			}
		}
	}
}
