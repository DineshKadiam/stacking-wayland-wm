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

#include <string.h>

#include <cairo.h>
#include <pango/pangocairo.h>

#include "cow/colour.h"
#include "cow/compat.h"
#include "cow/config.h"
#include "cow/font.h"
#include "cow/vector.h"

int
cow_titlebar_compute_thickness(struct cow_font *font)
{
	int raw_height = font->height - 8;
	if (raw_height < 12)
		raw_height = 12;
	return raw_height + COW_TITLEBAR_PADDING * 2;
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
	uint32_t ul = sunken ? px_dk : px_lt;
	uint32_t br = sunken ? px_lt : px_dk;

	for (int r = 0; r < 2 && r < rw / 2 && r < rh / 2; r++) {
		/* Top edge */
		for (int x = rx + r; x < rx + rw - r; x++)
			pixels[(ry + r) * stride + x] = ul;
		/* Left edge */
		for (int y = ry + r; y < ry + rh - r; y++)
			pixels[y * stride + (rx + r)] = ul;
		/* Bottom edge */
		for (int x = rx + r; x < rx + rw - r; x++)
			pixels[(ry + rh - 1 - r) * stride + x] = br;
		/* Right edge */
		for (int y = ry + r; y < ry + rh - r; y++)
			pixels[y * stride + (rx + rw - 1 - r)] = br;
	}
}

void
cow_titlebar_render(uint32_t *pixels, int stride_px, int frame_w,
    int border_width, int title_thickness, const char *title, bool active,
    struct cow_font *font, int pressed_button, int icon_inverted_button,
    bool title_pressed, const float *style_colour, const float *style_fg,
    bool sticky)
{
	if (title_thickness <= 0 || frame_w <= 2 * border_width)
		return;

	/* Titlebar colors: use style override if provided,
	 * otherwise fall back to config */
	float				  base[4], fg[4];
	const struct cow_titlebar_config *tb_cfg = cow_config ?
	    &cow_config->titlebar :
	    NULL;
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

	/* Titlebar geometry within the frame */
	int tb_x = border_width;
	int tb_y = border_width;
	int tb_w = frame_w - 2 * border_width;
	int tb_h = title_thickness;

	if (tb_w <= 0)
		return;

	/* Fill titlebar base colour */
	for (int y = tb_y; y < tb_y + tb_h; y++) {
		for (int x = tb_x; x < tb_x + tb_w; x++) {
			pixels[y * stride_px + x] = px_base;
		}
	}

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

		/* Fill button background */
		for (int py = by; py < by + btn_s; py++)
			for (int px = bx; px < bx + btn_s; px++)
				pixels[py * stride_px + px] = px_base;

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

		/* Fill button background */
		for (int py = by; py < by + btn_s; py++)
			for (int px = bx; px < bx + btn_s; px++)
				pixels[py * stride_px + px] = px_base;

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
		int text_inset = 2 + COW_TITLEBAR_PADDING;

		cairo_surface_t *surface = cairo_image_surface_create(
		    CAIRO_FORMAT_ARGB32, title_w, tb_h);
		cairo_t *cr = cairo_create(surface);

		/* Fill with base colour */
		cairo_set_source_rgba(cr, base[0], base[1], base[2], base[3]);
		cairo_paint(cr);

		/* Render text */
		PangoLayout *layout = pango_cairo_create_layout(cr);
		pango_layout_set_font_description(layout, font->desc);
		pango_layout_set_text(layout, title, -1);

		int avail_w = title_w - 2 * text_inset;
		if (avail_w > 0)
			pango_layout_set_width(layout, avail_w * PANGO_SCALE);
		pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
		pango_layout_set_alignment(layout, PANGO_ALIGN_CENTER);

		int text_w, text_h;
		pango_layout_get_pixel_size(layout, &text_w, &text_h);
		rendered_text_w = text_w;
		int text_y	= (tb_h - text_h) / 2;
		if (text_y < 0)
			text_y = 0;

		cairo_move_to(cr, text_inset, text_y);
		cairo_set_source_rgba(cr, fg[0], fg[1], fg[2], fg[3]);
		pango_cairo_show_layout(cr, layout);

		g_object_unref(layout);
		cairo_surface_flush(surface);

		/* Copy rendered text into the frame buffer, preserving 2px
		 * relief */
		unsigned char *sdata = cairo_image_surface_get_data(surface);
		int sstride = cairo_image_surface_get_stride(surface) / 4;
		int relief  = 2;

		for (int py = relief; py < tb_h - relief; py++) {
			for (int px = relief; px < title_w - relief; px++) {
				uint32_t c = ((
				    uint32_t *)sdata)[py * sstride + px];
				if ((c & 0xFF000000) != 0) {
					pixels[(tb_y + py) * stride_px +
					    (title_x + px)] = c;
				}
			}
		}

		cairo_destroy(cr);
		cairo_surface_destroy(surface);
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

	/* Draw relief on title text area LAST so it isn't overwritten.
	 * Sunken when title is being dragged, raised otherwise. */
	if (title_w > 2) {
		draw_fvwm_relief(pixels, stride_px, title_x, tb_y, title_w,
		    tb_h, px_lt, px_dk, title_pressed);
	}
}
