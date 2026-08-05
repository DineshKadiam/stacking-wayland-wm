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

#include <string.h>
#include <wayland-client.h>

#include "cow/compat.h"
#include "cow/config.h"
#include "cow/desk_ops.h"
#include "cow/ipc.h"
#include "cow/memory.h"
#include "cow/menu.h"
#include "cow/output.h"
#include "cow/pan_frames.h"
#include "cow/pick.h"
#include "cow/pointer_op.h"
#include "cow/seat.h"
#include "cow/shm.h"
#include "cow/titlebar.h"
#include "cow/util.h"
#include "cow/view.h"
#include "cow/wm.h"
#include "cursor-shape-v1-client-protocol.h"
#include "pointer-constraints-unstable-v1-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

/* Thickness of each pan frame surface in pixels. */
#define PAN_FRAME_SIZE 8

/* Colours */
#define PAN_COL_CLEAR 0x00000000u
#define PAN_COL_ORANGE 0xCCFF8800u

/* Pre-multiplied ARGB orange shades for fill levels 1..5. */
static const uint32_t pan_col_fill[5] = {
	0x29331C00u, /* ~16% */
	0x52663800u, /* ~32% */
	0x7A995400u, /* ~48% */
	0xA3CC7000u, /* ~64% */
	0xCCFF8800u, /* ~80% (matches PAN_COL_ORANGE) */
};

static struct cow_output *pointer_focus_background;

/* Index helpers */
/* Local short aliases for the public enum values used pervasively
 * below.  Defined here rather than in the header so the names stay
 * file-local; the enum in pan_frames.h is the canonical form. */
#define PAN_TOP COW_PAN_TOP
#define PAN_BOTTOM COW_PAN_BOTTOM
#define PAN_LEFT COW_PAN_LEFT
#define PAN_RIGHT COW_PAN_RIGHT

unsigned
cow_pan_edge_mask(int cur_col, int cur_row, int cols, int rows)
{
	unsigned mask = 0;
	if (rows > 1 && cur_row > 0)
		mask |= COW_PAN_EDGE_BIT(COW_PAN_TOP);
	if (rows > 1 && cur_row < rows - 1)
		mask |= COW_PAN_EDGE_BIT(COW_PAN_BOTTOM);
	if (cols > 1 && cur_col > 0)
		mask |= COW_PAN_EDGE_BIT(COW_PAN_LEFT);
	if (cols > 1 && cur_col < cols - 1)
		mask |= COW_PAN_EDGE_BIT(COW_PAN_RIGHT);
	return mask;
}

static void pan_frame_destroy(struct cow_output *output, int edge);
static void pan_frame_paint(struct cow_output *output, int edge, bool orange);
static void pan_frame_set_level(struct cow_output *output, int edge, int level);
static void pan_disarm(void);
static void pan_fire(struct cow_output *output, int edge);

static struct wl_surface *decoration_cursor_surface;
static uint32_t		  decoration_cursor_serial;
static struct cow_output *pan_cursor_output;
static int		  pan_cursor_edge = -1;
static bool		  cursor_held;

struct dec_frame_hit {
	int bw;
	int tt;
	int cl;
	int frame_w;
	int frame_h;
	int fx;
	int fy;
};

/* Cooldown after a fire so the post-warp cursor doesn't re-arm. */
#define PAN_GIVEUP_COOLDOWN_MS 200

/* Map a wl_surface back to (output, edge) */
static int
surface_to_output_edge(struct wl_surface *surf, struct cow_output **out_output)
{
	struct cow_output *output;
	wl_list_for_each(output, &cow_wm.outputs, wm_link)
	{
		for (int e = 0; e < 4; e++) {
			if (output->pan_surf[e] == surf) {
				*out_output = output;
				return e;
			}
		}
	}
	return -1;
}

/* Dwell timer: cursor on a strip for this many ms fires the page
 * switch.  cow_output_pan_frame_tick() is the periodic check. */
static int
pan_dwell_ms(void)
{
	int v = cow_config != NULL ? cow_config->edge_scroll_delay_ms : 300;
	if (v < 50)
		v = 50;
	return v;
}

/* Would a page-switch in this direction actually move the page? */
static bool
pan_edge_has_page(const struct cow_output *output, int edge)
{
	if (cow_config == NULL)
		return false;
	int cols = cow_config->page_cols;
	int rows = cow_config->page_rows;
	if (cols <= 0 || rows <= 0)
		return false;
	int cur_col = cow_page_col(output);
	int cur_row = cow_page_row(output);
	unsigned mask = cow_pan_edge_mask(cur_col, cur_row, cols, rows);
	return (mask & COW_PAN_EDGE_BIT(edge)) != 0;
}

static void
pan_arm_edge(struct cow_output *output, int edge)
{
	if (cow_mono_ms() < cow_wm.pan_global_cooldown_ms)
		return;
	if (cow_wm.pan_lock_output == output && cow_wm.pan_lock_edge == edge)
		return;
	/* No page in that direction -- skip arming so the cursor isn't
	 * trapped by a no-op orange fill. */
	if (!pan_edge_has_page(output, edge))
		return;
	if (cow_wm.pan_lock_output != NULL)
		pan_disarm();

	output->pan_entered[edge]  = true;
	output->pan_edge	   = edge;
	output->pan_resist_motion  = 0;
	output->pan_resist_done	   = false;
	output->pan_arm_ms	   = cow_mono_ms();
	cow_wm.pan_lock_output	   = output;
	cow_wm.pan_lock_edge	   = edge;

	cow_wm_request_manage_dirty();
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

static struct dec_frame_hit
decoration_frame_hit(struct cow_view *view, int sx, int sy, int cw, int ch)
{
	struct dec_frame_hit hit;

	hit.bw = view->dec.border_width;
	hit.tt = view->dec.title_thickness;
	/* Match wm.c's click hit-test: corner_length extends inward from
	 * the inner border edge so output-edge corners remain reachable.
	 */
	hit.cl = view->cfg.corner_length + hit.bw;

	int content_x, content_y;
	cow_titlebar_frame_geometry(view, cw, ch, &hit.frame_w,
	    &hit.frame_h, &content_x, &content_y);
	hit.fx = sx;
	hit.fy = sy;
	return (hit);
}

static bool
decoration_in_border(const struct dec_frame_hit *hit)
{
	if (hit->fx < 0 || hit->fx >= hit->frame_w ||
	    hit->fy < 0 || hit->fy >= hit->frame_h)
		return (false);
	return (hit->fx < hit->bw || hit->fx >= hit->frame_w - hit->bw ||
	    hit->fy < hit->bw || hit->fy >= hit->frame_h - hit->bw);
}

static uint32_t
decoration_corner_edges(struct cow_view *view, int sx, int sy, int cw, int ch)
{
	struct dec_frame_hit hit = decoration_frame_hit(view, sx, sy, cw, ch);

	if (!decoration_in_border(&hit))
		return (0);

	int content_x, content_y, frame_w, frame_h;
	cow_titlebar_frame_geometry(view, cw, ch, &frame_w, &frame_h,
	    &content_x, &content_y);
	if (cow_titlebar_hit_for(view, cw, ch, sx - content_x,
		sy - content_y))
		return (0);

	bool near_top	= (hit.fy < hit.cl);
	bool near_bot	= (hit.fy >= hit.frame_h - hit.cl);
	bool near_left	= (hit.fx < hit.cl);
	bool near_right = (hit.fx >= hit.frame_w - hit.cl);

	if (near_top && near_left)
		return (COW_EDGE_TOP | COW_EDGE_LEFT);
	if (near_top && near_right)
		return (COW_EDGE_TOP | COW_EDGE_RIGHT);
	if (near_bot && near_left)
		return (COW_EDGE_BOTTOM | COW_EDGE_LEFT);
	if (near_bot && near_right)
		return (COW_EDGE_BOTTOM | COW_EDGE_RIGHT);
	if (hit.fy < hit.bw)
		return (COW_EDGE_TOP);
	if (hit.fy >= hit.frame_h - hit.bw)
		return (COW_EDGE_BOTTOM);
	if (hit.fx < hit.bw)
		return (COW_EDGE_LEFT);
	if (hit.fx >= hit.frame_w - hit.bw)
		return (COW_EDGE_RIGHT);
	return (0);
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
	uint32_t edges = decoration_corner_edges(view, x, y, cw, ch);
	if (edges != 0)
		return (cursor_shape_for_edges(edges));

	int content_x, content_y, frame_w, frame_h;
	cow_titlebar_frame_geometry(view, cw, ch, &frame_w, &frame_h,
	    &content_x, &content_y);
	if (cow_titlebar_hit_for(view, cw, ch, x - content_x, y - content_y))
		return (WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_GRAB);

	return (WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT);
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
	int cw = view->content_width;
	int ch = view->content_height;
	if (view->sd != COW_SHADE_NONE)
		cow_view_shaded_content_size(view, view->sd, &cw, &ch);

	uint32_t shape = decoration_cursor_shape_at(view, x, y, cw, ch);

	cow_wm_set_cursor_shape(shape, decoration_cursor_serial);
	return (true);
}

static bool
pan_frame_point(struct cow_output *output, int edge, wl_fixed_t sx,
    wl_fixed_t sy, int *gx, int *gy)
{
	if (output == NULL || edge < 0 || edge > 3)
		return (false);

	*gx = output->x + wl_fixed_to_int(sx);
	*gy = output->y + wl_fixed_to_int(sy);
	switch (edge) {
	case PAN_BOTTOM:
		*gy += output->height - PAN_FRAME_SIZE;
		break;
	case PAN_RIGHT:
		*gx += output->width - PAN_FRAME_SIZE;
		break;
	case PAN_TOP:
	case PAN_LEFT:
		break;
	default:
		return (false);
	}
	return (true);
}

static struct cow_view *
view_for_global_decoration_point(int gx, int gy, int *dx, int *dy, int *cw,
    int *ch)
{
	struct cow_view *view, *best = NULL;
	uint64_t	best_raise = 0;

	wl_list_for_each(view, &cow_wm.views, wm_link)
	{
		if (view->closed || !view->use_ssd || view->output == NULL)
			continue;
		if (!cow_view_visible(view, view->output))
			continue;

		int vcw = view->content_width;
		int vch = view->content_height;
		if (view->sd != COW_SHADE_NONE)
			cow_view_shaded_content_size(view, view->sd, &vcw, &vch);

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
			best	    = view;
			best_raise  = view->raise_seq;
			*dx	    = gx - fx;
			*dy	    = gy - fy;
			*cw	    = vcw;
			*ch	    = vch;
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
	if (!pan_frame_point(output, edge, sx, sy, &gx, &gy))
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
cow_pan_cursor_hold_start(void)
{
	if (cow_wm.wl_cursor_shape !=
	    WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT)
		cursor_held = true;
}

void
cow_pan_cursor_hold_end(void)
{
	if (!cursor_held)
		return;
	cursor_held = false;
	cow_wm_reset_cursor_shape(decoration_cursor_serial);
}

void
cow_pan_cursor_clear(void)
{
	if (cursor_held)
		return;

	decoration_cursor_surface = NULL;
	pan_cursor_output	  = NULL;
	pan_cursor_edge		  = -1;
	cow_wm_reset_cursor_shape(decoration_cursor_serial);
}

static struct cow_menu_popup *
surface_to_menu_popup(struct wl_surface *surf)
{
	if (surf == NULL || cow_wm.open_menus.next == NULL)
		return NULL;
	struct cow_menu_popup *p;
	wl_list_for_each(p, &cow_wm.open_menus, link)
	{
		if (p->surface == surf)
			return p;
	}
	return NULL;
}

static struct cow_output *
surface_to_output_background(struct wl_surface *surf)
{
	if (surf == NULL)
		return NULL;
	struct cow_output *output;
	wl_list_for_each(output, &cow_wm.outputs, wm_link)
	{
		if (output->bg_surface == surf)
			return output;
	}
	return NULL;
}

static bool
dispatch_background_mouse_binding(const char *key)
{
	if (cow_config == NULL || key == NULL)
		return false;

	struct cow_mouse_binding_entry *entry;
	wl_list_for_each(entry, &cow_config->root_mouse_bindings, link)
	{
		if (entry->key_string == NULL || entry->action_string == NULL)
			continue;
		if (strcmp(entry->key_string, key) != 0)
			continue;
		return cow_ipc_dispatch(entry->action_string, -1);
	}
	return false;
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
cow_pan_cursor_reconcile_position(int x, int y)
{
	if (cursor_held)
		return;

	int dx, dy, cw, ch;
	struct cow_view *view = view_for_global_decoration_point(x, y, &dx,
	    &dy, &cw, &ch);
	if (view == NULL) {
		if (decoration_cursor_surface != NULL ||
		    pan_cursor_output != NULL) {
			decoration_cursor_surface = NULL;
			pan_cursor_output	  = NULL;
			pan_cursor_edge		  = -1;
			cow_wm_reset_cursor_shape(decoration_cursor_serial);
		}
		return;
	}

	decoration_cursor_surface = view->decoration_surface;
	cow_wm_set_cursor_shape(decoration_cursor_shape_at(view, dx, dy, cw,
	    ch), decoration_cursor_serial);
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
		cow_pan_cursor_clear();
}

static void
pointer_enter(void *data, struct wl_pointer *pointer, uint32_t serial,
    struct wl_surface *surf, wl_fixed_t sx, wl_fixed_t sy)
{
	(void)data;
	(void)pointer;

	cow_wm_note_cursor_focus(serial, true);

	struct cow_menu_popup *mp = surface_to_menu_popup(surf);
	if (mp != NULL) {
		cow_wm.pointer_focus_menu = mp;
		pointer_focus_background = NULL;
		raw_pointer_set_cow_surface(mp->x + wl_fixed_to_int(sx),
		    mp->y + wl_fixed_to_int(sy));
		cow_wm.needs_manage_dirty = true;
		return;
	}

	struct cow_output *bg_output = surface_to_output_background(surf);
	if (bg_output != NULL) {
		pointer_focus_background = bg_output;
		cow_wm.pointer_focus_menu = NULL;
		raw_pointer_set_cow_surface(bg_output->x + wl_fixed_to_int(sx),
		    bg_output->y + wl_fixed_to_int(sy));
		return;
	}

	if (cow_wm.pointer_focus_menu != NULL)
		cow_wm.needs_manage_dirty = true;
	cow_wm.pointer_focus_menu = NULL;
	pointer_focus_background = NULL;

	struct cow_output *output;
	int		   edge = surface_to_output_edge(surf, &output);
	if (edge < 0) {
		if (cow_pick_pointer_enter(surf, serial, sx, sy))
			return;
		if (decoration_cursor_update(surf, serial, sx, sy)) {
			decoration_cursor_surface = surf;
			return;
		}
		return;
	}

	pan_cursor_output = output;
	pan_cursor_edge   = edge;
	pan_cursor_update(output, edge, serial, sx, sy);
	output->pan_entered[edge] = true;

	long now = cow_mono_ms();
	if (now < output->pan_giveup_until_ms)
		return;
	if (now < cow_wm.pan_global_cooldown_ms)
		return;

	pan_arm_edge(output, edge);
}

static void
pointer_leave(void *data, struct wl_pointer *pointer, uint32_t serial,
    struct wl_surface *surf)
{
	(void)data;
	(void)pointer;
	(void)serial;

	cow_wm_note_cursor_focus(0, false);

	/* Leaving a menu popup surface: drop the focus pointer.  The
	 * hover-update in cow_menu_process_input picks this up and
	 * de-highlights items in popups that aren't under the pointer
	 * any more. */
	if (cow_wm.pointer_focus_menu != NULL &&
	    cow_wm.pointer_focus_menu->surface == surf) {
		cow_wm.pointer_focus_menu = NULL;
		cow_wm.needs_manage_dirty = true;
	}
	if (pointer_focus_background != NULL &&
	    pointer_focus_background->bg_surface == surf)
		pointer_focus_background = NULL;

	struct cow_output *output;
	int		   edge = surface_to_output_edge(surf, &output);
	if (edge < 0) {
		if (cow_pick_pointer_leave(surf))
			return;
		if (decoration_cursor_surface == surf) {
			decoration_cursor_surface = NULL;
			if (!cursor_held)
				cow_wm_reset_cursor_shape(
				    decoration_cursor_serial);
		}
		return;
	}

	output->pan_entered[edge] = false;
	if (pan_cursor_output == output && pan_cursor_edge == edge) {
		pan_cursor_output = NULL;
		pan_cursor_edge   = -1;
		if (!cursor_held)
			cow_wm_reset_cursor_shape(decoration_cursor_serial);
	}
	if (cow_wm.pan_lock_output == output && cow_wm.pan_lock_edge == edge)
		pan_disarm();
}

static void
pointer_motion(void *d, struct wl_pointer *p, uint32_t t, wl_fixed_t x,
    wl_fixed_t y)
{
	(void)d;
	(void)p;

	if (cow_wm.pointer_focus_menu != NULL) {
		raw_pointer_set_cow_surface(cow_wm.pointer_focus_menu->x +
			wl_fixed_to_int(x),
		    cow_wm.pointer_focus_menu->y + wl_fixed_to_int(y));
		if (cow_menu_is_open())
			cow_wm.needs_manage_dirty = true;
		cow_pick_pointer_motion(t, x, y);
		return;
	}

	if (pointer_focus_background != NULL) {
		raw_pointer_set_cow_surface(pointer_focus_background->x +
			wl_fixed_to_int(x),
		    pointer_focus_background->y + wl_fixed_to_int(y));
		if (cow_menu_is_open())
			cow_wm.needs_manage_dirty = true;
		cow_pick_pointer_motion(t, x, y);
		return;
	}

	if (decoration_cursor_surface != NULL &&
	    decoration_cursor_update(decoration_cursor_surface, 0, x, y))
		return;
	if (pan_cursor_output != NULL && pan_cursor_edge >= 0 &&
	    pan_cursor_update(pan_cursor_output, pan_cursor_edge, 0, x, y))
		return;

	cow_wm.raw_pointer_x   = wl_fixed_to_int(x);
	cow_wm.raw_pointer_y   = wl_fixed_to_int(y);
	cow_wm.has_raw_pointer = true;

	if (cow_menu_is_open())
		cow_wm.needs_manage_dirty = true;

	cow_pick_pointer_motion(t, x, y);
}

static void
pointer_button(void *d, struct wl_pointer *p, uint32_t ser, uint32_t t,
    uint32_t btn, uint32_t state)
{
	(void)d;
	(void)p;
	(void)t;

	if (state == WL_POINTER_BUTTON_STATE_PRESSED &&
	    (decoration_cursor_surface != NULL || pan_cursor_output != NULL))
		cow_pan_cursor_hold_start();
	else if (state == WL_POINTER_BUTTON_STATE_RELEASED)
		cow_pan_cursor_hold_end();

	if (state == WL_POINTER_BUTTON_STATE_PRESSED && cow_menu_is_open()) {
		(void)btn;
		struct cow_seat *s = cow_seat_primary();
		if (s != NULL)
			s->menu_click_pending = true;
		cow_wm.needs_manage_dirty = true;
	}
	if (state == WL_POINTER_BUTTON_STATE_PRESSED && btn == BTN_LEFT &&
	    pointer_focus_background != NULL && !cow_menu_is_open()) {
		seed_seat_pointer_from_background();
		if (dispatch_background_mouse_binding("0+left"))
			cow_wm.needs_manage_dirty = true;
	}

	cow_pick_pointer_button(ser, btn, state);
}

static void
pointer_axis(void *d, struct wl_pointer *p, uint32_t t, uint32_t a,
    wl_fixed_t v)
{
	(void)d;
	(void)p;
	(void)t;
	(void)a;
	(void)v;
}

static void
pointer_frame(void *d, struct wl_pointer *p)
{
	(void)d;
	(void)p;
}

static void
pointer_axis_source(void *d, struct wl_pointer *p, uint32_t s)
{
	(void)d;
	(void)p;
	(void)s;
}

static void
pointer_axis_stop(void *d, struct wl_pointer *p, uint32_t t, uint32_t a)
{
	(void)d;
	(void)p;
	(void)t;
	(void)a;
}

static void
pointer_axis_discrete(void *d, struct wl_pointer *p, uint32_t a, int32_t v)
{
	(void)d;
	(void)p;
	(void)a;
	(void)v;
}

static const struct wl_pointer_listener pointer_listener = {
	.enter	       = pointer_enter,
	.leave	       = pointer_leave,
	.motion	       = pointer_motion,
	.button	       = pointer_button,
	.axis	       = pointer_axis,
	.frame	       = pointer_frame,
	.axis_source   = pointer_axis_source,
	.axis_stop     = pointer_axis_stop,
	.axis_discrete = pointer_axis_discrete,
};

static void
pan_disarm(void)
{
	struct cow_output *out	= cow_wm.pan_lock_output;
	int		   edge = cow_wm.pan_lock_edge;
	if (out != NULL && edge >= 0) {
		pan_frame_set_level(out, edge, 0);
		out->pan_edge	= -1;
		out->pan_arm_ms = 0;
	}
	cow_wm.pan_lock_output = NULL;
	cow_wm.pan_lock_edge   = -1;
}

static bool
pan_edge_is_shared(const struct cow_output *output, int edge)
{
	struct cow_output *o;
	wl_list_for_each(o, &cow_wm.outputs, wm_link)
	{
		if (o == output)
			continue;
		switch (edge) {
		case COW_PAN_TOP:
			if (o->y + o->height == output->y &&
			    o->x < output->x + output->width &&
			    o->x + o->width > output->x)
				return true;
			break;
		case COW_PAN_BOTTOM:
			if (output->y + output->height == o->y &&
			    o->x < output->x + output->width &&
			    o->x + o->width > output->x)
				return true;
			break;
		case COW_PAN_LEFT:
			if (o->x + o->width == output->x &&
			    o->y < output->y + output->height &&
			    o->y + o->height > output->y)
				return true;
			break;
		case COW_PAN_RIGHT:
			if (output->x + output->width == o->x &&
			    o->y < output->y + output->height &&
			    o->y + o->height > output->y)
				return true;
			break;
		}
	}
	return false;
}

struct pan_frame_ctx {
	struct cow_output *output;
	int		   edge;
};

/* zwlr_layer_surface configure callback */
static void
layer_surface_configure(void *data, struct zwlr_layer_surface_v1 *lsurf,
    uint32_t serial, uint32_t w, uint32_t h)
{
	(void)w;
	(void)h;
	zwlr_layer_surface_v1_ack_configure(lsurf, serial);

	/* First configure: attach the transparent buffer now that the
	 * compositor has acked the surface. */
	struct pan_frame_ctx *ctx = data;
	if (ctx == NULL)
		return;
	struct cow_output *output = ctx->output;
	int		   edge	  = ctx->edge;
	if (output->pan_surf[edge] == NULL)
		return;
	struct cow_shm_buffer *buf = output->pan_buf_clear[edge];
	if (buf == NULL)
		return;
	wl_surface_attach(output->pan_surf[edge], buf->buffer, 0, 0);
	wl_surface_damage_buffer(output->pan_surf[edge], 0, 0, buf->width,
	    buf->height);
	wl_surface_commit(output->pan_surf[edge]);
}

static void
layer_surface_closed(void *data, struct zwlr_layer_surface_v1 *lsurf)
{
	(void)data;
	(void)lsurf;
}

static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
	.configure = layer_surface_configure,
	.closed	   = layer_surface_closed,
};

static struct cow_shm_buffer *
make_solid_buffer(int w, int h, uint32_t colour)
{
	struct cow_shm_buffer *buf = cow_shm_buffer_create(w, h);
	if (!buf)
		return NULL;

	uint32_t *px	 = (uint32_t *)buf->data;
	int	  stride = buf->stride / 4;

	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++)
			px[y * stride + x] = colour;
	return buf;
}

/* level: 0 = clear, 1..5 = fill 1/5..5/5. */
static void
pan_frame_set_level(struct cow_output *output, int edge, int level)
{
	if (output->pan_surf[edge] == NULL)
		return;
	if (level < 0)
		level = 0;
	if (level > 5)
		level = 5;
	if (output->pan_fill_level[edge] == level)
		return;
	output->pan_fill_level[edge] = level;

	struct cow_shm_buffer *buf;
	if (level == 0)
		buf = output->pan_buf_clear[edge];
	else
		buf = output->pan_buf_fill[edge][level - 1];
	if (!buf)
		return;

	wl_surface_attach(output->pan_surf[edge], buf->buffer, 0, 0);
	wl_surface_damage_buffer(output->pan_surf[edge], 0, 0, buf->width,
	    buf->height);
	wl_surface_commit(output->pan_surf[edge]);
}

static void
pan_frame_paint(struct cow_output *output, int edge, bool orange)
{
	if (output->pan_surf[edge] == NULL)
		return;

	struct cow_shm_buffer *buf = orange ? output->pan_buf_orange[edge] :
					      output->pan_buf_clear[edge];
	if (!buf)
		return;
	output->pan_fill_level[edge] = orange ? 5 : 0;

	wl_surface_attach(output->pan_surf[edge], buf->buffer, 0, 0);
	wl_surface_damage_buffer(output->pan_surf[edge], 0, 0, buf->width,
	    buf->height);
	wl_surface_commit(output->pan_surf[edge]);
}

static void
pan_frame_destroy(struct cow_output *output, int edge)
{
	if (cow_wm.pan_lock_output == output && cow_wm.pan_lock_edge == edge)
		pan_disarm();
	if (output->pan_lsurf[edge] != NULL) {
		zwlr_layer_surface_v1_destroy(output->pan_lsurf[edge]);
		output->pan_lsurf[edge] = NULL;
	}
	if (output->pan_surf[edge] != NULL) {
		wl_surface_destroy(output->pan_surf[edge]);
		output->pan_surf[edge] = NULL;
	}

	cow_shm_buffer_destroy(output->pan_buf_clear[edge]);
	output->pan_buf_clear[edge] = NULL;

	cow_shm_buffer_destroy(output->pan_buf_orange[edge]);
	output->pan_buf_orange[edge] = NULL;

	for (int i = 0; i < 5; i++) {
		cow_shm_buffer_destroy(output->pan_buf_fill[edge][i]);
		output->pan_buf_fill[edge][i] = NULL;
	}
	output->pan_fill_level[edge] = 0;

	xfree(output->pan_ctx[edge]);

	output->pan_ctx[edge]	  = NULL;
	output->pan_entered[edge] = false;

	if (output->pan_edge == edge)
		output->pan_edge = -1;
}

static bool
edge_is_scrollable(struct cow_output *output, int edge)
{
	if (!cow_config || !cow_config->edge_scroll)
		return false;
	int cols = cow_config->page_cols;
	int rows = cow_config->page_rows;
	if (cols <= 1 && rows <= 1)
		return false;
	if (edge < 0 || edge > 3)
		return false;

	/* Per-output / shared: every edge of every output gets a strip
	 * regardless of current page position -- each output pages
	 * independently and the user expects all four edges available. */
	if (cow_config->desktop_config != COW_DESKTOP_GLOBAL)
		return true;

	/* Global: only outer edges of the combined layout get strips.
	 * Edges that abut another monitor are not scroll-fire points
	 * because the cursor naturally crosses to the neighbour there. */
	if (pan_edge_is_shared(output, edge))
		return false;

	int		 cur_col, cur_row;
	struct cow_desk *desk = output->current_desk;
	if (desk != NULL && desk->scrolling) {
		int w	= output->usable_area.width;
		int h	= output->usable_area.height;
		cur_col = (w > 0) ? desk->scroll_to_vx / w : 0;
		cur_row = (h > 0) ? desk->scroll_to_vy / h : 0;
	} else {
		cur_col = cow_page_col(output);
		cur_row = cow_page_row(output);
	}

	unsigned mask = cow_pan_edge_mask(cur_col, cur_row, cols, rows);
	return (mask & COW_PAN_EDGE_BIT(edge)) != 0;
}

static void
pan_frame_create(struct cow_output *output, int edge)
{
	if (!cow_wm.wl_layer_shell || !cow_wm.compositor || !cow_wm.shm)
		return;

	/* Output dimensions or wl_output not yet known — defer. */
	if (output->width <= 0 || output->height <= 0)
		return;
	if (output->wl_output == NULL)
		return;

	pan_frame_destroy(output, edge);

	int	 ow = output->width, oh = output->height;
	int	 iw, ih;
	uint32_t anchor;

	switch (edge) {
	case PAN_TOP:
		iw     = ow;
		ih     = PAN_FRAME_SIZE;
		anchor = ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
		    ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
		    ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
		break;
	case PAN_BOTTOM:
		iw     = ow;
		ih     = PAN_FRAME_SIZE;
		anchor = ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
		    ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
		    ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
		break;
	case PAN_LEFT:
		iw     = PAN_FRAME_SIZE;
		ih     = oh;
		anchor = ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
		    ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
		    ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM;
		break;
	case PAN_RIGHT:
		iw     = PAN_FRAME_SIZE;
		ih     = oh;
		anchor = ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT |
		    ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
		    ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM;
		break;
	default:
		return;
	}

	struct wl_surface *surf = wl_compositor_create_surface(
	    cow_wm.compositor);
	if (!surf)
		return;

	struct zwlr_layer_surface_v1 *lsurf =
	    zwlr_layer_shell_v1_get_layer_surface(cow_wm.wl_layer_shell, surf,
		output->wl_output, ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,
		"cow-pan");
	if (!lsurf) {
		wl_surface_destroy(surf);
		return;
	}

	/* Pre-create both buffers before the initial commit so they are
	 * ready when the configure callback fires. */
	struct cow_shm_buffer *buf_clear  = make_solid_buffer(iw, ih,
	     PAN_COL_CLEAR);
	struct cow_shm_buffer *buf_orange = make_solid_buffer(iw, ih,
	    PAN_COL_ORANGE);

	if (!buf_clear || !buf_orange) {
		cow_shm_buffer_destroy(buf_clear);
		cow_shm_buffer_destroy(buf_orange);
		zwlr_layer_surface_v1_destroy(lsurf);
		wl_surface_destroy(surf);
		return;
	}

	output->pan_surf[edge]	     = surf;
	output->pan_lsurf[edge]	     = lsurf;
	output->pan_buf_clear[edge]  = buf_clear;
	output->pan_buf_orange[edge] = buf_orange;
	output->pan_entered[edge]    = false;
	output->pan_fill_level[edge] = 0;
	for (int i = 0; i < 5; i++)
		output->pan_buf_fill[edge][i] = make_solid_buffer(iw, ih,
		    pan_col_fill[i]);

	/* ctx freed in pan_frame_destroy */
	struct pan_frame_ctx *ctx = xcalloc(1, sizeof(*ctx));
	ctx->output		  = output;
	ctx->edge		  = edge;
	output->pan_ctx[edge]	  = ctx;

	zwlr_layer_surface_v1_set_size(lsurf, iw, ih);
	zwlr_layer_surface_v1_set_anchor(lsurf, anchor);
	/* exclusive_zone=-1: ignore reserved areas. */
	zwlr_layer_surface_v1_set_exclusive_zone(lsurf, -1);
	zwlr_layer_surface_v1_set_keyboard_interactivity(lsurf,
	    ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
	zwlr_layer_surface_v1_add_listener(lsurf, &layer_surface_listener, ctx);

	/* Initial commit: triggers compositor configure event.
	 * Do NOT attach a buffer here — protocol requires waiting for
	 * configure before attaching. layer_surface_configure() will
	 * attach the transparent buffer after acking.
	 */
	wl_surface_commit(surf);
}

void
pan_frames_init(void)
{
	cow_wm.pan_lock_edge = -1;

	if (!cow_wm.wl_seat_raw)
		return;

	/* wl_seat_get_pointer on a seat without a pointer cap is a fatal
	 * protocol error; only call it once the cap is advertised. */
	if (cow_wm.wl_pointer_raw == NULL && cow_wm.wl_seat_has_pointer) {
		cow_wm.wl_pointer_raw = wl_seat_get_pointer(cow_wm.wl_seat_raw);
		if (cow_wm.wl_pointer_raw != NULL)
			wl_pointer_add_listener(cow_wm.wl_pointer_raw,
			    &pointer_listener, NULL);
		cow_wm_ensure_cursor_shape_device();
	}
}

void
cow_pan_suppress_for(long ms)
{
	long until = cow_mono_ms() + ms;

	if (cow_wm.pan_global_cooldown_ms < until)
		cow_wm.pan_global_cooldown_ms = until;
	pan_disarm();
}

void
pan_frame_set_orange(struct cow_output *output, int edge, bool orange)
{
	if (edge < 0 || edge > 3)
		return;

	output->pan_entered[edge] = orange;
	pan_frame_paint(output, edge, orange);
}

void
pan_frames_update_output(struct cow_output *output)
{
	for (int e = 0; e < 4; e++) {
		bool needed = edge_is_scrollable(output, e);
		bool exists = output->pan_surf[e] != NULL;

		if (needed && !exists) {
			pan_frame_create(output, e);
		} else if (!needed && exists) {
			pan_frame_destroy(output, e);
		} else if (exists) {
			/* Clear any stale fill from the previous accumulation
			 * (a page just switched, or the page grid changed). */
			output->pan_fill_level[e] = -1; /* force repaint */
			pan_frame_set_level(output, e, 0);
		}
	}
}

void
pan_frames_update_all(void)
{
	struct cow_output *output;
	wl_list_for_each(output, &cow_wm.outputs, wm_link)
	    pan_frames_update_output(output);
}

void
pan_frames_destroy_output(struct cow_output *output)
{
	if (pointer_focus_background == output)
		pointer_focus_background = NULL;
	if (pan_cursor_output == output) {
		pan_cursor_output = NULL;
		pan_cursor_edge	  = -1;
		if (!cursor_held)
			cow_wm_reset_cursor_shape(decoration_cursor_serial);
	}

	for (int e = 0; e < 4; e++)
		pan_frame_destroy(output, e);
}

void
pan_frames_destroy_all(void)
{
	struct cow_output *output;
	wl_list_for_each(output, &cow_wm.outputs, wm_link)
	    pan_frames_destroy_output(output);
}

/* Active MOVE drag (excluding container drags), or NULL. */
static struct cow_view *
pan_find_drag_view(void)
{
	struct cow_seat *s;
	wl_list_for_each(s, &cow_wm.seats, wm_link)
	{
		if (s->op_in_progress && s->op.type == COW_POINTER_OP_MOVE &&
		    !s->container_drag && s->op.view != NULL &&
		    !s->op.view->closed)
			return s->op.view;
	}
	return NULL;
}

/* Compute view's frame edges (incl. border + titlebar) in global coords. */
static void
pan_view_frame(const struct cow_view *v, int *l, int *t, int *r, int *b)
{
	struct cow_box fb = cow_view_frame_box(v);
	*l		  = fb.x;
	*t		  = fb.y;
	*r		  = fb.x + fb.width;
	*b		  = fb.y + fb.height;
}

/* Which edge of 'output' the pointer is touching, or -1. */
static int
pan_edge_for_pointer(const struct cow_output *output, int px, int py, int strip)
{
	int cols = cow_config->page_cols;
	int rows = cow_config->page_rows;

	if (rows <= 0 || cols <= 0)
		return (-1);

	/* Only arm edges where a page-switch would actually move
	 * somewhere -- a no-op edge (already at the row/col boundary
	 * of the page grid) shouldn't trap the cursor. */
	int	 cur_col = cow_page_col(output);
	int	 cur_row = cow_page_row(output);
	unsigned mask	 = cow_pan_edge_mask(cur_col, cur_row, cols, rows);

	if ((mask & COW_PAN_EDGE_BIT(COW_PAN_TOP)) &&
	    py <= output->y + strip)
		return COW_PAN_TOP;
	if ((mask & COW_PAN_EDGE_BIT(COW_PAN_BOTTOM)) &&
	    py >= output->y + output->height - strip)
		return COW_PAN_BOTTOM;
	if ((mask & COW_PAN_EDGE_BIT(COW_PAN_LEFT)) &&
	    px <= output->x + strip)
		return COW_PAN_LEFT;
	if ((mask & COW_PAN_EDGE_BIT(COW_PAN_RIGHT)) &&
	    px >= output->x + output->width - strip)
		return COW_PAN_RIGHT;

	return -1;
}

/* Direction tables for the four pan edges.  Indexed by edge value. */
static const struct {
	int dcol; /* delta col on page-switch */
	int drow; /* delta row on page-switch */
} pan_edge_dir[4] = {
	{ 0,  -1 }, /* 0 = TOP    */
	{ 0,  +1 }, /* 1 = BOTTOM */
	{ -1, 0	}, /* 2 = LEFT   */
	{ +1, 0	}, /* 3 = RIGHT  */
};

/*
 * Compute the warp target for landing on the new page after firing 'edge'.
 *
 * Without a drag, the cursor jumps to a fixed margin from the new page's
 * leading edge (the edge opposite the one the user crossed) so it doesn't
 * immediately re-trigger the strip we just fired from.
 *
 * With an active MOVE drag, we additionally pick the cursor position so
 * the dragged view's leading edge sits one base-margin inside the new
 * page's leading edge -- this means the whole window is on-screen as
 * soon as the page change completes, rather than only revealing as the
 * cursor moves further.
 *
 * The cursor target is also kept inside the usable area so the cursor
 * doesn't end up off-screen for very tall/wide windows; in that case the
 * view simply extends past the trailing edge as it would naturally.
 */
static void
pan_warp_target(const struct cow_output *output, int edge, int ptr_x, int ptr_y,
    const struct cow_view *drag, int *out_x, int *out_y)
{
	const int margin = 64;
	int	  ua_top = output->usable_area.y;
	int	  ua_bot = output->usable_area.y + output->usable_area.height;
	int	  ua_lft = output->usable_area.x;
	int	  ua_rgt = output->usable_area.x + output->usable_area.width;

	*out_x = ptr_x;
	*out_y = ptr_y;

	int  v_l = 0, v_t = 0, v_r = 0, v_b = 0;
	bool have_view = drag != NULL && drag->output == output;
	if (have_view)
		pan_view_frame(drag, &v_l, &v_t, &v_r, &v_b);

	switch (edge) {
	case 0: /* UP -> land near bottom of new page */
		if (have_view) {
			/* Want: v_b_new = ua_bot - margin
			 * v_b = ptr_y + (v_b - ptr_y), so warp ptr to:
			 * warp_y = (ua_bot - margin) - (v_b - ptr_y) */
			*out_y = (ua_bot - margin) - (v_b - ptr_y);
		} else {
			*out_y = ua_bot - margin;
		}
		break;
	case 1: /* DOWN -> land near top of new page */
		if (have_view)
			*out_y = (ua_top + margin) + (ptr_y - v_t);
		else
			*out_y = ua_top + margin;
		break;
	case 2: /* LEFT -> land near right of new page */
		if (have_view)
			*out_x = (ua_rgt - margin) - (v_r - ptr_x);
		else
			*out_x = ua_rgt - margin;
		break;
	case 3: /* RIGHT -> land near left of new page */
		if (have_view)
			*out_x = (ua_lft + margin) + (ptr_x - v_l);
		else
			*out_x = ua_lft + margin;
		break;
	}

	/* Clamp warp to the usable area so the cursor itself stays
	 * on-screen and inside the page (we'd rather have the view
	 * extend past the trailing edge than land cursor off-screen). */
	if (*out_x < ua_lft + margin)
		*out_x = ua_lft + margin;
	if (*out_x > ua_rgt - margin)
		*out_x = ua_rgt - margin;
	if (*out_y < ua_top + margin)
		*out_y = ua_top + margin;
	if (*out_y > ua_bot - margin)
		*out_y = ua_bot - margin;
}

/*
 * After a page change during a drag, the dragged view should follow the
 * (warped) cursor.  Our MOVE formula is view->x = cursor + offset, so
 * once seat->pointer_x is updated to the warp target (handled by the
 * caller), cow_pointer_op_process naturally repositions the view.
 */
static void
pan_adjust_drags(struct cow_output *output, int warp_dx, int warp_dy)
{
	if (warp_dx == 0 && warp_dy == 0)
		return;
	struct cow_seat *s;
	wl_list_for_each(s, &cow_wm.seats, wm_link)
	{
		if (!s->op_in_progress || s->op.type != COW_POINTER_OP_MOVE)
			continue;
		struct cow_view *view = s->op.view;
		if (view == NULL || view->closed || view->output != output)
			continue;

		s->op.start_x += warp_dx;
		s->op.start_y += warp_dy;
	}
}

static void
pan_fire(struct cow_output *output, int edge)
{
	if (output == NULL || edge < 0)
		return;

	long now = cow_mono_ms();
	output->pan_last_fire_ms      = now;
	output->pan_giveup_until_ms   = now + PAN_GIVEUP_COOLDOWN_MS;
	cow_wm.pan_global_cooldown_ms = now + PAN_GIVEUP_COOLDOWN_MS;
	int cur_col = cow_page_col(output);
	int cur_row = cow_page_row(output);

	if (output->pan_entered[edge])
		pan_frame_paint(output, edge, false);

	/* Disarm before warping so the warp lands on the post-page page. */
	pan_disarm();
	output->pan_edge = -1;

	int ptr_x = output->x + output->width / 2;
	int ptr_y = output->y + output->height / 2;
	cow_seat_get_pointer(&ptr_x, &ptr_y);
	if (ptr_x < output->x)
		ptr_x = output->x;
	if (ptr_x >= output->x + output->width)
		ptr_x = output->x + output->width - 1;
	if (ptr_y < output->y)
		ptr_y = output->y;
	if (ptr_y >= output->y + output->height)
		ptr_y = output->y + output->height - 1;

	struct cow_view *drag = pan_find_drag_view();
	int		 warp_x, warp_y;
	pan_warp_target(output, edge, ptr_x, ptr_y, drag, &warp_x, &warp_y);

	cow_page_switch(output, cur_col + pan_edge_dir[edge].dcol,
	    cur_row + pan_edge_dir[edge].drow);

	cow_wm_request_pointer_warp(warp_x, warp_y);

	/* Pre-warp each seat's tracked pointer so the same-cycle
	 * pointer-op processor sees the post-warp position. */
	struct cow_seat *ws;
	wl_list_for_each(ws, &cow_wm.seats, wm_link)
	{
		ws->pointer_x = warp_x;
		ws->pointer_y = warp_y;
	}

	pan_adjust_drags(output, warp_x - ptr_x, warp_y - ptr_y);
}

void
cow_output_pan_frame_tick(struct cow_output *output)
{
	if (output == NULL || cow_config == NULL || !cow_config->edge_scroll)
		return;
	if (cow_wm.pan_lock_output != output || cow_wm.pan_lock_edge < 0)
		return;
	int edge = cow_wm.pan_lock_edge;
	if (!output->pan_entered[edge])
		return;

	long now     = cow_mono_ms();
	if (now < cow_wm.pan_global_cooldown_ms)
		return;
	long elapsed = now - output->pan_arm_ms;
	int  dwell   = pan_dwell_ms();
	int  level   = (elapsed * 5) / (dwell > 0 ? dwell : 1);
	if (level > 5)
		level = 5;
	pan_frame_set_level(output, edge, level);

	if (elapsed >= dwell)
		pan_fire(output, edge);
}

void
cow_output_pan_pointer_leave(int px, int py)
{
	if (cow_wm.wl_layer_shell != NULL)
		return;
	if (cow_config == NULL || !cow_config->edge_scroll)
		return;
	if (cow_config->page_cols <= 1 && cow_config->page_rows <= 1)
		return;

	struct cow_output *output;
	wl_list_for_each(output, &cow_wm.outputs, wm_link)
	{
		int edge = pan_edge_for_pointer(output, px, py,
		    /* strip */ 2);
		if (edge >= 0 && output->pan_edge != edge) {
			output->pan_edge	  = edge;
			output->pan_resist_motion = 0;
			output->pan_resist_done	  = false;
		} else if (edge < 0) {
			output->pan_edge = -1;
		}
	}
	cow_wm_request_manage_dirty();
}

void
cow_output_pan_pointer_enter(void)
{
	/* Layer-shell-unavailable fallback only. */
	if (cow_wm.wl_layer_shell != NULL)
		return;
	struct cow_output *output;
	wl_list_for_each(output, &cow_wm.outputs, wm_link)
	    output->pan_edge = -1;
	cow_wm_request_manage_dirty();
}

void
cow_pan_pointer_moved(int x, int y)
{
	(void)x;
	(void)y;
}

/* Drag-mode arming: when a window is being moved, the cursor's
 * position dwells at an edge → fire after edge_scroll_delay_ms.
 * Driven by pointer_op processing rather than wl_pointer events
 * because layer-shell pointer events don't fire during a drag. */
void
cow_output_pan_op_arm(struct cow_seat *seat)
{
	if (seat == NULL)
		return;
	if (cow_config == NULL || !cow_config->edge_scroll)
		return;
	if (cow_config->page_cols <= 1 && cow_config->page_rows <= 1)
		return;

	int px = seat->pointer_x;
	int py = seat->pointer_y;

	const int strip = 8;
	long	  now	= cow_mono_ms();

	struct cow_output *output;
	wl_list_for_each(output, &cow_wm.outputs, wm_link)
	{
		bool on_output = (px >= output->x &&
		    px < output->x + output->width && py >= output->y &&
		    py < output->y + output->height);
		if (!on_output) {
			if (output->pan_edge >= 0) {
				pan_frame_set_level(output, output->pan_edge, 0);
				output->pan_edge   = -1;
				output->pan_arm_ms = 0;
			}
			continue;
		}

		int edge = pan_edge_for_pointer(output, px, py, strip);

		if (edge < 0) {
			if (output->pan_edge >= 0) {
				pan_frame_set_level(output, output->pan_edge, 0);
				output->pan_edge   = -1;
				output->pan_arm_ms = 0;
			}
			continue;
		}

		if (output->pan_edge != edge) {
			if (output->pan_edge >= 0)
				pan_frame_set_level(output, output->pan_edge, 0);
			output->pan_edge   = edge;
			output->pan_arm_ms = now;
		}

		if (now < cow_wm.pan_global_cooldown_ms)
			continue;
		if (now < output->pan_giveup_until_ms)
			continue;

		long elapsed = now - output->pan_arm_ms;
		int  dwell   = pan_dwell_ms();
		int  level   = (elapsed * 5) / (dwell > 0 ? dwell : 1);
		if (level > 5)
			level = 5;
		pan_frame_set_level(output, edge, level);

		if (elapsed >= dwell) {
			pan_fire(output, edge);
			return;
		}
	}
}
