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
#include "cow/pointer_input.h"

#include <string.h>

#include <wayland-client.h>

#include "cow/compat.h"
#include "cow/decoration.h"
#include "cow/input-event-codes.h"
#include "cow/keybinding.h"
#include "cow/menu.h"
#include "cow/output.h"
#include "cow/pan_frames.h"
#include "cow/pick.h"
#include "cow/pointer_op.h"
#include "cow/seat.h"
#include "cow/titlebar.h"
#include "cow/view.h"
#include "cow/wm.h"

#include "cursor-shape-v1-client-protocol.h"

static struct wl_surface *decoration_cursor_surface;
static struct cow_view   *decoration_cursor_view;
static uint32_t		  decoration_cursor_serial;
static int		  decoration_cursor_x;
static int		  decoration_cursor_y;
static struct cow_output *pan_cursor_output;
static int		  pan_cursor_edge = -1;
static bool		  cursor_held;
static struct cow_output *pointer_focus_background;

static void
decoration_context_leave(void)
{
	struct cow_seat *seat = cow_seat_primary();
	if (seat != NULL && decoration_cursor_view != NULL &&
	    seat->pointer_entered_view == decoration_cursor_view)
		cow_keybinding_set_context(seat, COW_BIND_CONTEXT_WINDOW,
		    decoration_cursor_view);
	decoration_cursor_view = NULL;
}

static struct cow_view *
view_for_decoration_surface(struct wl_surface *surface)
{
	struct cow_view *view;

	wl_list_for_each(view, &cow_wm.views, wm_link)
	{
		if (!view->closed && view->decoration_surface == surface)
			return (view);
	}
	return (NULL);
}

static uint32_t
cursor_shape_for_edges(uint32_t edges)
{
	if (edges == (COW_EDGE_TOP | COW_EDGE_LEFT))
		return (WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NW_RESIZE);
	if (edges == (COW_EDGE_TOP | COW_EDGE_RIGHT))
		return (WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NE_RESIZE);
	if (edges == (COW_EDGE_BOTTOM | COW_EDGE_LEFT))
		return (WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_SW_RESIZE);
	if (edges == (COW_EDGE_BOTTOM | COW_EDGE_RIGHT))
		return (WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_SE_RESIZE);
	if (edges == COW_EDGE_TOP)
		return (WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_N_RESIZE);
	if (edges == COW_EDGE_BOTTOM)
		return (WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_S_RESIZE);
	if (edges == COW_EDGE_LEFT)
		return (WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_W_RESIZE);
	if (edges == COW_EDGE_RIGHT)
		return (WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_E_RESIZE);
	return (WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT);
}

static uint32_t
decoration_cursor_shape_at(struct cow_view *view, int x, int y, int cw, int ch)
{
	uint32_t edges = cow_decoration_edges_at(view, x, y, cw, ch);
	if (edges != 0)
		return (cursor_shape_for_edges(edges));

	int content_x, content_y, frame_w, frame_h;
	cow_titlebar_frame_geometry(view, cw, ch, &frame_w, &frame_h,
	    &content_x, &content_y);
	if (cow_titlebar_hit_for(view, cw, ch, x - content_x, y - content_y))
		return (WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_GRAB);

	return (WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT);
}

static uint32_t
decoration_binding_context_at(struct cow_view *view, int x, int y, int cw,
    int ch)
{
	uint32_t edges = cow_decoration_edges_at(view, x, y, cw, ch);
	if (edges == (COW_EDGE_TOP | COW_EDGE_LEFT))
		return (COW_BIND_CONTEXT_NW);
	if (edges == (COW_EDGE_TOP | COW_EDGE_RIGHT))
		return (COW_BIND_CONTEXT_NE);
	if (edges == (COW_EDGE_BOTTOM | COW_EDGE_RIGHT))
		return (COW_BIND_CONTEXT_SE);
	if (edges == (COW_EDGE_BOTTOM | COW_EDGE_LEFT))
		return (COW_BIND_CONTEXT_SW);
	if (edges == COW_EDGE_TOP)
		return (COW_BIND_CONTEXT_TOP);
	if (edges == COW_EDGE_BOTTOM)
		return (COW_BIND_CONTEXT_BOTTOM);
	if (edges == COW_EDGE_LEFT)
		return (COW_BIND_CONTEXT_LEFT);
	if (edges == COW_EDGE_RIGHT)
		return (COW_BIND_CONTEXT_RIGHT);

	int content_x, content_y, frame_w, frame_h;
	cow_titlebar_frame_geometry(view, cw, ch, &frame_w, &frame_h,
	    &content_x, &content_y);
	int lx = x - content_x;
	int ly = y - content_y;
	if (cow_titlebar_hit_for(view, cw, ch, lx, ly)) {
		int button = cow_titlebar_button_at(view, lx, ly);
		return (button >= 0 ? COW_BIND_CONTEXT_BUTTON(button) :
				      COW_BIND_CONTEXT_TITLE);
	}
	return (COW_BIND_CONTEXT_WINDOW);
}

static int
decoration_button_at(struct cow_view *view, int x, int y, int cw, int ch)
{
	int content_x, content_y, frame_w, frame_h;
	cow_titlebar_frame_geometry(view, cw, ch, &frame_w, &frame_h,
	    &content_x, &content_y);
	return (cow_titlebar_button_at(view, x - content_x, y - content_y));
}

static bool
decoration_cursor_update(struct wl_surface *surface, uint32_t serial,
    wl_fixed_t sx, wl_fixed_t sy)
{
	struct cow_view *view = view_for_decoration_surface(surface);
	if (view == NULL)
		return (false);

	if (serial != 0)
		decoration_cursor_serial = serial;

	int x  = wl_fixed_to_int(sx);
	int y  = wl_fixed_to_int(sy);
	int cw, ch;
	cow_view_effective_content_size(view, &cw, &ch);
	cow_keybinding_set_context(cow_seat_primary(),
	    decoration_binding_context_at(view, x, y, cw, ch), view);
	decoration_cursor_view = view;
	decoration_cursor_x = x;
	decoration_cursor_y = y;

	cow_wm_set_cursor_shape(decoration_cursor_shape_at(view, x, y, cw, ch),
	    decoration_cursor_serial);
	return (true);
}

static struct cow_view *
view_for_global_decoration_point(int gx, int gy, int *dx, int *dy, int *cw,
    int *ch)
{
	struct cow_view *view, *best = NULL;
	uint64_t	 best_raise = 0;

	wl_list_for_each(view, &cow_wm.views, wm_link)
	{
		if (view->closed || !view->use_ssd || view->output == NULL)
			continue;
		if (!cow_view_visible(view, view->output))
			continue;

		int vcw, vch;
		cow_view_effective_content_size(view, &vcw, &vch);

		int content_x, content_y, frame_w, frame_h;
		cow_titlebar_frame_geometry(view, vcw, vch, &frame_w,
		    &frame_h, &content_x, &content_y);
		int fx = view->x - content_x;
		int fy = view->y - content_y;
		if (gx < fx || gx >= fx + frame_w || gy < fy ||
		    gy >= fy + frame_h)
			continue;

		uint32_t shape = decoration_cursor_shape_at(view, gx - fx,
		    gy - fy, vcw, vch);
		if (shape == WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT)
			continue;
		if (best == NULL || view->raise_seq > best_raise) {
			best		= view;
			best_raise	= view->raise_seq;
			*dx		= gx - fx;
			*dy		= gy - fy;
			*cw		= vcw;
			*ch		= vch;
		}
	}
	return (best);
}

static bool
pan_cursor_update(struct cow_output *output, int edge, uint32_t serial,
    wl_fixed_t sx, wl_fixed_t sy)
{
	if (serial != 0)
		decoration_cursor_serial = serial;

	int gx, gy;
	if (!cow_pan_frame_point(output, edge, wl_fixed_to_int(sx),
		wl_fixed_to_int(sy), &gx, &gy))
		return (false);

	int dx, dy, cw, ch;
	struct cow_view *view = view_for_global_decoration_point(gx, gy, &dx,
	    &dy, &cw, &ch);
	if (view == NULL) {
		cow_wm_reset_cursor_shape(decoration_cursor_serial);
		return (false);
	}

	cow_wm_set_cursor_shape(decoration_cursor_shape_at(view, dx, dy, cw,
	    ch), decoration_cursor_serial);
	return (true);
}

void
cow_pointer_cursor_hold_start(void)
{
	if (cow_wm.wl_cursor_shape !=
	    WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT)
		cursor_held = true;
}

void
cow_pointer_cursor_hold_end(void)
{
	if (!cursor_held)
		return;
	cursor_held = false;
	cow_wm_reset_cursor_shape(decoration_cursor_serial);
}

void
cow_pointer_cursor_clear(void)
{
	if (cursor_held)
		return;

	decoration_context_leave();
	decoration_cursor_surface = NULL;
	pan_cursor_output	  = NULL;
	pan_cursor_edge		  = -1;
	cow_wm_reset_cursor_shape(decoration_cursor_serial);
}

static struct cow_menu_popup *
surface_to_menu_popup(struct wl_surface *surface)
{
	if (surface == NULL || cow_wm.open_menus.next == NULL)
		return (NULL);
	struct cow_menu_popup *popup;
	wl_list_for_each(popup, &cow_wm.open_menus, link)
	{
		if (popup->surface == surface)
			return (popup);
	}
	return (NULL);
}

static struct cow_output *
surface_to_output_background(struct wl_surface *surface)
{
	if (surface == NULL)
		return (NULL);
	struct cow_output *output;
	wl_list_for_each(output, &cow_wm.outputs, wm_link)
	{
		if (output->bg_surface == surface)
			return (output);
	}
	return (NULL);
}

static void
seed_seat_pointer_from_background(void)
{
	if (!cow_wm.has_raw_pointer)
		return;

	struct cow_seat *seat = cow_seat_primary();
	if (seat == NULL)
		return;

	seat->pointer_x		   = cow_wm.raw_pointer_x;
	seat->pointer_y		   = cow_wm.raw_pointer_y;
	seat->has_pointer_position = true;
}

void
cow_pointer_cursor_reconcile_position(int x, int y)
{
	if (cursor_held)
		return;

	int dx, dy, cw, ch;
	struct cow_view *view = view_for_global_decoration_point(x, y, &dx,
	    &dy, &cw, &ch);
	if (view == NULL) {
		if (decoration_cursor_surface != NULL ||
		    pan_cursor_output != NULL) {
			decoration_context_leave();
			decoration_cursor_surface = NULL;
			pan_cursor_output	  = NULL;
			pan_cursor_edge		  = -1;
			cow_wm_reset_cursor_shape(decoration_cursor_serial);
		}
		return;
	}

	decoration_cursor_surface = view->decoration_surface;
	decoration_cursor_view = view;
	decoration_cursor_x = dx;
	decoration_cursor_y = dy;
	cow_keybinding_set_context(cow_seat_primary(),
	    decoration_binding_context_at(view, dx, dy, cw, ch), view);
	cow_wm_set_cursor_shape(decoration_cursor_shape_at(view, dx, dy, cw,
	    ch), decoration_cursor_serial);
}

void
cow_pointer_input_forget_output(struct cow_output *output)
{
	if (pointer_focus_background == output)
		pointer_focus_background = NULL;
	if (pan_cursor_output == output) {
		pan_cursor_output = NULL;
		pan_cursor_edge	  = -1;
		if (!cursor_held)
			cow_wm_reset_cursor_shape(decoration_cursor_serial);
	}
}

static void
raw_pointer_set_absolute(int x, int y)
{
	cow_wm.raw_pointer_x	= x;
	cow_wm.raw_pointer_y	= y;
	cow_wm.has_raw_pointer = true;
}

static void
raw_pointer_set_cow_surface(int x, int y)
{
	raw_pointer_set_absolute(x, y);
	if (decoration_cursor_surface != NULL || pan_cursor_output != NULL)
		cow_pointer_cursor_clear();
}

static void
pointer_enter(void *data, struct wl_pointer *pointer, uint32_t serial,
    struct wl_surface *surface, wl_fixed_t sx, wl_fixed_t sy)
{
	(void)data;
	(void)pointer;

	cow_wm_note_cursor_focus(serial, true);

	struct cow_menu_popup *popup = surface_to_menu_popup(surface);
	if (popup != NULL) {
		cow_wm.pointer_focus_menu = popup;
		pointer_focus_background = NULL;
		raw_pointer_set_cow_surface(popup->x + wl_fixed_to_int(sx),
		    popup->y + wl_fixed_to_int(sy));
		cow_wm.needs_manage_dirty = true;
		return;
	}

	struct cow_output *background = surface_to_output_background(surface);
	if (background != NULL) {
		pointer_focus_background = background;
		cow_wm.pointer_focus_menu = NULL;
		raw_pointer_set_cow_surface(background->x + wl_fixed_to_int(sx),
		    background->y + wl_fixed_to_int(sy));
		cow_keybinding_set_context(cow_seat_primary(),
		    COW_BIND_CONTEXT_ROOT, NULL);
		return;
	}

	struct cow_view *icon_view;
	wl_list_for_each(icon_view, &cow_wm.views, wm_link)
	{
		if (!icon_view->closed && icon_view->icon_surface == surface) {
			cow_keybinding_set_context(cow_seat_primary(),
			    COW_BIND_CONTEXT_ICON, icon_view);
			return;
		}
	}

	if (cow_wm.pointer_focus_menu != NULL)
		cow_wm.needs_manage_dirty = true;
	cow_wm.pointer_focus_menu = NULL;
	pointer_focus_background = NULL;

	struct cow_output *output;
	int		   edge;
	if (!cow_pan_frame_for_surface(surface, &output, &edge)) {
		if (cow_pick_pointer_enter(surface, serial, sx, sy))
			return;
		if (decoration_cursor_update(surface, serial, sx, sy)) {
			decoration_cursor_surface = surface;
			return;
		}
		return;
	}

	pan_cursor_output = output;
	pan_cursor_edge   = edge;
	pan_cursor_update(output, edge, serial, sx, sy);
	cow_pan_frame_pointer_enter(output, edge);
}

static void
pointer_leave(void *data, struct wl_pointer *pointer, uint32_t serial,
    struct wl_surface *surface)
{
	(void)data;
	(void)pointer;
	(void)serial;

	cow_wm_note_cursor_focus(0, false);
	struct cow_seat *seat = cow_seat_primary();

	if (cow_wm.pointer_focus_menu != NULL &&
	    cow_wm.pointer_focus_menu->surface == surface) {
		cow_wm.pointer_focus_menu = NULL;
		cow_wm.needs_manage_dirty = true;
	}
	if (pointer_focus_background != NULL &&
	    pointer_focus_background->bg_surface == surface) {
		pointer_focus_background = NULL;
		cow_keybinding_clear_context(seat, COW_BIND_CONTEXT_ROOT,
		    NULL);
	}
	if (seat != NULL && seat->binding_context == COW_BIND_CONTEXT_ICON &&
	    seat->binding_context_view != NULL &&
	    seat->binding_context_view->icon_surface == surface)
		cow_keybinding_set_context(seat, 0, NULL);

	struct cow_output *output;
	int		   edge;
	if (!cow_pan_frame_for_surface(surface, &output, &edge)) {
		if (cow_pick_pointer_leave(surface))
			return;
		if (decoration_cursor_surface == surface) {
			decoration_context_leave();
			decoration_cursor_surface = NULL;
			if (!cursor_held)
				cow_wm_reset_cursor_shape(
				    decoration_cursor_serial);
		}
		return;
	}

	if (pan_cursor_output == output && pan_cursor_edge == edge) {
		pan_cursor_output = NULL;
		pan_cursor_edge   = -1;
		if (!cursor_held)
			cow_wm_reset_cursor_shape(decoration_cursor_serial);
	}
	cow_pan_frame_pointer_leave(output, edge);
}

static void
pointer_motion(void *data, struct wl_pointer *pointer, uint32_t time,
    wl_fixed_t sx, wl_fixed_t sy)
{
	(void)data;
	(void)pointer;

	if (cow_wm.pointer_focus_menu != NULL) {
		raw_pointer_set_cow_surface(cow_wm.pointer_focus_menu->x +
			wl_fixed_to_int(sx),
		    cow_wm.pointer_focus_menu->y + wl_fixed_to_int(sy));
		if (cow_menu_is_open())
			cow_wm.needs_manage_dirty = true;
		cow_pick_pointer_motion(time, sx, sy);
		return;
	}

	if (pointer_focus_background != NULL) {
		raw_pointer_set_cow_surface(pointer_focus_background->x +
			wl_fixed_to_int(sx),
		    pointer_focus_background->y + wl_fixed_to_int(sy));
		if (cow_menu_is_open())
			cow_wm.needs_manage_dirty = true;
		cow_pick_pointer_motion(time, sx, sy);
		return;
	}

	if (decoration_cursor_surface != NULL &&
	    decoration_cursor_update(decoration_cursor_surface, 0, sx, sy))
		return;
	if (pan_cursor_output != NULL && pan_cursor_edge >= 0 &&
	    pan_cursor_update(pan_cursor_output, pan_cursor_edge, 0, sx, sy))
		return;

	raw_pointer_set_absolute(wl_fixed_to_int(sx), wl_fixed_to_int(sy));
	if (cow_menu_is_open())
		cow_wm.needs_manage_dirty = true;
	cow_pick_pointer_motion(time, sx, sy);
}

static void
pointer_button(void *data, struct wl_pointer *pointer, uint32_t serial,
    uint32_t time, uint32_t button, uint32_t state)
{
	(void)data;
	(void)pointer;
	(void)time;

	struct cow_seat *seat = cow_seat_primary();
	if (button == BTN_LEFT && seat != NULL) {
		if (state == WL_POINTER_BUTTON_STATE_PRESSED &&
		    decoration_cursor_view != NULL) {
			struct cow_view *view = decoration_cursor_view;
			int cw = view->content_width;
			int ch = view->content_height;
			if (view->sd != COW_SHADE_NONE)
				cow_view_shaded_content_size(view, view->sd, &cw,
				    &ch);
			int idx = decoration_button_at(view,
			    decoration_cursor_x, decoration_cursor_y, cw, ch);
			if (idx >= 0) {
				seat->pending_titlebar_button_press_view = view;
				seat->pending_titlebar_button_press_idx = idx;
				seat->pending_titlebar_button_release = false;
				seat->consumed_titlebar_button_interaction =
				    view;
				cow_wm_request_manage_dirty();
			}
		} else if (state == WL_POINTER_BUTTON_STATE_RELEASED &&
		    seat->pending_titlebar_button_press_view != NULL) {
			seat->pending_titlebar_button_release = true;
			cow_wm_request_manage_dirty();
		} else if (state == WL_POINTER_BUTTON_STATE_RELEASED &&
		    seat->titlebar_press == COW_TITLEBAR_PRESS_BUTTON) {
			seat->op_released = true;
			cow_wm_request_manage_dirty();
		}
	}

	if (state == WL_POINTER_BUTTON_STATE_PRESSED &&
	    (decoration_cursor_surface != NULL || pan_cursor_output != NULL))
		cow_pointer_cursor_hold_start();
	else if (state == WL_POINTER_BUTTON_STATE_RELEASED)
		cow_pointer_cursor_hold_end();

	if (state == WL_POINTER_BUTTON_STATE_PRESSED && cow_menu_is_open()) {
		if (seat != NULL)
			seat->menu_click_pending = true;
		cow_wm.needs_manage_dirty = true;
	}
	if (state == WL_POINTER_BUTTON_STATE_PRESSED && seat != NULL &&
	    seat->binding_context == COW_BIND_CONTEXT_ICON &&
	    seat->binding_context_view != NULL && !cow_menu_is_open()) {
		struct cow_view *icon_view = seat->binding_context_view;
		seat->consumed_icon_interaction = icon_view;
		if (cow_seat_dispatch_pointer_binding(seat, button,
			COW_BIND_CONTEXT_ICON, icon_view)) {
			cow_wm.needs_manage_dirty = true;
		} else if (seat->consumed_icon_interaction == icon_view)
			seat->consumed_icon_interaction = NULL;
	}
	if (state == WL_POINTER_BUTTON_STATE_PRESSED && seat != NULL &&
	    pointer_focus_background != NULL && !cow_menu_is_open()) {
		seed_seat_pointer_from_background();
		if (cow_seat_dispatch_pointer_binding(seat, button,
			COW_BIND_CONTEXT_ROOT, NULL))
			cow_wm.needs_manage_dirty = true;
	}

	cow_pick_pointer_button(serial, button, state);
}

static void
pointer_axis(void *data, struct wl_pointer *pointer, uint32_t time,
    uint32_t axis, wl_fixed_t value)
{
	(void)data;
	(void)pointer;
	(void)time;
	(void)axis;
	(void)value;
}

static void
pointer_frame(void *data, struct wl_pointer *pointer)
{
	(void)data;
	(void)pointer;
}

static void
pointer_axis_source(void *data, struct wl_pointer *pointer, uint32_t source)
{
	(void)data;
	(void)pointer;
	(void)source;
}

static void
pointer_axis_stop(void *data, struct wl_pointer *pointer, uint32_t time,
    uint32_t axis)
{
	(void)data;
	(void)pointer;
	(void)time;
	(void)axis;
}

static void
pointer_axis_discrete(void *data, struct wl_pointer *pointer, uint32_t axis,
    int32_t discrete)
{
	(void)data;
	(void)pointer;
	(void)axis;
	(void)discrete;
}

static const struct wl_pointer_listener pointer_listener = {
	.enter		 = pointer_enter,
	.leave		 = pointer_leave,
	.motion		 = pointer_motion,
	.button		 = pointer_button,
	.axis		 = pointer_axis,
	.frame		 = pointer_frame,
	.axis_source	 = pointer_axis_source,
	.axis_stop	 = pointer_axis_stop,
	.axis_discrete	 = pointer_axis_discrete,
};

void
cow_pointer_input_init(void)
{
	if (cow_wm.wl_seat_raw == NULL)
		return;

	/* wl_seat_get_pointer on a seat without a pointer capability is a
	 * protocol error, so wait until the capability has been advertised.
	 */
	if (cow_wm.wl_pointer_raw == NULL && cow_wm.wl_seat_has_pointer) {
		cow_wm.wl_pointer_raw = wl_seat_get_pointer(cow_wm.wl_seat_raw);
		if (cow_wm.wl_pointer_raw != NULL)
			wl_pointer_add_listener(cow_wm.wl_pointer_raw,
			    &pointer_listener, NULL);
		cow_wm_ensure_cursor_shape_device();
	}
}
