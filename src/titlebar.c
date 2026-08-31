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
#include "cow/image.h"
#include "cow/memory.h"
#include "cow/output.h"
#include "cow/vector.h"
#include "cow/view.h"
#include "cow/options.h"

int
cow_titlebar_compute_thickness(struct cow_font *font)
{
	int raw_height = font->height - 8;
	if (raw_height < 12)
		raw_height = 12;
	return raw_height + COW_TITLEBAR_PADDING * 2;
}

static bool
titlebar_button_visible(const struct cow_titlebar_config *tb, int idx)
{
	return tb != NULL && idx >= 0 && idx < COW_TITLEBAR_NUM_BUTTONS &&
	    (tb->buttons[idx].vector.num_points > 0 ||
		tb->buttons[idx].image != NULL);
}

static int
titlebar_visible_button_count(const struct cow_titlebar_config *tb)
{
	int n = 0;
	for (int i = 0; i < COW_TITLEBAR_NUM_BUTTONS; i++) {
		if (titlebar_button_visible(tb, i))
			n++;
	}
	return n;
}

bool
cow_titlebar_is_vertical(enum cow_titlebar_position position)
{
	return (position == COW_TITLEBAR_LEFT ||
	    position == COW_TITLEBAR_RIGHT);
}

const char *
cow_titlebar_position_name(enum cow_titlebar_position position)
{
	switch (position) {
	case COW_TITLEBAR_BOTTOM:
		return "bottom";
	case COW_TITLEBAR_LEFT:
		return "left";
	case COW_TITLEBAR_RIGHT:
		return "right";
	case COW_TITLEBAR_TOP:
	default:
		return "top";
	}
}

void
cow_titlebar_frame_geometry(const struct cow_view *view, int cw, int ch,
    int *frame_w, int *frame_h, int *content_x, int *content_y)
{
	int bw = view->dec.border_width;
	int tt = view->dec.title_thickness;
	enum cow_titlebar_position pos = view->cfg.titlebar.position;

	if (cow_titlebar_is_vertical(pos)) {
		*frame_w = cw + 2 * bw + tt;
		*frame_h = ch + 2 * bw;
	} else {
		*frame_w = cw + 2 * bw;
		*frame_h = ch + 2 * bw + tt;
	}

	switch (pos) {
	case COW_TITLEBAR_BOTTOM:
	case COW_TITLEBAR_RIGHT:
		*content_x = bw;
		*content_y = bw;
		break;
	case COW_TITLEBAR_LEFT:
		*content_x = bw + tt;
		*content_y = bw;
		break;
	case COW_TITLEBAR_TOP:
	default:
		*content_x = bw;
		*content_y = bw + tt;
		break;
	}
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
	 * apply a decor profile against this view's target
	 * land in view->cfg, so this read picks them up automatically. */
	*out = view->cfg.titlebar.squeeze;
}

struct cow_box
cow_titlebar_squeezed_rect(const struct cow_view *view)
{
	return cow_titlebar_squeezed_rect_for(view, view->content_width,
	    view->content_height);
}

struct cow_box
cow_titlebar_squeezed_rect_for(const struct cow_view *view, int cw, int ch)
{
	int  bw = view->dec.border_width;
	int  tt = view->dec.title_thickness;
	bool vertical = cow_titlebar_is_vertical(view->cfg.titlebar.position);
	int  strip_x, strip_y, strip_w, strip_h;

	struct cow_box empty = { 0, 0, 0, 0 };

	switch (view->cfg.titlebar.position) {
	case COW_TITLEBAR_BOTTOM:
		strip_x = bw;
		strip_y = bw + ch;
		strip_w = cw;
		strip_h = tt;
		break;
	case COW_TITLEBAR_LEFT:
		strip_x = bw;
		strip_y = bw;
		strip_w = tt;
		strip_h = ch;
		break;
	case COW_TITLEBAR_RIGHT:
		strip_x = bw + cw;
		strip_y = bw;
		strip_w = tt;
		strip_h = ch;
		break;
	case COW_TITLEBAR_TOP:
	default:
		strip_x = bw;
		strip_y = bw;
		strip_w = cw;
		strip_h = tt;
		break;
	}

	int strip_len = vertical ? strip_h : strip_w;
	if (tt <= 0 || strip_len <= 0)
		return empty;

	struct cow_titlebar_squeeze sq;
	cow_titlebar_effective_squeeze(view, &sq);

	int want;
	if (sq.mode == COW_SQUEEZE_NONE) {
		return (struct cow_box) {
			.x	= strip_x,
			.y	= strip_y,
			.width	= strip_w,
			.height = strip_h,
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
		int btn_run = tt *
		    titlebar_visible_button_count(&view->cfg.titlebar);
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

	if (want > strip_len)
		want = strip_len;
	if (want < 1)
		want = 1;

	int start;
	switch (sq.justify) {
	case COW_JUST_CENTRE:
		start = (strip_len - want) / 2;
		break;
	case COW_JUST_RIGHT:
		start = strip_len - want;
		break;
	case COW_JUST_LEFT:
	default:
		start = 0;
		break;
	}
	start += sq.offset;

	if (start < 0)
		start = 0;
	if (start + want > strip_len)
		start = strip_len - want;

	if (vertical) {
		return (struct cow_box) {
			.x	= strip_x,
			.y	= strip_y + start,
			.width	= strip_w,
			.height = want,
		};
	}
	return (struct cow_box) {
		.x	= strip_x + start,
		.y	= strip_y,
		.width	= want,
		.height = strip_h,
	};
}

bool
cow_titlebar_hit(const struct cow_view *view, int lx, int ly)
{
	return cow_titlebar_hit_for(view, view->content_width,
	    view->content_height, lx, ly);
}

int
cow_titlebar_button_at(const struct cow_view *view, int lx, int ly)
{
	static const int left_order[] = { 1, 3, 5, 7, 9 };
	static const int right_order[] = { 0, 2, 4, 6, 8 };
	int tt = view->dec.title_thickness;

	if (!cow_titlebar_hit(view, lx, ly))
		return (-1);
	struct cow_box tbr = cow_titlebar_squeezed_rect(view);
	int frame_w, frame_h, content_x, content_y;
	cow_titlebar_frame_geometry(view, view->content_width,
	    view->content_height, &frame_w, &frame_h, &content_x, &content_y);
	int pos_coord = cow_titlebar_is_vertical(view->cfg.titlebar.position) ?
	    ly + content_y : lx + content_x;
	int l = cow_titlebar_is_vertical(view->cfg.titlebar.position) ? tbr.y :
							     tbr.x;
	int r = l + (cow_titlebar_is_vertical(view->cfg.titlebar.position) ?
		tbr.height : tbr.width);
	if (view->cfg.titlebar.position == COW_TITLEBAR_LEFT)
		pos_coord = l + (r - 1 - pos_coord);

	int pos = 0;
	for (size_t i = 0; i < nitems(left_order); i++) {
		int idx = left_order[i];
		if (!titlebar_button_visible(&view->cfg.titlebar, idx))
			continue;
		int x = l + pos++ * tt;
		if (pos_coord >= x && pos_coord < x + tt)
			return (idx);
	}
	pos = 0;
	for (size_t i = 0; i < nitems(right_order); i++) {
		int idx = right_order[i];
		if (!titlebar_button_visible(&view->cfg.titlebar, idx))
			continue;
		int x = r - (++pos * tt);
		if (pos_coord >= x && pos_coord < x + tt)
			return (idx);
	}
	return (-1);
}

bool
cow_titlebar_hit_for(const struct cow_view *view, int cw, int ch, int lx,
    int ly)
{
	int tt = view->dec.title_thickness;

	if (tt <= 0)
		return false;

	struct cow_box r = cow_titlebar_squeezed_rect_for(view, cw, ch);
	if (r.width <= 0 || r.height <= 0)
		return false;

	int frame_w, frame_h, content_x, content_y;
	cow_titlebar_frame_geometry(view, cw, ch, &frame_w, &frame_h,
	    &content_x, &content_y);

	int dx = lx + content_x;
	int dy = ly + content_y;
	return (dx >= r.x && dx < r.x + r.width &&
	    dy >= r.y && dy < r.y + r.height);
}

struct titlebar_relief_edges {
	uint32_t top;
	uint32_t left;
	uint32_t bottom;
	uint32_t right;
};

static struct titlebar_relief_edges
titlebar_relief_edges(enum cow_titlebar_position position, uint32_t ul,
    uint32_t br)
{
	/* Vertical titlebars are rendered horizontally and rotated later. */
	switch (position) {
	case COW_TITLEBAR_LEFT:
		return (struct titlebar_relief_edges) { ul, br, br, ul };
	case COW_TITLEBAR_RIGHT:
		return (struct titlebar_relief_edges) { br, ul, ul, br };
	default:
		return (struct titlebar_relief_edges) { ul, ul, br, br };
	}
}

/*
 * Draw 2-ring concentric FVWM relief matching the original titlebar.c
 * draw_fvwm_relief (2px raised bevel = hiout + shin layers).
 */
static void
draw_fvwm_relief(uint32_t *pixels, int stride, int rx, int ry, int rw, int rh,
    uint32_t px_lt, uint32_t px_dk, bool sunken,
    enum cow_titlebar_position position)
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
	struct titlebar_relief_edges edges =
	    titlebar_relief_edges(position, ul, br);

	for (int r = 0; r < 2 && r < rw / 2 && r < rh / 2; r++) {
		int y1 = ry + r + 1;

		if (r > 0) {
			cow_draw_hline(&p, rx + r, rx + rw - 1 - r, ry + r,
			    edges.top);
		}
		cow_draw_vline(&p, rx + r, y1, ry + rh - 1 - r,
		    edges.left);
		cow_draw_hline(&p, rx + r, rx + rw - 1 - r,
		    ry + rh - 1 - r, edges.bottom);
		cow_draw_vline(&p, rx + rw - 1 - r, y1, ry + rh - 1 - r,
		    edges.right);
	}
}

static void
draw_titlebar_relief(uint32_t *pixels, int stride, int rx, int ry, int rw,
    int rh, uint32_t px_lt, uint32_t px_dk, bool sunken,
    enum cow_border_style style, enum cow_titlebar_position position)
{
	if (style == COW_BORDER_STYLE_NONE)
		return;
	if (style == COW_BORDER_STYLE_MWM) {
		struct cow_pixbuf p = {
			.pixels	   = pixels,
			.stride_px = stride,
			.width	   = INT_MAX,
			.height	   = INT_MAX,
		};
		uint32_t ul = sunken ? px_dk : px_lt;
		uint32_t br = sunken ? px_lt : px_dk;
		struct titlebar_relief_edges edges =
		    titlebar_relief_edges(position, ul, br);
		cow_draw_hline(&p, rx, rx + rw - 1, ry, edges.top);
		cow_draw_vline(&p, rx, ry + 1, ry + rh - 1, edges.left);
		cow_draw_hline(&p, rx, rx + rw - 2, ry + rh - 1,
		    edges.bottom);
		cow_draw_vline(&p, rx + rw - 1, ry + 1, ry + rh - 1,
		    edges.right);
		return;
	}
	draw_fvwm_relief(pixels, stride, rx, ry, rw, rh, px_lt, px_dk,
	    sunken, position);
}

static void
draw_titlebar_button(uint32_t *pixels, int stride_px,
    struct cow_pixbuf *strip, const struct cow_titlebar_config *tb_cfg,
    int btn_idx, int bx, int by, int btn_s, bool active, int pressed_button,
    int icon_inverted_button, const struct cow_gradient *tb_gradient,
    const char *tb_image, uint32_t px_base, uint32_t px_lt, uint32_t px_dk,
    uint32_t px_fg, enum cow_border_style border_style,
    enum cow_titlebar_position position)
{
	const struct cow_titlebar_button_config *button =
	    &tb_cfg->buttons[btn_idx];
	const float *custom_colour = active ? button->active_colour :
	    button->inactive_colour;
	bool has_custom_colour = active ? button->has_active_colour :
	    button->has_inactive_colour;
	const float *custom_fg = active ? button->active_fg :
	    button->inactive_fg;
	bool has_custom_fg = active ? button->has_active_fg :
	    button->has_inactive_fg;
	int relief = active ? button->active_relief : button->inactive_relief;
	uint32_t button_base = px_base;
	uint32_t colours[5] = {
		[COW_VECTOR_SHADOW] = px_dk,
		[COW_VECTOR_HIGHLIGHT] = px_lt,
		[COW_VECTOR_BG] = px_base,
		[COW_VECTOR_FG] = px_fg,
		[COW_VECTOR_INVISIBLE] = 0,
	};

	if ((tb_gradient == NULL ||
	    tb_gradient->mode == COW_GRADIENT_NONE) && tb_image == NULL)
		cow_draw_fill_rect(strip, bx, by, btn_s, btn_s, px_base);

	if (has_custom_colour) {
		float custom_lt[4], custom_dk[4];

		cow_colour_lighten(custom_lt, custom_colour);
		cow_colour_darken(custom_dk, custom_colour);
		button_base = cow_colour_to_pixel(custom_colour);
		colours[COW_VECTOR_SHADOW] = cow_colour_to_pixel(custom_dk);
		colours[COW_VECTOR_HIGHLIGHT] = cow_colour_to_pixel(custom_lt);
		colours[COW_VECTOR_BG] = button_base;
		cow_draw_fill_rect(strip, bx, by, btn_s, btn_s, button_base);
	}
	if (has_custom_fg)
		colours[COW_VECTOR_FG] = cow_colour_to_pixel(custom_fg);

	bool sunken = pressed_button == btn_idx;
	if (relief == COW_RELIEF_SUNKEN)
		sunken = !sunken;
	if (sunken || relief != COW_RELIEF_FLAT) {
		draw_titlebar_relief(pixels, stride_px, bx, by, btn_s, btn_s,
		    colours[COW_VECTOR_HIGHLIGHT], colours[COW_VECTOR_SHADOW],
		    sunken, border_style, position);
	}

	if (button->image != NULL) {
		cow_image_render_rect(button->image, strip, bx + 2, by + 2,
		    btn_s - 4, btn_s - 4);
		return;
	}

	uint32_t btn_buf[btn_s * btn_s];
	for (int p = 0; p < btn_s * btn_s; p++)
		btn_buf[p] = button_base;

	bool icon_inverted = icon_inverted_button == btn_idx || sunken;
	cow_vector_button_draw(&button->vector, btn_buf, btn_s, btn_s, btn_s,
	    btn_s, colours, icon_inverted);
	for (int py = 1; py < btn_s - 1; py++) {
		for (int px = 1; px < btn_s - 1; px++) {
			uint32_t colour = btn_buf[py * btn_s + px];
			if (colour != button_base)
				pixels[(by + py) * stride_px + bx + px] = colour;
		}
	}
}

static PangoAlignment
title_alignment(enum cow_title_justify justify)
{
	switch (justify) {
	case COW_TITLE_JUST_LEFT:
		return PANGO_ALIGN_LEFT;
	case COW_TITLE_JUST_RIGHT:
		return PANGO_ALIGN_RIGHT;
	case COW_TITLE_JUST_CENTRE:
	default:
		return PANGO_ALIGN_CENTER;
	}
}

static void
cow_titlebar_render_horizontal(const struct cow_titlebar_render_ctx *ctx)
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
	enum cow_titlebar_position position = ctx->tb_cfg->position;
	PangoAlignment align = title_alignment(active ?
	    tb_cfg->active_title_justify : tb_cfg->inactive_title_justify);
	enum cow_border_style border_style =
	    tb_cfg->button_style == COW_TITLEBAR_BUTTON_STYLE_INHERIT ?
	    ctx->border_style : (enum cow_border_style)tb_cfg->button_style;

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
	int strip_y = tb_y;
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
	const char *tb_image = NULL;
	if (style_colour == NULL && tb_cfg != NULL)
		tb_image = active ? tb_cfg->active_image :
				    tb_cfg->inactive_image;
	if (tb_gradient != NULL && tb_gradient->mode != COW_GRADIENT_NONE)
		cow_draw_gradient_rect(&strip_p, tb_x, tb_y, tb_w, tb_h,
		    tb_gradient);
	cow_image_render_rect(tb_image, &strip_p, tb_x, tb_y, tb_w, tb_h);

	static const int left_order[] = { 1, 3, 5, 7, 9 };
	static const int right_order[] = { 0, 2, 4, 6, 8 };
	int btn_s = tb_h;
	int left_btns = 0, right_btns = 0;
	for (size_t i = 0; i < sizeof left_order / sizeof left_order[0]; i++) {
		if (titlebar_button_visible(tb_cfg, left_order[i]))
			left_btns++;
	}
	for (size_t i = 0; i < sizeof right_order / sizeof right_order[0]; i++) {
		if (titlebar_button_visible(tb_cfg, right_order[i]))
			right_btns++;
	}

	int pos = 0;
	for (size_t i = 0; i < sizeof left_order / sizeof left_order[0]; i++) {
		int btn_idx = left_order[i];
		if (!titlebar_button_visible(tb_cfg, btn_idx))
			continue;
		int bx = tb_x + pos * btn_s;
		int by = tb_y;
		if (bx + btn_s > tb_x + tb_w)
			break;
		pos++;

		draw_titlebar_button(pixels, stride_px, &strip_p, tb_cfg,
		    btn_idx, bx, by, btn_s, active, pressed_button,
		    icon_inverted_button, tb_gradient, tb_image, px_base, px_lt,
		    px_dk, px_fg, border_style, position);
	}

	pos = 0;
	for (size_t i = 0; i < sizeof right_order / sizeof right_order[0]; i++) {
		int btn_idx = right_order[i];
		if (!titlebar_button_visible(tb_cfg, btn_idx))
			continue;
		int from_right = pos++;
		int bx	       = tb_x + tb_w - (from_right + 1) * btn_s;
		int by	       = tb_y;
		if (bx < tb_x)
			break;

		draw_titlebar_button(pixels, stride_px, &strip_p, tb_cfg,
		    btn_idx, bx, by, btn_s, active, pressed_button,
		    icon_inverted_button, tb_gradient, tb_image, px_base, px_lt,
		    px_dk, px_fg, border_style, position);
	}

	/* Title text area: between left and right buttons */
	int title_x	    = tb_x + left_btns * btn_s;
	int title_w	    = tb_w - (left_btns + right_btns) * btn_s;
	int rendered_text_w = 0; /* actual rendered text width */

	/* Render title text with cairo/pango */
	if (title != NULL && title_w > 4 && font != NULL) {
		const float *label_bg = ((tb_gradient != NULL &&
		    tb_gradient->mode != COW_GRADIENT_NONE) ||
		    tb_image != NULL) ?
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
		    align, &rendered_text_w);
	}

	/* Draw stipple for sticky windows in the empty areas
	 * either side of the title text. */
	if (sticky && title_w > 4) {
		int relief = 2;
		int pad	   = COW_TITLEBAR_PADDING;

		/* Centre the text gap within the title area */
		int text_centre = title_x + title_w / 2;
		int text_start;
		int text_end;

		switch (align) {
		case PANGO_ALIGN_CENTER:
			text_start = text_centre - rendered_text_w / 2 - pad;
			text_end = text_centre + (rendered_text_w + 1) / 2 + pad;
			break;
		case PANGO_ALIGN_LEFT:
			text_start = title_x + pad;
			text_end = title_x + rendered_text_w + 2 * pad;
			break;
		case PANGO_ALIGN_RIGHT:
			text_start =
			    title_x + title_w - 2 * pad - rendered_text_w;
			text_end = title_x + title_w - 2 * pad;
			break;
		}

		int sx1 = title_x + relief;
		int sx2 = title_x + title_w - relief;

		for (int y = tb_y + relief; y < tb_y + tb_h - relief; y++) {
			/* Left stipple area */
			if (align == PANGO_ALIGN_RIGHT ||
			    align == PANGO_ALIGN_CENTER) {
				for (int x = sx1; x < text_start && x < sx2; x++) {
					if ((x + y) % 2 == 0)
						pixels[y * stride_px + x] = px_lt;
					else
						pixels[y * stride_px + x] = px_dk;
				}
			}
			/* Right stipple area */
			if (align == PANGO_ALIGN_CENTER || align == PANGO_ALIGN_LEFT) {
					for (int x = text_end > sx1 ? text_end : sx1; x < sx2;
					x++) {
					if ((x + y) % 2 == 0)
						pixels[y * stride_px + x] = px_lt;
					else
						pixels[y * stride_px + x] = px_dk;
				}
			}
		}
	}

	/*
	 * Container tab strip.  When n_tabs > 1, divide the title area into
	 * equal-width segments.  Active/inactive tab colours come from the
	 * cc parameter (the view's cow_view_cfg.container snapshot) so
	 * users can style them per-view via decor profiles.  Falls
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
			const char *tab_image = NULL;
			if (active && cc != NULL)
				tab_image = is_active ? cc->tab_active_image :
				    cc->tab_inactive_image;
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
			cow_image_render_rect(tab_image, &strip_p, tx, tb_y, tw,
			    tb_h);

			/* Tab bevel — active is always raised; inactive is
			 * sunken only when container.tab.sunken is true
			 * (default: flat). */
			bool sunken = !is_active && cc != NULL &&
			    cc->tab_sunken_inactive;
			draw_titlebar_relief(pixels, stride_px, tx, tb_y, tw,
			    tb_h, tab_bv_lt, tab_bv_dk, sunken,
			    border_style, position);

			/* Tab label text */
			const char *lbl = tab_titles[ti];
			if (lbl != NULL && tw > 6 && font != NULL) {
				const float *label_bg = ((tab_gradient != NULL &&
				    tab_gradient->mode != COW_GRADIENT_NONE) ||
				    tab_image != NULL) ?
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
				    COW_TITLEBAR_PADDING, PANGO_ALIGN_CENTER, NULL);
			}
		}
	} else {
		/* Draw relief on title text area LAST so it isn't overwritten.
		 * Sunken when title is being dragged, raised otherwise. */
		if (title_w > 2) {
			if ((active && tb_cfg->active_relief == COW_RELIEF_SUNKEN) || (!active && tb_cfg->inactive_relief == COW_RELIEF_SUNKEN)) {
				title_pressed = !title_pressed; // "Sunken" profile reverses depth
			}
			if (title_pressed || (active && tb_cfg->active_relief != COW_RELIEF_FLAT) || (!active && tb_cfg->inactive_relief != COW_RELIEF_FLAT)) {
			// If the mode is non-flat
				draw_titlebar_relief(pixels, stride_px, title_x, tb_y,
					title_w, tb_h, px_lt, px_dk, title_pressed,
					border_style, position);
			}
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

static void
cow_titlebar_rotate_into(const struct cow_titlebar_render_ctx *ctx,
    const uint32_t *tmp, int tmp_w, int tmp_h)
{
	uint32_t *dst = ctx->pixels;
	int	 stride = ctx->stride_px;
	struct cow_box r = ctx->tb_rect;
	bool left = ctx->tb_cfg != NULL &&
	    ctx->tb_cfg->position == COW_TITLEBAR_LEFT;

	for (int sy = 0; sy < tmp_h; sy++) {
		for (int sx = 0; sx < tmp_w; sx++) {
			int dx, dy;

			if (left) {
				dx = r.x + sy;
				dy = r.y + (tmp_w - 1 - sx);
			} else {
				dx = r.x + (tmp_h - 1 - sy);
				dy = r.y + sx;
			}
			dst[dy * stride + dx] = tmp[sy * tmp_w + sx];
		}
	}
}

void
cow_titlebar_render(const struct cow_titlebar_render_ctx *ctx)
{
	if (ctx->tb_cfg == NULL ||
	    !cow_titlebar_is_vertical(ctx->tb_cfg->position)) {
		cow_titlebar_render_horizontal(ctx);
		return;
	}

	struct cow_box r = ctx->tb_rect;
	int	       tmp_w = r.height;
	int	       tmp_h = r.width;
	if (tmp_w <= 0 || tmp_h <= 0)
		return;

	uint32_t *tmp = xcalloc((size_t)tmp_w * (size_t)tmp_h,
	    sizeof(uint32_t));
	struct cow_titlebar_render_ctx hctx = *ctx;
	hctx.pixels = tmp;
	hctx.stride_px = tmp_w;
	hctx.frame_w = tmp_w;
	hctx.frame_h = tmp_h;
	hctx.border_width = 0;
	hctx.tb_rect = (struct cow_box) {
		.x = 0,
		.y = 0,
		.width = tmp_w,
		.height = tmp_h,
	};
	cow_titlebar_render_horizontal(&hctx);
	struct cow_pixbuf dst = {
		.pixels	   = ctx->pixels,
		.stride_px = ctx->stride_px,
		.width	   = INT_MAX,
		.height	   = INT_MAX,
	};
	cow_draw_clear_rect(&dst, r.x, ctx->border_width, r.width,
	    ctx->frame_h - 2 * ctx->border_width);
	cow_titlebar_rotate_into(ctx, tmp, tmp_w, tmp_h);
	xfree(tmp);
}
