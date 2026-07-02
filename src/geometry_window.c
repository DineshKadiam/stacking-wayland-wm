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
#include "cow/geometry_window.h"

#include <cairo/cairo.h>
#include <pango/pangocairo.h>
#include <stdio.h>
#include <string.h>

#include "cow/colour.h"
#include "cow/compat.h"
#include "cow/config.h"
#include "cow/draw.h"
#include "cow/output.h"
#include "cow/shm.h"
#include "cow/wm.h"

#include "river-window-management-v1-client-protocol.h"

#define GEOM_CONTENT_WIDTH 220
#define GEOM_CONTENT_HEIGHT 28
#define GEOM_PAD_X 10
#define GEOM_RELIEF_WIDTH 2

static bool geometry_window_create(void);
static void geometry_window_draw(void);
static void geometry_window_set_text(struct cow_output *, const char *);

void
cow_geometry_window_update_move(struct cow_output *output, int x, int y)
{
	char text[sizeof cow_wm.geometry_text];
	snprintf(text, sizeof text, "%+d%+d", x, y);
	geometry_window_set_text(output, text);
}

void
cow_geometry_window_update_resize(struct cow_output *output, int w, int h)
{
	char text[sizeof cow_wm.geometry_text];
	snprintf(text, sizeof text, "%dx%d", w, h);
	geometry_window_set_text(output, text);
}

static void
geometry_window_set_text(struct cow_output *output, const char *text)
{
	if (output == NULL)
		return;

	if (strcmp(cow_wm.geometry_text, text) != 0) {
		strlcpy(cow_wm.geometry_text, text, sizeof cow_wm.geometry_text);
		cow_wm.geometry_dirty = true;
	}

	if (geometry_window_create()) {
		cow_wm.geometry_x = output->x +
		    (output->width - GEOM_CONTENT_WIDTH) / 2;
		cow_wm.geometry_y = output->y +
		    (output->height - GEOM_CONTENT_HEIGHT) / 2;
	}
}

void
cow_geometry_window_hide(void)
{
	if (cow_wm.geometry_node != NULL) {
		river_node_v1_destroy(cow_wm.geometry_node);
		cow_wm.geometry_node = NULL;
	}
	if (cow_wm.geometry_shell != NULL) {
		river_shell_surface_v1_destroy(cow_wm.geometry_shell);
		cow_wm.geometry_shell = NULL;
	}
	if (cow_wm.geometry_surface != NULL) {
		wl_surface_destroy(cow_wm.geometry_surface);
		cow_wm.geometry_surface = NULL;
	}
	cow_shm_buffer_destroy(cow_wm.geometry_buffer);
	cow_wm.geometry_buffer = NULL;
	cow_wm.geometry_text[0] = '\0';
	cow_wm.geometry_dirty = false;
}

void
cow_geometry_window_render(void)
{
	if (cow_wm.geometry_node == NULL)
		return;

	river_node_v1_set_position(cow_wm.geometry_node, cow_wm.geometry_x,
	    cow_wm.geometry_y);
	river_node_v1_place_top(cow_wm.geometry_node);

	if (!cow_wm.geometry_dirty)
		return;

	if (cow_wm.geometry_buffer == NULL || cow_wm.geometry_buffer->busy)
		return;

	geometry_window_draw();
	cow_wm.geometry_dirty = false;

	wl_surface_attach(cow_wm.geometry_surface,
	    cow_wm.geometry_buffer->buffer, 0, 0);
	wl_surface_damage_buffer(cow_wm.geometry_surface, 0, 0,
	    GEOM_CONTENT_WIDTH, GEOM_CONTENT_HEIGHT);
	river_shell_surface_v1_sync_next_commit(cow_wm.geometry_shell);
	wl_surface_commit(cow_wm.geometry_surface);
}

static bool
geometry_window_create(void)
{
	if (cow_wm.geometry_surface != NULL)
		return true;
	if (cow_wm.compositor == NULL || cow_wm.manager == NULL)
		return false;

	struct wl_surface *surf =
	    wl_compositor_create_surface(cow_wm.compositor);
	if (surf == NULL)
		return false;

	struct river_shell_surface_v1 *shell =
	    river_window_manager_v1_get_shell_surface(cow_wm.manager, surf);
	if (shell == NULL) {
		wl_surface_destroy(surf);
		return false;
	}

	struct cow_shm_buffer *buf =
	    cow_shm_buffer_create(GEOM_CONTENT_WIDTH, GEOM_CONTENT_HEIGHT);
	if (buf == NULL) {
		river_shell_surface_v1_destroy(shell);
		wl_surface_destroy(surf);
		return false;
	}

	cow_wm.geometry_surface = surf;
	cow_wm.geometry_shell = shell;
	cow_wm.geometry_node = river_shell_surface_v1_get_node(shell);
	cow_wm.geometry_buffer = buf;
	cow_wm.geometry_dirty = true;
	return true;
}

static void
geometry_window_draw(void)
{
	const char *font = cow_config != NULL && cow_config->font_name != NULL ?
	    cow_config->font_name :
	    "monospace 10";
	cairo_surface_t *measure =
	    cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
	cairo_t *mcr = cairo_create(measure);
	PangoLayout *layout = pango_cairo_create_layout(mcr);
	PangoFontDescription *desc = pango_font_description_from_string(font);

	pango_layout_set_font_description(layout, desc);
	pango_layout_set_text(layout, cow_wm.geometry_text, -1);
	pango_layout_set_width(layout, (GEOM_CONTENT_WIDTH - GEOM_PAD_X * 2) *
	    PANGO_SCALE);
	pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
	pango_layout_set_alignment(layout, PANGO_ALIGN_CENTER);

	int text_w = 0, text_h = 0;
	pango_layout_get_pixel_size(layout, &text_w, &text_h);

	cairo_surface_t *cs = cairo_image_surface_create_for_data(
	    (unsigned char *)cow_wm.geometry_buffer->data, CAIRO_FORMAT_ARGB32,
	    GEOM_CONTENT_WIDTH, GEOM_CONTENT_HEIGHT,
	    cow_wm.geometry_buffer->stride);
	cairo_t *cr = cairo_create(cs);

	cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
	cairo_paint(cr);
	cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
	cairo_destroy(cr);

	struct cow_pixbuf p = {
		.pixels	   = cow_wm.geometry_buffer->data,
		.stride_px = cow_wm.geometry_buffer->stride / 4,
		.width	   = GEOM_CONTENT_WIDTH,
		.height	   = GEOM_CONTENT_HEIGHT,
	};
	uint32_t bg = cow_config != NULL ?
	    cow_colour_to_pixel(cow_config->titlebar.inactive_colour) :
	    0xffa0a0a0;
	cow_draw_fill_rect(&p, 0, 0, GEOM_CONTENT_WIDTH, GEOM_CONTENT_HEIGHT,
	    bg);

	float lt_f[4], dk_f[4];
	if (cow_config != NULL) {
		cow_colour_lighten(lt_f, cow_config->titlebar.inactive_colour);
		cow_colour_darken(dk_f, cow_config->titlebar.inactive_colour);
	} else {
		float fallback[4];
		cow_colour_convert(fallback, 0xa0a0a0);
		cow_colour_lighten(lt_f, fallback);
		cow_colour_darken(dk_f, fallback);
	}
	cow_draw_relieve_rect(&p, 0, 0, GEOM_CONTENT_WIDTH - 1,
	    GEOM_CONTENT_HEIGHT - 1, GEOM_RELIEF_WIDTH,
	    cow_colour_to_pixel(lt_f), cow_colour_to_pixel(lt_f),
	    cow_colour_to_pixel(dk_f), cow_colour_to_pixel(dk_f));

	cr = cairo_create(cs);

	pango_cairo_update_layout(cr, layout);
	cairo_move_to(cr, GEOM_PAD_X, (GEOM_CONTENT_HEIGHT - text_h) / 2);
	if (cow_config != NULL) {
		const float *fg = cow_config->titlebar.fg_inactive;
		cairo_set_source_rgba(cr, fg[0], fg[1], fg[2], fg[3]);
	} else
		cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 1.0);
	pango_cairo_show_layout(cr, layout);

	cairo_destroy(cr);
	cairo_surface_destroy(cs);

	g_object_unref(layout);
	pango_font_description_free(desc);
	cairo_destroy(mcr);
	cairo_surface_destroy(measure);
}
