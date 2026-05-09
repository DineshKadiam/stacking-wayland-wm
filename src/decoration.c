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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <wayland-client.h>

#include "cow/border.h"
#include "cow/colour.h"
#include "cow/config.h"
#include "cow/font.h"
#include "cow/memory.h"
#include "cow/shm.h"
#include "cow/titlebar.h"
#include "cow/view.h"
#include "cow/wm.h"

#include "river-window-management-v1-client-protocol.h"

#define COW_MIN_ID_PREFIX 8

/* Global font for titlebar rendering.
 * Rebuilt whenever the font name changes (tracked via last_font_name). */
static struct cow_font titlebar_font;
static char	      *last_font_name = NULL;

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
		xfree(last_font_name);
		last_font_name = xstrdup(name);
	}

	return &titlebar_font;
}

/*
 * Build a display title of the form "[<short-id>] <title>".
 * The ID prefix is the shortest prefix of view->identifier (minimum
 * COW_MIN_ID_PREFIX characters) that is not a prefix of any other
 * live view's identifier.  Returns a heap-allocated string; caller
 * must free it.  Falls back to view->title if no identifier is set.
 */
static char *
make_display_title(const struct cow_view *view)
{
	const char *id	  = view->identifier;
	const char *title = view->title ? view->title : "";

	if (id == NULL || id[0] == '\0') {
		return xstrdup(title);
	}

	size_t id_len	  = strlen(id);
	size_t prefix_len = COW_MIN_ID_PREFIX;
	if (prefix_len > id_len)
		prefix_len = id_len;

	/* Extend prefix until no other view shares it */
	while (prefix_len < id_len) {
		bool		 unique = true;
		struct cow_view *other;
		wl_list_for_each(other, &cow_wm.views, wm_link)
		{
			if (other == view || other->closed)
				continue;
			if (other->identifier == NULL)
				continue;
			if (strncmp(id, other->identifier, prefix_len) == 0) {
				unique = false;
				break;
			}
		}
		if (unique)
			break;
		prefix_len++;
	}

	char *out = NULL;
	xasprintf(&out, "[%.*s] %s", (int)prefix_len, id, title);
	return out;
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

/*
 * Shared post-processing for one side of a squeezed titlebar.
 *
 * 'left_side' is true when we're painting the titlebar's LEFT edge
 * (the relocated NW) and false for the titlebar's RIGHT edge (the
 * relocated NE).  LEFT-justify calls this once with left_side=false,
 * RIGHT-justify once with left_side=true, and CENTRE calls both.
 */
static void
squeeze_paint_side(uint32_t *buf, int stride_px, int frame_w, int frame_h,
    int bw, int cl, int tt, int top_band, int marker_w,
    int	 tbr_near_x, /* titlebar's near-edge x for this side */
    bool left_side, const float *corner_colour,
    const struct cow_border_colour *bc, const uint32_t *scratch)
{
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
	int	  mini_h   = mh * 4;
	int	  mini_str = mini_w;
	uint32_t *mini = xcalloc((size_t)mini_str * mini_h, sizeof(uint32_t));
	cow_border_style_render(mini, mini_str, mini_w, mini_h, &cm_bc, bw, cl,
	    cow_config->border_style);

	int src_x0 = !left_side ? (mini_w - mw) : 0;

	/* Top arm: bw rows along the top. */
	for (int row = 0; row < bw && row < mh; row++) {
		for (int col = 0; col < mw; col++) {
			int dx = mx + col;
			if (dx < 0 || dx >= frame_w)
				continue;
			uint32_t src = mini[row * mini_str + (src_x0 + col)];
			if ((src >> 24) == 0)
				continue;
			buf[row * stride_px + dx] = src;
		}
	}
	/* Side arm: bw cols at the apex side. */
	int arm_col = !left_side ? (mw - bw) : 0;
	for (int col = arm_col; col < arm_col + bw; col++) {
		for (int row = bw; row < mh; row++) {
			int dx = mx + col;
			if (dx < 0 || dx >= frame_w)
				continue;
			if (row >= frame_h)
				break;
			uint32_t src = mini[row * mini_str + (src_x0 + col)];
			if ((src >> 24) == 0)
				continue;
			buf[row * stride_px + dx] = src;
		}
	}
	xfree(mini);

	/*
	 * draw_handle_marks() draws the NE corner's ym mark at the
	 * corner's top-left in a *lightened* shade (ym_shadow=false
	 * on the NE row of the cm[] table) WHEN has_explicit is true.
	 * On a saturated explicit colour like magenta the lightened
	 * variant is nearly invisible, so the handle looks missing
	 * compared to the shadow-coloured marks on NW/SW/SE.  For
	 * that case only, repaint the mark in the shadow (darkened)
	 * variant so the handle reads cleanly.
	 *
	 * When bc->has_ne is false the mini-frame already drew the
	 * default mark (ec->n[1]) which is the right shade for the
	 * border's base colour, and overriding it here would change
	 * the handle's appearance on inactive windows where the user
	 * hasn't set colour.inactive.ne (the corner falls back to the
	 * N-edge colour and should use the matching default handle).
	 */
	if (!left_side && bc->has_ne) {
		int w_dout = (cow_config->border_style ==
				 COW_BORDER_STYLE_MWM) ?
		    0 :
		    1;
		/* inset = (w[5]>0 || w[6]>0) ? 1 : 0 -- is 1 for both
		 * FVWM (w[5]=w[6]=1) and MWM (w[5]=1) with default bws. */
		int inset    = 1;
		int mark_len = bw - w_dout - inset;
		int mark_thk = (cow_config->border_style ==
				   COW_BORDER_STYLE_MWM) ?
		    1 :
		    2;
		if (mark_len > 0) {
			float dk[4];
			cow_colour_darken(dk, corner_colour);
			uint32_t px_dk = cow_colour_to_pixel(dk);
			for (int k = 0; k < mark_thk; k++) {
				int len = mark_len - k;
				if (len <= 0)
					break;
				int x1 = mx + k;
				int y1 = w_dout;
				for (int y = y1; y < y1 + len; y++) {
					if (y < 0 || y >= frame_h)
						continue;
					if (x1 < 0 || x1 >= frame_w)
						continue;
					buf[y * stride_px + x1] = px_dk;
				}
			}
		}
	}

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

void
cow_decoration_init(struct cow_view_decoration *dec)
{
	memset(dec, 0, sizeof(*dec));
	dec->dirty	  = true;
	dec->border_width = cow_config ? cow_config->border :
					 COW_CFG_DEFAULT_BORDER;

	/* Enable titlebar when config says so */
	if (cow_config != NULL && cow_config->titlebar.enabled) {
		struct cow_font *font = get_titlebar_font();
		int		 h    = cow_config->titlebar.height;
		dec->title_thickness  = (h > 0) ?
		     h :
		     cow_titlebar_compute_thickness(font);
	} else {
		dec->title_thickness = 0;
	}
}

void
cow_decoration_fini(struct cow_view_decoration *dec)
{
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

	/* Apply per-view style overrides */
	if (view->style.resolved) {
		dec->border_width = view->style.border;
		if (view->style.titlebar_enabled) {
			if (dec->title_thickness == 0) {
				struct cow_font *font = get_titlebar_font();
				int		 h    = cow_config ?
						    cow_config->titlebar.height :
						    0;
				dec->title_thickness  = (h > 0) ?
				     h :
				     cow_titlebar_compute_thickness(font);
			}
		} else {
			dec->title_thickness = 0;
		}
	}

	int bw	       = dec->border_width;
	int tt	       = dec->title_thickness;
	int top_border = bw + tt;

	/* When shaded, use saved dimensions for frame size but zero content */
	int  cw, ch;
	bool shaded = (view->sd != COW_SHADE_NONE);

	if (shaded) {
		int sw = view->shade_saved_width;
		int sh = view->shade_saved_height;

		switch (view->sd) {
		case COW_SHADE_NORTH:
			cw = sw;
			ch = 0;
			break;
		case COW_SHADE_SOUTH:
			cw = sw;
			ch = 0;
			break;
		case COW_SHADE_EAST:
			cw = 0;
			ch = sh;
			break;
		case COW_SHADE_WEST:
			cw = 0;
			ch = sh;
			break;
		case COW_SHADE_NW:
		case COW_SHADE_NE:
		case COW_SHADE_SW:
		case COW_SHADE_SE:
			cw = 0;
			ch = 0;
			break;
		default:
			cw = sw;
			ch = 0;
			break;
		}
	} else {
		cw = view->content_width;
		ch = view->content_height;
	}

	int frame_w = cw + 2 * bw;
	int frame_h = ch + top_border + bw;

	if (frame_w <= 0 || frame_h <= 0)
		return;

	/* Check if we need to regenerate */
	bool needs_regen = dec->dirty || active != dec->last_active ||
	    cw != dec->last_content_width || ch != dec->last_content_height;

	if (!needs_regen && dec->buffer != NULL)
		goto commit;

	/* Free old buffer if dimensions changed or it's busy */
	if (dec->buffer != NULL &&
	    (dec->buffer->width != frame_w || dec->buffer->height != frame_h ||
		dec->buffer->busy)) {
		cow_shm_buffer_destroy(dec->buffer);
		dec->buffer = NULL;
	}

	/* Allocate buffer if needed */
	if (dec->buffer == NULL) {
		dec->buffer = cow_shm_buffer_create(frame_w, frame_h);
		if (dec->buffer == NULL) {
			fprintf(stderr,
			    "cow: failed to create decoration buffer\n");
			return;
		}
	}

	/* Render the frame using the view's resolved style */
	{
		int stride_px = dec->buffer->stride / 4;

		/* Use per-view style colors if overridden by a rule,
		 * otherwise fall back to global config per-edge colors. */
		const struct cow_border_colour *bc;
		if (view->style.resolved && view->style.has_border_override)
			bc = active ? &view->style.border_active :
				      &view->style.border_inactive;
		else
			bc = active ? &cow_config->border_active :
				      &cow_config->border_inactive;

		/* Render border: fill_border_base always runs for
		 * per-edge active/inactive colors; FVWM/MWM add 3D
		 * relief on top; NONE = flat fill only. */
		int cl = (cow_config->handles &&
			     cow_config->corner_length > 0) ?
		    cow_config->corner_length :
		    0;
		if (cl > 0 && tt > 0 && cl < bw + tt)
			cl = bw + tt;
		/*
		 * When this view is a container drop target, override the
		 * entire frame (borders + titlebar) with solid orange so the
		 * user sees a clear highlight indicating where the drop will
		 * land.
		 */
		bool is_drop_target = cow_view_is_drop_target(view);
		struct cow_border_colour drop_bc;
		if (is_drop_target) {
			memcpy(drop_bc.n, COW_COLOUR_DROP_ORANGE,
			    sizeof(float) * 4);
			memcpy(drop_bc.s, COW_COLOUR_DROP_ORANGE,
			    sizeof(float) * 4);
			memcpy(drop_bc.e, COW_COLOUR_DROP_ORANGE,
			    sizeof(float) * 4);
			memcpy(drop_bc.w, COW_COLOUR_DROP_ORANGE,
			    sizeof(float) * 4);
			memcpy(drop_bc.nw, COW_COLOUR_DROP_ORANGE,
			    sizeof(float) * 4);
			memcpy(drop_bc.ne, COW_COLOUR_DROP_ORANGE,
			    sizeof(float) * 4);
			memcpy(drop_bc.sw, COW_COLOUR_DROP_ORANGE,
			    sizeof(float) * 4);
			memcpy(drop_bc.se, COW_COLOUR_DROP_ORANGE,
			    sizeof(float) * 4);
			drop_bc.has_nw = drop_bc.has_ne = true;
			drop_bc.has_sw = drop_bc.has_se = true;
			drop_bc.has_corners		= true;
			bc				= &drop_bc;
		}

		cow_border_style_render(dec->buffer->data, stride_px, frame_w,
		    frame_h, bc, bw, cl, cow_config->border_style);

		/* Render titlebar if enabled */
		if (tt > 0) {
			struct cow_font *font = get_titlebar_font();
			/* Use per-view titlebar colors if overridden */
			const float *tb_colour = NULL;
			const float *tb_fg     = NULL;
			if (view->style.resolved &&
			    view->style.has_titlebar_override) {
				tb_colour = active ?
				    view->style.titlebar_active :
				    view->style.titlebar_inactive;
			}
			if (view->style.resolved &&
			    view->style.has_titlebar_fg_override) {
				tb_fg = active ?
				    view->style.titlebar_fg_active :
				    view->style.titlebar_fg_inactive;
			}
			bool is_sticky = (view->flags & COW_VIEW_STICKY_FLAG) !=
			    0;

			/* Override titlebar to orange when this is a drop
			 * target */
			if (is_drop_target) {
				tb_colour = (float *)COW_COLOUR_DROP_ORANGE;
				tb_fg	  = (float *)COW_COLOUR_WHITE;
			}

			/* Build tab info if this view is in a container */
			const char	    **tab_titles = NULL;
			int		      n_tabs	 = 0;
			int		      active_tab = 0;
			struct cow_container *ctr = cow_view_container(view);
			if (ctr != NULL && ctr->n_members > 1) {
				n_tabs	   = ctr->n_members;
				tab_titles = xcalloc(n_tabs, sizeof(char *));
				int			     idx = 0;
				struct cow_container_member *mem;
				wl_list_for_each(mem, &ctr->members, link)
				{
					const char *lbl = mem->view->app_id;
					if (lbl == NULL)
						lbl = mem->view->title;
					if (lbl == NULL)
						lbl = "?";
					tab_titles[idx] = lbl;
					if (mem == ctr->active)
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
			struct cow_box tb_rect = cow_titlebar_squeezed_rect(
			    view);
			cow_titlebar_render(dec->buffer->data, stride_px,
			    frame_w, bw, tb_rect, display_title, active, font,
			    view->pressed_button, view->icon_inverted_button,
			    view->title_pressed, tb_colour, tb_fg, is_sticky,
			    tab_titles, n_tabs, active_tab, is_drop_target);
			xfree(display_title);
			xfree(tab_titles);
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
		 *     the east border using the *unconfigured* default
		 *     corner join (no explicit NE swatch).
		 *
		 * The continued segment's relief is borrowed from a
		 * scratch render that has no explicit NW/NE colours, so
		 * the highlight/shadow pattern at the frame's top-right
		 * corner matches what an un-squeezed frame would produce.
		 *
		 *   LEFT justify  -- relocated NE at tbr.x + tbr.width.
		 *   RIGHT justify -- mirror (NW marker): relocated at
		 *                    tbr.x - cl.
		 *   CENTRE        -- not stepped: titlebar sits centred
		 *                    beneath an unbroken north border.
		 */
		if (tt > 0 && bw > 0 && !shaded) {
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
				struct cow_box tbr = cow_titlebar_squeezed_rect(
				    view);
				int marker_w = cl > 0 ? cl : bw;
				int top_band = bw + tt;

				/*
				 * Scratch render with no explicit NW/NE and
				 * corner_length=0 -- gives default 3D relief
				 * without drawing handle-mark dashes at the
				 * relocated corner positions (confusing,
				 * since the corners have moved).
				 */
				struct cow_border_colour plain = *bc;
				plain.has_nw		       = false;
				plain.has_ne		       = false;
				size_t nbytes = (size_t)stride_px *
				    (size_t)frame_h * 4;
				uint32_t *scratch = xmalloc(nbytes);
				cow_border_style_render(scratch, stride_px,
				    frame_w, frame_h, &plain, bw,
				    /* cl = */ 0, cow_config->border_style);

				/* Corner colours (fall back to plain N if
				 * not explicitly configured). */
				const float *n_colour  = active ?
				     cow_config->border_active.n :
				     cow_config->border_inactive.n;
				const float *ne_colour = bc->has_ne ? bc->ne :
								      n_colour;
				const float *nw_colour = bc->has_nw ? bc->nw :
								      n_colour;

				/*
				 * LEFT  -- paint right side only (NE at
				 *          tbr's right edge).
				 * RIGHT -- paint left side only (NW at
				 *          tbr's left edge).
				 * CENTRE -- paint both (NW at left edge,
				 *          NE at right edge); top border
				 *          steps down on both flanks.
				 */
				if (sq.justify == COW_JUST_LEFT ||
				    sq.justify == COW_JUST_CENTRE) {
					squeeze_paint_side(dec->buffer->data,
					    stride_px, frame_w, frame_h, bw, cl,
					    tt, top_band, marker_w,
					    tbr.x + tbr.width,
					    /* left_side = */ false, ne_colour,
					    bc, scratch);
				}
				if (sq.justify == COW_JUST_RIGHT ||
				    sq.justify == COW_JUST_CENTRE) {
					squeeze_paint_side(dec->buffer->data,
					    stride_px, frame_w, frame_h, bw, cl,
					    tt, top_band, marker_w, tbr.x,
					    /* left_side = */ true, nw_colour,
					    bc, scratch);
				}

				xfree(scratch);
			}
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
	river_decoration_v1_set_offset(view->decoration, -bw, -top_border);

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
		/* Top border spans the full frame width. */
		if (bw > 0)
			wl_region_add(region, 0, 0, frame_w, bw);
		/* Titlebar: the (possibly squeezed) rectangle only. */
		if (tt > 0) {
			struct cow_box tbr = cow_titlebar_squeezed_rect(view);
			if (tbr.width > 0 && tbr.height > 0)
				wl_region_add(region, tbr.x, tbr.y, tbr.width,
				    tbr.height);
		}
		/* Left/right side borders next to content. */
		if (bw > 0) {
			wl_region_add(region, 0, top_border, bw,
			    frame_h - top_border - bw);
			wl_region_add(region, frame_w - bw, top_border, bw,
			    frame_h - top_border - bw);
		}
		/* Bottom border. */
		if (bw > 0)
			wl_region_add(region, 0, frame_h - bw, frame_w, bw);

		wl_surface_set_input_region(view->decoration_surface, region);
		wl_region_destroy(region);
	}

	/* Attach buffer and commit */
	if (dec->buffer != NULL) {
		wl_surface_attach(view->decoration_surface, dec->buffer->buffer,
		    0, 0);
		wl_surface_damage_buffer(view->decoration_surface, 0, 0,
		    frame_w, frame_h);
		river_decoration_v1_sync_next_commit(view->decoration);
		wl_surface_commit(view->decoration_surface);
		dec->buffer->busy = true;
	}
}
