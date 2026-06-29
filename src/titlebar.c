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
#include "cow/titlebar.h"

#include <limits.h>
#include <string.h>

#include <cairo.h>
#include <pango/pangocairo.h>

#include "cow/colour.h"
#include "cow/compat.h"
#include "cow/config.h"
#include "cow/decoration.h"
#include "cow/draw.h"
#include "cow/font.h"
#include "cow/memory.h"
#include "cow/output.h"
#include "cow/vector.h"
#include "cow/view.h"

int
cow_titlebar_compute_thickness(struct cow_font *font)
{
	int raw_height = font->height - 8;
	if (raw_height < 12)
		raw_height = 12;
	return raw_height + COW_TITLEBAR_PADDING * 2;
}

void
cow_titlebar_effective_squeeze(const struct cow_view *view,
    struct cow_titlebar_squeeze			     *out)
{
	/*
	 * Containers with more than one member render a tab strip and
	 * cannot compose cleanly with squeeze; force NONE there.
	 */
	struct cow_container *ctr = cow_view_container(view);
	if (ctr != NULL && ctr->n_members > 1) {
		out->mode      = COW_SQUEEZE_NONE;
		out->justify   = COW_JUST_LEFT;
		out->width     = 0;
		out->offset    = 0;
		out->min_width = 0;
		return;
	}

	/* Read from the view's snapshot.  on-map / on-desk rules that
	 * write `set titlebar.squeeze ...` against this view's target
	 * land in view->cfg, so this read picks them up automatically. */
	*out = view->cfg.titlebar.squeeze;
}

/*
 * The drawn titlebar rectangle inside the decoration buffer.
 * Coordinates match the (tb_x, tb_y, tb_w, tb_h) used by
 * cow_titlebar_render: tb_x = border_width, tb_y = border_width,
 * tb_w spans the frame inside the left/right borders.
 */
struct cow_box
cow_titlebar_squeezed_rect(const struct cow_view *view)
{
	int bw	    = view->dec.border_width;
	int tt	    = view->dec.title_thickness;
	int frame_w = view->content_width + 2 * bw;
	int strip_x = bw;
	int strip_y = bw;
	int strip_w = frame_w - 2 * bw;

	struct cow_box empty = { 0, 0, 0, 0 };
	if (tt <= 0 || strip_w <= 0)
		return empty;

	struct cow_titlebar_squeeze sq;
	cow_titlebar_effective_squeeze(view, &sq);

	int want;
	if (sq.mode == COW_SQUEEZE_NONE) {
		return (struct cow_box) {
			.x	= strip_x,
			.y	= strip_y,
			.width	= strip_w,
			.height = tt,
		};
	} else if (sq.mode == COW_SQUEEZE_FIXED) {
		want = sq.width > 0 ? sq.width : 1;
	} else { /* NATURAL */
		/*
		 * Use pango to measure the actual rendered width of the
		 * display title, plus the fixed button run (3 buttons
		 * each of width tt), plus internal padding that matches
		 * the text_inset + right_pad used by cow_titlebar_render.
		 */
		int   btn_run	    = tt * COW_TITLEBAR_NUM_BUTTONS;
		int   text	    = 0;
		char *display_title = cow_decoration_display_title(view);
		if (display_title != NULL && display_title[0] != '\0') {
			struct cow_font *font = cow_decoration_titlebar_font();
			text = cow_font_text_width(font, display_title);
		}
		xfree(display_title);
		/*
		 * The renderer reserves 2 + PADDING at the left of the
		 * text and a COW_TITLEBAR_PADDING + 1 margin to the right
		 * button, plus a further PADDING.  Mirror that here so
		 * the rect hugs the text+buttons flush.
		 */
		int text_inset = 2 + COW_TITLEBAR_PADDING;
		int text_pad = COW_TITLEBAR_PADDING + 1 + COW_TITLEBAR_PADDING;
		want	     = btn_run + text + text_inset + text_pad;
		if (sq.min_width > 0 && want < sq.min_width)
			want = sq.min_width;
	}

	if (want > strip_w)
		want = strip_w;
	if (want < 1)
		want = 1;

	int x;
	switch (sq.justify) {
	case COW_JUST_CENTRE:
		x = strip_x + (strip_w - want) / 2;
		break;
	case COW_JUST_RIGHT:
		x = strip_x + strip_w - want;
		break;
	case COW_JUST_LEFT:
	default:
		x = strip_x;
		break;
	}
	x += sq.offset;

	/* Clamp so the squeezed rect stays inside the strip. */
	if (x < strip_x)
		x = strip_x;
	if (x + want > strip_x + strip_w)
		x = strip_x + strip_w - want;

	return (struct cow_box) {
		.x	= x,
		.y	= strip_y,
		.width	= want,
		.height = tt,
	};
}

bool
cow_titlebar_hit(const struct cow_view *view, int lx, int ly)
{
	int bw = view->dec.border_width;
	int tt = view->dec.title_thickness;

	if (tt <= 0)
		return false;

	struct cow_box r = cow_titlebar_squeezed_rect(view);
	if (r.width <= 0 || r.height <= 0)
		return false;

	/*
	 * lx, ly are view-relative: (0, 0) is the content's top-left.
	 * The decoration buffer origin is (-bw, -bw - tt) in that
	 * space, so a dec-coord (dx, dy) corresponds to
	 *   lx = dx - bw
	 *   ly = dy - bw - tt.
	 * The titlebar rect's y is always bw (top border), so the
	 * vertical test in view-relative space is ly ∈ [-tt, 0).
	 */
	if (ly < -tt || ly >= 0)
		return false;

	int left  = r.x - bw;
	int right = left + r.width;
	return lx >= left && lx < right;
}

/*
 * Draw FVWM-style raised relief: 1px highlight on top+left,
 * 1px shadow on bottom+right, matching fvwm's corner ownership.
 */
/*
 * Draw 2-ring concentric FVWM relief matching the original titlebar.c
 * draw_fvwm_relief (2px raised bevel = hiout + shin layers).
 */
static void
draw_fvwm_relief(uint32_t *pixels, int stride, int rx, int ry, int rw, int rh,
    uint32_t px_lt, uint32_t px_dk, bool sunken)
{
	/*
	 * FVWM-style 2px bevel.  Note that the corner ownership rule
	 * differs from cow_draw_relieve_rect: here the bottom edge
	 * overdraws the upper-left ring at the bottom-left, and the
	 * right edge overdraws at the top-right; effectively each ring
	 * paints all four edges and corners go to whichever direction
	 * is drawn last.  We paint via cow_pixbuf so off-canvas
	 * coordinates are clipped consistently with the rest of the
	 * decoration code.
	 */
	struct cow_pixbuf p = {
		.pixels	   = pixels,
		.stride_px = stride,
		.width	   = INT_MAX,
		.height	   = INT_MAX,
	};
	uint32_t ul = sunken ? px_dk : px_lt;
	uint32_t br = sunken ? px_lt : px_dk;

	for (int r = 0; r < 2 && r < rw / 2 && r < rh / 2; r++) {
		int y1 = ry + r + 1;

		if (r > 0)
			cow_draw_hline(&p, rx + r, rx + rw - 1 - r, ry + r,
			    ul);
		cow_draw_vline(&p, rx + r, y1, ry + rh - 1 - r, ul);
		cow_draw_hline(&p, rx + r, rx + rw - 1 - r, ry + rh - 1 - r,
		    br);
		cow_draw_vline(&p, rx + rw - 1 - r, y1, ry + rh - 1 - r,
		    br);
	}
}

void
cow_titlebar_render(const struct cow_titlebar_render_ctx *ctx)
{
	uint32_t			      *pixels = ctx->pixels;
	int			       stride_px = ctx->stride_px;
	int			       frame_w = ctx->frame_w;
	int			       border_width = ctx->border_width;
	struct cow_box		       tb_rect = ctx->tb_rect;
	const char		      *title = ctx->title;
	bool			       active = ctx->active;
	struct cow_font		      *font = ctx->font;
	int			       pressed_button = ctx->pressed_button;
	int			       icon_inverted_button =
				    ctx->icon_inverted_button;
	bool			       title_pressed = ctx->title_pressed;
	const struct cow_titlebar_config *tb_cfg = ctx->tb_cfg;
	const struct cow_container_config *cc = ctx->container_cfg;
	const float		      *style_colour = ctx->style_colour;
	const float		      *style_fg = ctx->style_fg;
	bool			       sticky = ctx->sticky;
	const char		     **tab_titles = ctx->tab_titles;
	int			       n_tabs = ctx->n_tabs;
	int			       active_tab = ctx->active_tab;
	bool			       drop_target = ctx->drop_target;

	if (tb_rect.height <= 0 || tb_rect.width <= 0 ||
	    frame_w <= 2 * border_width)
		return;

	/* Titlebar colors: use style override if provided, otherwise
	 * fall back to the supplied per-view tb_cfg.  Callers always
	 * supply tb_cfg from the view's snapshot, so the NULL branch
	 * below only exists for defensive symmetry with the legacy
	 * cow_config-may-be-NULL paths. */
	float base[4], fg[4];
	if (style_colour != NULL) {
		memcpy(base, style_colour, sizeof(float) * 4);
		if (style_fg != NULL)
			memcpy(fg, style_fg, sizeof(float) * 4);
		else if (tb_cfg != NULL)
			memcpy(fg,
			    active ? tb_cfg->fg_active : tb_cfg->fg_inactive,
			    sizeof(float) * 4);
		else
			cow_colour_convert(fg, 0x000000);
	} else if (tb_cfg != NULL) {
		memcpy(base,
		    active ? tb_cfg->active_colour : tb_cfg->inactive_colour,
		    sizeof(float) * 4);
		memcpy(fg, active ? tb_cfg->fg_active : tb_cfg->fg_inactive,
		    sizeof(float) * 4);
	} else {
		cow_colour_convert(base, active ? 0xC0C0C0 : 0xA0A0A0);
		cow_colour_convert(fg, 0x000000);
	}

	float lt[4], dk[4];
	cow_colour_lighten(lt, base);
	cow_colour_darken(dk, base);

	uint32_t px_base = cow_colour_to_pixel(base);
	uint32_t px_lt	 = cow_colour_to_pixel(lt);
	uint32_t px_dk	 = cow_colour_to_pixel(dk);
	uint32_t px_fg	 = cow_colour_to_pixel(fg);
	static const float transparent[4] = { 0, 0, 0, 0 };

	/*
	 * Titlebar geometry within the decoration frame.  The caller
	 * passes the exact rectangle it wants painted; for unsqueezed
	 * views this is the full inner strip, for squeezed ones it is
	 * narrower and positioned by effective justify + offset.  The
	 * rest of the full-width strip (between the left/right borders)
	 * is cleared to transparent so those pixels fall through.
	 */
	int tb_x = tb_rect.x;
	int tb_y = tb_rect.y;
	int tb_w = tb_rect.width;
	int tb_h = tb_rect.height;

	int strip_x = border_width;
	int strip_y = border_width;
	int strip_w = frame_w - 2 * border_width;

	if (tb_w <= 0 || strip_w <= 0)
		return;

	/* Clear the full-width strip to transparent, then fill only the
	 * squeezed rect with the base colour. */
	struct cow_pixbuf strip_p = {
		.pixels	   = pixels,
		.stride_px = stride_px,
		.width	   = INT_MAX,
		.height	   = INT_MAX,
	};
	cow_draw_clear_rect(&strip_p, strip_x, strip_y, strip_w, tb_h);
	cow_draw_fill_rect(&strip_p, tb_x, tb_y, tb_w, tb_h, px_base);
	const struct cow_gradient *tb_gradient = NULL;
	if (style_colour == NULL && tb_cfg != NULL)
		tb_gradient = active ? &tb_cfg->active_gradient :
				       &tb_cfg->inactive_gradient;
	if (tb_gradient != NULL && tb_gradient->mode != COW_GRADIENT_NONE)
		cow_draw_gradient_rect(&strip_p, tb_x, tb_y, tb_w, tb_h,
		    tb_gradient);

	/* Button size = titlebar height */
	int btn_s      = tb_h;
	int left_btns  = COW_TITLEBAR_LEFT_BUTTONS;
	int right_btns = COW_TITLEBAR_RIGHT_BUTTONS;

	/* Vector button colour palette */
	uint32_t colors[5];
	colors[COW_VECTOR_SHADOW]    = px_dk;
	colors[COW_VECTOR_HIGHLIGHT] = px_lt;
	colors[COW_VECTOR_BG]	     = px_base;
	colors[COW_VECTOR_FG]	     = px_fg;
	colors[COW_VECTOR_INVISIBLE] = 0;

	/* Get button vectors from config or use defaults */
	const struct cow_vector_button	     *buttons[3];
	static const struct cow_vector_button default_buttons[3] = {
		{ .num_points = 5,
		    .points   = { { 25, 25, 0, 0, -1 }, { 75, 75, 0, 0, 3 },
			  { 75, 25, 0, 0, -1 }, { 25, 75, 0, 0, 3 },
			  { 25, 25, 0, 0, -1 } } },
		{ .num_points = 5,
		    .points   = { { 25, 25, 0, 0, -1 }, { 25, 75, 0, 0, 3 },
			  { 75, 75, 0, 0, 3 }, { 75, 25, 0, 0, 3 },
			  { 25, 25, 0, 0, 3 } }	},
		{ .num_points = 5,
		    .points   = { { 25, 65, 0, 0, -1 }, { 75, 65, 0, 0, 3 },
			  { 75, 75, 0, 0, 3 }, { 25, 75, 0, 0, 3 },
			  { 25, 65, 0, 0, 3 } }	},
	};
	for (int i = 0; i < 3; i++) {
		if (tb_cfg != NULL && tb_cfg->buttons[i].vector.num_points > 0)
			buttons[i] = &tb_cfg->buttons[i].vector;
		else
			buttons[i] = &default_buttons[i];
	}

	/* Render left button(s) */
	for (int i = 0; i < left_btns && i < 3; i++) {
		int bx = tb_x + i * btn_s;
		int by = tb_y;
		if (bx + btn_s > tb_x + tb_w)
			break;

		bool btn_sunken = (pressed_button == i);

		if (tb_gradient == NULL ||
		    tb_gradient->mode == COW_GRADIENT_NONE) {
			for (int py = by; py < by + btn_s; py++)
				for (int px = bx; px < bx + btn_s; px++)
					pixels[py * stride_px + px] = px_base;
		}

		/* FVWM relief on button: sunken if pressed */
		draw_fvwm_relief(pixels, stride_px, bx, by, btn_s, btn_s, px_lt,
		    px_dk, btn_sunken);

		/* Draw vector into temp buffer then blit */
		uint32_t btn_buf[btn_s * btn_s];
		for (int p = 0; p < btn_s * btn_s; p++)
			btn_buf[p] = px_base;

		bool icon_inverted = (icon_inverted_button == i) || btn_sunken;
		cow_vector_button_draw(buttons[i], btn_buf, btn_s, btn_s, btn_s,
		    btn_s, colors, icon_inverted);

		for (int py = 1; py < btn_s - 1; py++) {
			for (int px = 1; px < btn_s - 1; px++) {
				uint32_t c = btn_buf[py * btn_s + px];
				if (c != px_base)
					pixels[(by + py) * stride_px +
					    (bx + px)] = c;
			}
		}
	}

	/* Render right button(s) */
	for (int i = 0; i < right_btns && (left_btns + i) < 3; i++) {
		int btn_idx    = left_btns + i;
		int from_right = right_btns - 1 - i;
		int bx	       = tb_x + tb_w - (from_right + 1) * btn_s;
		int by	       = tb_y;
		if (bx < tb_x)
			break;

		bool btn_sunken = (pressed_button == btn_idx);

		if (tb_gradient == NULL ||
		    tb_gradient->mode == COW_GRADIENT_NONE) {
			for (int py = by; py < by + btn_s; py++)
				for (int px = bx; px < bx + btn_s; px++)
					pixels[py * stride_px + px] = px_base;
		}

		/* FVWM relief on button: sunken if pressed */
		draw_fvwm_relief(pixels, stride_px, bx, by, btn_s, btn_s, px_lt,
		    px_dk, btn_sunken);

		uint32_t btn_buf[btn_s * btn_s];
		for (int p = 0; p < btn_s * btn_s; p++)
			btn_buf[p] = px_base;

		bool icon_inv = (icon_inverted_button == btn_idx) || btn_sunken;
		cow_vector_button_draw(buttons[btn_idx], btn_buf, btn_s, btn_s,
		    btn_s, btn_s, colors, icon_inv);

		for (int py = 1; py < btn_s - 1; py++) {
			for (int px = 1; px < btn_s - 1; px++) {
				uint32_t c = btn_buf[py * btn_s + px];
				if (c != px_base)
					pixels[(by + py) * stride_px +
					    (bx + px)] = c;
			}
		}
	}

	/* Title text area: between left and right buttons */
	int title_x	    = tb_x + left_btns * btn_s;
	int title_w	    = tb_w - (left_btns + right_btns) * btn_s;
	int rendered_text_w = 0; /* actual rendered text width */

	/* Render title text with cairo/pango */
	if (title != NULL && title_w > 4 && font != NULL) {
		const float *label_bg = (tb_gradient != NULL &&
		    tb_gradient->mode != COW_GRADIENT_NONE) ?
		    transparent :
		    base;
		struct cow_pixbuf dst = {
			.pixels	   = pixels,
			.stride_px = stride_px,
			.width	   = INT_MAX,
			.height	   = INT_MAX,
		};
		cow_font_render_label(&dst, title_x, tb_y, title_w, tb_h, font,
		    title, label_bg, fg, COW_TITLEBAR_PADDING,
		    &rendered_text_w);
	}

	/* Draw stipple for sticky windows in the empty areas
	 * either side of the title text. */
	if (sticky && title_w > 4) {
		int relief = 2;
		int pad	   = COW_TITLEBAR_PADDING;

		/* Centre the text gap within the title area */
		int text_centre = title_x + title_w / 2;
		int text_start	= text_centre - rendered_text_w / 2 - pad;
		int text_end	= text_centre + (rendered_text_w + 1) / 2 + pad;

		int sx1 = title_x + relief;
		int sx2 = title_x + title_w - relief;

		for (int y = tb_y + relief; y < tb_y + tb_h - relief; y++) {
			/* Left stipple area */
			for (int x = sx1; x < text_start && x < sx2; x++) {
				if ((x + y) % 2 == 0)
					pixels[y * stride_px + x] = px_lt;
				else
					pixels[y * stride_px + x] = px_dk;
			}
			/* Right stipple area */
			for (int x = text_end > sx1 ? text_end : sx1; x < sx2;
			    x++) {
				if ((x + y) % 2 == 0)
					pixels[y * stride_px + x] = px_lt;
				else
					pixels[y * stride_px + x] = px_dk;
			}
		}
	}

	/*
	 * Container tab strip.  When n_tabs > 1, divide the title area into
	 * equal-width segments.  Active/inactive tab colours come from the
	 * cc parameter (the view's cow_view_cfg.container snapshot) so
	 * users can style them per-view via 'set container.tab.*'.  Falls
	 * back to the titlebar base/dk colours when cc is NULL.
	 */
	if (n_tabs > 1 && tab_titles != NULL && title_w > 0) {
		/*
		 * Resolve tab colours.  The container config allows explicit
		 * overrides; the defaults use the titlebar's own base/dk for
		 * backgrounds and the titlebar's fg for both label colours so
		 * that text remains readable regardless of how dark the
		 * inactive (dk) background is.  cc was passed in by the
		 * caller from the view's snapshot. */

		float tab_active_f[4], tab_inactive_f[4];
		float tab_fg_active_f[4], tab_fg_inactive_f[4];

		/*
		 * Use configured value if explicitly set, otherwise inherit
		 * from the titlebar's own base/dk/fg so tabs match the window
		 * style.
		 *
		 * The overrides are only applied when the window is focused
		 * (active=true).  When unfocused, base/dk/fg already reflect
		 * the inactive titlebar colours (set above from
		 * tb_cfg->inactive_colour etc.), so all tabs correctly adopt
		 * the unfocused palette instead of the configured focused
		 * colours bleeding through.
		 */
		/*
		 * When unfocused (!active), base already holds
		 * titlebar.inactive_colour. All tabs — both the active
		 * container tab and inactive ones — use the same base colour,
		 * matching the flat single-colour look of a normal unfocused
		 * titlebar.  Configured container.tab.* overrides are only
		 * applied when the window is focused.
		 */
		const float *unfocused_bg =
		    base;			/* = titlebar.inactive_colour */
		const float *unfocused_fg = fg; /* = titlebar.fg_inactive     */
		memcpy(tab_active_f,
		    (active && cc && cc->has_tab_active) ?
			cc->tab_active :
			(active ? base : unfocused_bg),
		    sizeof(float) * 4);
		memcpy(tab_inactive_f,
		    (active && cc && cc->has_tab_inactive) ?
			cc->tab_inactive :
			(active ? dk : unfocused_bg),
		    sizeof(float) * 4);
		memcpy(tab_fg_active_f,
		    (active && cc && cc->has_tab_fg_active) ?
			cc->tab_fg_active :
			(active ? fg : unfocused_fg),
		    sizeof(float) * 4);
		memcpy(tab_fg_inactive_f,
		    (active && cc && cc->has_tab_fg_inactive) ?
			cc->tab_fg_inactive :
			(active ? fg : unfocused_fg),
		    sizeof(float) * 4);

		/* Pre-compute bevel colours for active and inactive tabs */
		struct cow_colour_triple act_t, inact_t;
		cow_colour_make_triple(&act_t, tab_active_f);
		cow_colour_make_triple(&inact_t, tab_inactive_f);
		uint32_t px_act	     = act_t.base;
		uint32_t px_inact    = inact_t.base;
		uint32_t px_act_lt   = act_t.lt;
		uint32_t px_act_dk   = act_t.dk;
		uint32_t px_inact_lt = inact_t.lt;
		uint32_t px_inact_dk = inact_t.dk;

		int tab_w = title_w / n_tabs;
		if (tab_w < 1)
			tab_w = 1;

		for (int ti = 0; ti < n_tabs; ti++) {
			int tx = title_x + ti * tab_w;
			int tw = (ti == n_tabs - 1) ? (title_x + title_w - tx) :
						      tab_w;
			bool is_active = (ti == active_tab);

			uint32_t tab_bg_px = is_active ? px_act : px_inact;
			uint32_t tab_bv_lt = is_active ? px_act_lt :
							 px_inact_lt;
			uint32_t tab_bv_dk = is_active ? px_act_dk :
							 px_inact_dk;
			float	*tab_bg_f  = is_active ? tab_active_f :
							 tab_inactive_f;
			float	*tab_fg	   = is_active ? tab_fg_active_f :
							 tab_fg_inactive_f;

			/* Tab background */
			for (int py = tb_y; py < tb_y + tb_h; py++)
				for (int px = tx; px < tx + tw; px++)
					pixels[py * stride_px + px] = tab_bg_px;
			const struct cow_gradient *tab_gradient = NULL;
			if (active && cc != NULL)
				tab_gradient = is_active ?
				    &cc->tab_active_gradient :
				    &cc->tab_inactive_gradient;
			if (tab_gradient != NULL &&
			    tab_gradient->mode != COW_GRADIENT_NONE) {
				struct cow_pixbuf dst = {
					.pixels	   = pixels,
					.stride_px = stride_px,
					.width	   = INT_MAX,
					.height	   = INT_MAX,
				};
				cow_draw_gradient_rect(&dst, tx, tb_y, tw, tb_h,
				    tab_gradient);
			}

			/* Tab bevel — active is always raised; inactive is
			 * sunken only when container.tab.sunken is true
			 * (default: flat). */
			bool sunken = !is_active && cc != NULL &&
			    cc->tab_sunken_inactive;
			draw_fvwm_relief(pixels, stride_px, tx, tb_y, tw, tb_h,
			    tab_bv_lt, tab_bv_dk, sunken);

			/* Tab label text */
			const char *lbl = tab_titles[ti];
			if (lbl != NULL && tw > 6 && font != NULL) {
				const float *label_bg = (tab_gradient != NULL &&
				    tab_gradient->mode != COW_GRADIENT_NONE) ?
				    transparent :
				    tab_bg_f;
				struct cow_pixbuf dst = {
					.pixels	   = pixels,
					.stride_px = stride_px,
					.width	   = INT_MAX,
					.height	   = INT_MAX,
				};
				cow_font_render_label(&dst, tx, tb_y, tw, tb_h,
				    font, lbl, label_bg, tab_fg,
				    COW_TITLEBAR_PADDING, NULL);
			}
		}
	} else {
		/* Draw relief on title text area LAST so it isn't overwritten.
		 * Sunken when title is being dragged, raised otherwise. */
		if (title_w > 2) {
			draw_fvwm_relief(pixels, stride_px, title_x, tb_y,
			    title_w, tb_h, px_lt, px_dk, title_pressed);
		}
	}

	/*
	 * Drop-target indicator: draw a 2px bright highlight ring around
	 * the full titlebar area to signal that dropping here will
	 * containerise the two windows.
	 */
	if (drop_target) {
		/* Bright accent ring (project-wide drop-target orange). */
		uint32_t px_hi = cow_colour_to_pixel(COW_COLOUR_DROP_ORANGE);
		struct cow_pixbuf p = {
			.pixels	   = pixels,
			.stride_px = stride_px,
			.width	   = INT_MAX,
			.height	   = INT_MAX,
		};
		cow_draw_relieve_rect(&p, tb_x, tb_y, tb_w, tb_h, 2, px_hi,
		    px_hi, px_hi, px_hi);
	}
}
