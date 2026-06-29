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
#include "cow/draw.h"

/* Sentinel: transparent / skip drawing.  Aliased onto the project-wide
 * cow_pixbuf primitive so existing call sites need not change. */
#define TRANSPARENT_PX COW_DRAW_TRANSPARENT_PX

/* Wrap (pixels, stride, img_w, img_h) into a cow_pixbuf for the
 * project-wide drawing primitives.  Inlined; the resulting struct is
 * tiny and the helper below only exists to shorten the per-call
 * boilerplate at the existing border.c call sites. */
static inline struct cow_pixbuf
border_pixbuf(uint32_t *pixels, int stride, int img_w, int img_h)
{
	return (struct cow_pixbuf) {
		.pixels	   = pixels,
		.stride_px = stride,
		.width	   = img_w,
		.height	   = img_h,
	};
}

static inline void
hline(uint32_t *pixels, int stride, int img_w, int img_h, int x1, int x2, int y,
    uint32_t colour)
{
	struct cow_pixbuf p = border_pixbuf(pixels, stride, img_w, img_h);
	cow_draw_hline(&p, x1, x2, y, colour);
}

static inline void
vline(uint32_t *pixels, int stride, int img_w, int img_h, int x, int y1, int y2,
    uint32_t colour)
{
	struct cow_pixbuf p = border_pixbuf(pixels, stride, img_w, img_h);
	cow_draw_vline(&p, x, y1, y2, colour);
}

static void
relieve_rect_per_edge(uint32_t *pixels, int stride, int img_w, int img_h, int x,
    int y, int w, int h, int line_width, uint32_t ul_left, uint32_t ul_top,
    uint32_t br_bot, uint32_t br_right)
{
	struct cow_pixbuf p = border_pixbuf(pixels, stride, img_w, img_h);
	cow_draw_relieve_rect(&p, x, y, w, h, line_width, ul_left, ul_top,
	    br_bot, br_right);
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

/*
 * Pack the per-side base colour into [dk, lt, base] for the bevel
 * routines.  The triple itself comes from cow_colour_make_triple; we
 * only fan it out into the array layout the rest of border.c uses.
 */
static inline void
edge_colours_pack_side(uint32_t out[3], const float src[4])
{
	struct cow_colour_triple t;
	cow_colour_make_triple(&t, src);
	out[0] = t.dk;
	out[1] = t.lt;
	out[2] = t.base;
}

static void
edge_colours_init(struct edge_colours *ec, const struct cow_border_colour *bc)
{
	edge_colours_pack_side(ec->n, bc->n);
	edge_colours_pack_side(ec->s, bc->s);
	edge_colours_pack_side(ec->w, bc->w);
	edge_colours_pack_side(ec->e, bc->e);
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
    const struct cow_border_colour *colors)
{
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

			bool	 in_top = (dt < cls), in_bot = (db < cls);
			bool	 in_left = (dl < cls), in_right = (dr < cls);
			uint32_t px;

			if (in_top && in_left) {
				px = colors->has_nw ?
				    cow_colour_to_pixel(colors->nw) :
				    ((dt <= dl) ? ec->n[2] : ec->w[2]);
			} else if (in_top && in_right) {
				px = colors->has_ne ?
				    cow_colour_to_pixel(colors->ne) :
				    ((dt <= dr) ? ec->n[2] : ec->e[2]);
			} else if (in_bot && in_left) {
				px = colors->has_sw ?
				    cow_colour_to_pixel(colors->sw) :
				    ((db <= dl) ? ec->s[2] : ec->w[2]);
			} else if (in_bot && in_right) {
				px = colors->has_se ?
				    cow_colour_to_pixel(colors->se) :
				    ((db <= dr) ? ec->s[2] : ec->e[2]);
			} else if (dt == dmin) {
				px = ec->n[2];
			} else if (db == dmin) {
				px = ec->s[2];
			} else if (dl == dmin) {
				px = ec->w[2];
			} else {
				px = ec->e[2];
			}
			pixels[y * stride + x] = px;
		}
	}
}

static void
fill_gradient_rect(struct cow_pixbuf *p, const struct cow_gradient *g, int x,
    int y, int w, int h)
{
	if (g != NULL && g->mode != COW_GRADIENT_NONE)
		cow_draw_gradient_rect(p, x, y, w, h, g);
}

static void
fill_border_gradients(uint32_t *pixels, int stride, int fw_s, int fh_s,
    int bws, int cls, const struct cow_border_colour *colors)
{
	struct cow_pixbuf p = border_pixbuf(pixels, stride, fw_s, fh_s);

	fill_gradient_rect(&p, &colors->gradient, 0, 0, fw_s, bws);
	fill_gradient_rect(&p, &colors->gradient, 0, fh_s - bws, fw_s, bws);
	fill_gradient_rect(&p, &colors->gradient, 0, 0, bws, fh_s);
	fill_gradient_rect(&p, &colors->gradient, fw_s - bws, 0, bws, fh_s);

	fill_gradient_rect(&p, &colors->gradient_n, 0, 0, fw_s, bws);
	fill_gradient_rect(&p, &colors->gradient_s, 0, fh_s - bws, fw_s,
	    bws);
	fill_gradient_rect(&p, &colors->gradient_w, 0, 0, bws, fh_s);
	fill_gradient_rect(&p, &colors->gradient_e, fw_s - bws, 0, bws,
	    fh_s);

	if (cls <= 0)
		cls = bws;
	fill_gradient_rect(&p, &colors->gradient_nw, 0, 0, cls, cls);
	fill_gradient_rect(&p, &colors->gradient_ne, fw_s - cls, 0, cls,
	    cls);
	fill_gradient_rect(&p, &colors->gradient_sw, 0, fh_s - cls, cls,
	    cls);
	fill_gradient_rect(&p, &colors->gradient_se, fw_s - cls, fh_s - cls,
	    cls, cls);
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
    int cls, const int w[7], const struct cow_border_colour *colors)
{
	struct {
		bool	     has;
		const float *colour;
		int	     cx, cy, cw, ch;
		bool	     ul_vert, ul_horiz, br_horiz, br_vert;
	} corners[4] = {
		{ colors->has_nw, colors->nw, 0,		 0,	    cls, cls, true,  true,  false,
		    false										     },
		{ colors->has_ne, colors->ne, fw_s - cls, 0,	     cls, cls, false,
		    true,									false, true  },
		{ colors->has_sw, colors->sw, 0,		 fh_s - cls, cls, cls, true,
		    false,								       true,	 false },
		{ colors->has_se, colors->se, fw_s - cls, fh_s - cls, cls, cls,
		    false,								false, true,	true  },
	};

	for (int ci = 0; ci < 4; ci++) {
		if (!corners[ci].has)
			continue;

		uint32_t cc[3];
		edge_colours_pack_side(cc, corners[ci].colour);

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
			struct cow_colour_triple t;
			cow_colour_make_triple(&t, cm[ci].colour);
			xm_colour = cm[ci].xm_shadow ? t.dk : t.lt;
			ym_colour = cm[ci].ym_shadow ? t.dk : t.lt;
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
    int corner_length, int style)
{
	if (frame_w < 2 || frame_h < 2 || border_width < 1)
		return;

	int  bws	     = border_width;
	int  cls	     = corner_length;
	bool corners_enabled = (cls > 0);
	if (cls < bws)
		cls = bws;

	/* Clear to transparent */
	memset(pixels, 0, (size_t)stride_px * frame_h * 4);

	struct edge_colours ec;
	edge_colours_init(&ec, colors);

	/* Fill base colors for the border ring.
	 * When corners are disabled, use cls=0 to skip diagonal splits. */
	fill_border_base(pixels, stride_px, frame_w, frame_h, bws,
	    corners_enabled ? cls : 0, &ec, colors);
	fill_border_gradients(pixels, stride_px, frame_w, frame_h, bws,
	    corners_enabled ? cls : 0, colors);

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
		    cls, w, colors);

		/* Draw handle marks on all 8 frame parts */
		draw_handle_marks(pixels, stride_px, frame_w, frame_h, bws, cls,
		    w, style, &ec, colors);
	}
}
