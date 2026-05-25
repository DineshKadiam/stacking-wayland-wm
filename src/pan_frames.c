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
#include "cow/memory.h"
#include "cow/output.h"
#include "cow/pan_frames.h"
#include "cow/pointer_op.h"
#include "cow/seat.h"
#include "cow/shm.h"
#include "cow/util.h"
#include "cow/view.h"
#include "cow/wm.h"
#include "pointer-constraints-unstable-v1-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

/* Thickness of each pan frame surface in pixels */
#define PAN_FRAME_SIZE 5

/* Colours */
#define PAN_COL_CLEAR 0x00000000u
#define PAN_COL_ORANGE 0xCCFF8800u

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
static void pan_confine_release(struct cow_output *output, int edge);

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

static void
pointer_enter(void *data, struct wl_pointer *pointer, uint32_t serial,
    struct wl_surface *surf, wl_fixed_t sx, wl_fixed_t sy)
{
	(void)data;
	(void)pointer;
	(void)serial;
	(void)sx;
	(void)sy;

	struct cow_output *output;
	int		   edge = surface_to_output_edge(surf, &output);
	if (edge < 0)
		return;

	output->pan_entered[edge] = true;
	/* Do NOT paint orange immediately — wait for the first tick after a
	 * short dwell so that fast cursor sweeps across the strip (e.g. when
	 * moving between monitors) don't produce a visible flash.
	 */

	/* Arm dwell timer */
	output->pan_edge     = edge;
	output->pan_enter_ms = cow_mono_ms();

	/* Confine the pointer to this strip so it can't cross the monitor
	 * boundary while the dwell timer is running.
	 */
	if (cow_wm.wl_pointer_constraints != NULL &&
	    output->pan_confined[edge] == NULL) {
		output->pan_confined[edge] =
		    zwp_pointer_constraints_v1_confine_pointer(
			cow_wm.wl_pointer_constraints, surf,
			cow_wm.wl_pointer_raw, NULL,
			ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_ONESHOT);
	}

	cow_wm_request_manage_dirty();
}

static void
pointer_leave(void *data, struct wl_pointer *pointer, uint32_t serial,
    struct wl_surface *surf)
{
	(void)data;
	(void)pointer;
	(void)serial;

	struct cow_output *output;
	int		   edge = surface_to_output_edge(surf, &output);
	if (edge < 0)
		return;

	output->pan_entered[edge] = false;
	pan_frame_paint(output, edge, false); /* back to transparent */

	/* Release confinement and cancel dwell */
	pan_confine_release(output, edge);
	output->pan_edge = -1;

	cow_wm_request_manage_dirty();
}

static void
pointer_motion(void *d, struct wl_pointer *p, uint32_t t, wl_fixed_t x,
    wl_fixed_t y)
{
	(void)d;
	(void)p;
	(void)t;
	(void)x;
	(void)y;
}

static void
pointer_button(void *d, struct wl_pointer *p, uint32_t ser, uint32_t t,
    uint32_t btn, uint32_t state)
{
	(void)d;
	(void)p;
	(void)ser;
	(void)t;
	(void)btn;
	(void)state;
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
pan_confine_release(struct cow_output *output, int edge)
{
	if (output->pan_confined[edge] != NULL) {
		zwp_confined_pointer_v1_destroy(output->pan_confined[edge]);
		output->pan_confined[edge] = NULL;
	}
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

static void
pan_frame_paint(struct cow_output *output, int edge, bool orange)
{
	if (output->pan_surf[edge] == NULL)
		return;

	struct cow_shm_buffer *buf = orange ? output->pan_buf_orange[edge] :
					      output->pan_buf_clear[edge];
	if (!buf)
		return;

	wl_surface_attach(output->pan_surf[edge], buf->buffer, 0, 0);
	wl_surface_damage_buffer(output->pan_surf[edge], 0, 0, buf->width,
	    buf->height);
	wl_surface_commit(output->pan_surf[edge]);
}

static void
pan_frame_destroy(struct cow_output *output, int edge)
{
	pan_confine_release(output, edge);
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

	/* When a page-scroll animation is in progress, use the target page
	 * position rather than the current mid-animation page_vx/vy.  This
	 * ensures pan frames are created/destroyed for the destination page
	 * rather than whichever intermediate position happens to be current
	 * when pan_frames_update_* is called. */
	int		 cur_col, cur_row;
	struct cow_desk *desk = (output->current_desk != NULL) ?
	    output->current_desk :
	    NULL;
	if (desk != NULL && desk->scrolling) {
		int w	= output->usable_area.width;
		int h	= output->usable_area.height;
		cur_col = (w > 0) ? desk->scroll_to_vx / w : 0;
		cur_row = (h > 0) ? desk->scroll_to_vy / h : 0;
	} else {
		cur_col = cow_page_col(output);
		cur_row = cow_page_row(output);
	}

	if (edge < 0 || edge > 3)
		return false;
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
	output->pan_confined[edge]   = NULL;

	/* ctx freed in pan_frame_destroy */
	struct pan_frame_ctx *ctx = xcalloc(1, sizeof(*ctx));
	ctx->output		  = output;
	ctx->edge		  = edge;
	output->pan_ctx[edge]	  = ctx;

	zwlr_layer_surface_v1_set_size(lsurf, iw, ih);
	zwlr_layer_surface_v1_set_anchor(lsurf, anchor);
	/* exclusive_zone=-1: ignore reserved areas from panels/bars so the
	 * pan frame is always at the raw output edge, not below a waybar.
	 */
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
	if (!cow_wm.wl_seat_raw)
		return;

	/* Only call wl_seat_get_pointer if the seat actually advertises the
	 * pointer capability.  Calling it on a seat without a pointer device
	 * (e.g. a headless/VNC session before a client connects) is a fatal
	 * Wayland protocol error that kills the connection. */
	if (cow_wm.wl_pointer_raw == NULL && cow_wm.wl_seat_has_pointer) {
		cow_wm.wl_pointer_raw = wl_seat_get_pointer(cow_wm.wl_seat_raw);
		if (cow_wm.wl_pointer_raw != NULL)
			wl_pointer_add_listener(cow_wm.wl_pointer_raw,
			    &pointer_listener, NULL);
	}
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
			/* Re-paint in case hover state changed */
			pan_frame_paint(output, e, output->pan_entered[e]);
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

/* ----------------------------------------------------------------------
 * Edge-driven dwell timer + warp.
 *
 * The functions below implement the per-output dwell timer, the
 * page-switch warp target, the drag-aware view-edge arming used by
 * pointer ops, and the cow_output_pan_pointer_{leave,enter} fallback
 * for compositors without zwlr_layer_shell_v1.  Originally lived in
 * output.c; consolidated here so all pan-frame code is in one
 * translation unit.
 * ---------------------------------------------------------------------- */

static int
pan_dwell_delay(const struct cow_output *output, long now_ms)
{
	int delay = cow_config->edge_scroll_delay_ms > 0 ?
	    cow_config->edge_scroll_delay_ms :
	    300;
	if (output->pan_last_fire_ms != 0 &&
	    now_ms - output->pan_last_fire_ms < (long)delay) {
		int rapid = delay / 5;
		if (rapid < 60)
			rapid = 60;
		delay = rapid;
	}
	return delay;
}

/* Find the view being dragged (if any).  Used to consider view-edge
 * arming during a drag.
 *
 * Excludes container drags (-C): during a containerise/uncontainerise
 * the source view's frame is dragged freely around the screen as the
 * user positions the cursor over a drop target.  The drag is the
 * user's hand placement, not a deliberate page approach -- using the
 * view's frame edges for arming there causes spurious page-switches
 * whenever the dragged frame happens to slide past a screen edge,
 * even with the pointer well inside the output.  Cursor-only arming
 * still works (handled by px/py checks alongside the view checks). */
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

/* Figure out which edge of 'output' the pointer (or, during a drag, the
 * view's frame) is touching.  Returns -1 for none, 0=top 1=bottom 2=left
 * 3=right. 'view_edge_ok' lets the caller suppress view-edge arming
 * (used during the post-page cooldown).
 *
 * FIXME: Use an enum for return value
 */
static int
pan_edge_for_pointer(const struct cow_output *output, int px, int py,
    const struct cow_view *drag, bool view_edge_ok, int strip)
{
	int cols    = cow_config->page_cols;
	int rows    = cow_config->page_rows;
	int cur_col = cow_page_col(output);
	int cur_row = cow_page_row(output);

	/*
	 * For a maximised (or fullscreen / shaded) view, the frame
	 * already spans the usable area by definition; using its edges
	 * for view-edge arming would arm a pan strip every manage cycle
	 * regardless of user intent.  Fall back to cursor-only arming
	 * for those cases.
	 */
	int  v_l = px, v_r = px, v_t = py, v_b = py;
	bool consider_view = drag != NULL && drag->output == output &&
	    !drag->maximized && !drag->fullscreen && drag->sd == COW_SHADE_NONE;
	if (consider_view)
		pan_view_frame(drag, &v_l, &v_t, &v_r, &v_b);
	if (rows <= 0 || cols <= 0)
		return (-1);

	unsigned mask = cow_pan_edge_mask(cur_col, cur_row, cols, rows);

	if ((mask & COW_PAN_EDGE_BIT(COW_PAN_TOP)) &&
	    (py <= output->y + strip ||
		(view_edge_ok && v_t <= output->y + strip)))
		return COW_PAN_TOP;
	if ((mask & COW_PAN_EDGE_BIT(COW_PAN_BOTTOM)) &&
	    (py >= output->y + output->height - strip ||
		(view_edge_ok && v_b >= output->y + output->height - strip)))
		return COW_PAN_BOTTOM;
	if ((mask & COW_PAN_EDGE_BIT(COW_PAN_LEFT)) &&
	    (px <= output->x + strip ||
		(view_edge_ok && v_l <= output->x + strip)))
		return COW_PAN_LEFT;
	if ((mask & COW_PAN_EDGE_BIT(COW_PAN_RIGHT)) &&
	    (px >= output->x + output->width - strip ||
		(view_edge_ok && v_r >= output->x + output->width - strip)))
		return COW_PAN_RIGHT;

	return -1;
}

/* Clear all four pan-frame orange indicators on this output.  Used when
 * we know none of them should be armed (cursor left the output, or we
 * just fired a page). */
static void
pan_clear_orange(struct cow_output *output)
{
	for (int e = 0; e < 4; e++) {
		if (output->pan_entered[e])
			pan_frame_set_orange(output, e, false);
	}
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

void
cow_output_pan_frame_tick(struct cow_output *output)
{
	if (output == NULL || cow_config == NULL || !cow_config->edge_scroll)
		return;
	if (output->pan_edge < 0)
		return;

	long now_ms  = cow_mono_ms();
	int  delay   = pan_dwell_delay(output, now_ms);
	long elapsed = now_ms - output->pan_enter_ms;

	/* Paint orange after 1/3 of the dwell (min 100ms) so a fast
	 * cursor sweep across the strip doesn't cause a visible flash. */
	int paint_threshold = delay / 3;
	if (paint_threshold < 100)
		paint_threshold = 100;
	if (elapsed >= paint_threshold &&
	    !output->pan_entered[output->pan_edge])
		pan_frame_set_orange(output, output->pan_edge, true);

	if (elapsed < delay)
		return;

	output->pan_last_fire_ms = now_ms;
	int edge		 = output->pan_edge;
	int cur_col		 = cow_page_col(output);
	int cur_row		 = cow_page_row(output);
	output->pan_edge	 = -1;

	if (output->pan_entered[edge])
		pan_frame_set_orange(output, edge, false);

	int ptr_x = output->x + output->width / 2;
	int ptr_y = output->y + output->height / 2;
	cow_seat_get_pointer(&ptr_x, &ptr_y);
	/* clamp perpendicular coords to this output */
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

	/*
	 * Optimistically pre-warp each seat's tracked pointer.  River
	 * delivers the matching pointer_position event next manage cycle,
	 * but cow_pointer_op_process runs LATER this cycle and needs the
	 * post-warp position so the dragged view's (pointer + offset)
	 * formula gives a consistent result.
	 */
	{
		struct cow_seat *ws;
		wl_list_for_each(ws, &cow_wm.seats, wm_link)
		{
			ws->pointer_x = warp_x;
			ws->pointer_y = warp_y;
		}
	}

	pan_adjust_drags(output, warp_x - ptr_x, warp_y - ptr_y);
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

	long		   now_ms = cow_mono_ms();
	struct cow_output *output;
	wl_list_for_each(output, &cow_wm.outputs, wm_link)
	{
		int edge = pan_edge_for_pointer(output, px, py, NULL, true,
		    /* strip */ 2);
		if (edge >= 0) {
			output->pan_edge     = edge;
			output->pan_enter_ms = now_ms;
		} else {
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
cow_output_pan_op_arm(int px, int py)
{
	if (cow_config == NULL || !cow_config->edge_scroll)
		return;
	if (cow_config->page_cols <= 1 && cow_config->page_rows <= 1)
		return;

	const int	 strip	  = 8;
	const int	 cooldown = 600;
	long		 now_ms	  = cow_mono_ms();
	struct cow_view *drag	  = pan_find_drag_view();

	struct cow_output *output;
	wl_list_for_each(output, &cow_wm.outputs, wm_link)
	{
		bool on_output = (px >= output->x &&
		    px < output->x + output->width && py >= output->y &&
		    py < output->y + output->height);
		if (!on_output) {
			/* Pointer is on a different output: disarm and
			 * clear any orange paint we may have left here.
			 * (River doesn't deliver pointer_leave to layer
			 * surfaces during an op.) */
			if (output->pan_edge >= 0)
				output->pan_edge = -1;
			pan_clear_orange(output);
			continue;
		}

		bool view_edge_ok = (output->pan_last_fire_ms == 0) ||
		    (now_ms - output->pan_last_fire_ms >= (long)cooldown);
		int edge = pan_edge_for_pointer(output, px, py, drag,
		    view_edge_ok, strip);

		if (edge >= 0 && output->pan_edge != edge) {
			/*
			 * Newly armed (or armed edge changed).  Clear any
			 * orange paint left on the previously-armed edge so
			 * we don't leave a stale stripe behind.
			 */
			if (output->pan_edge >= 0 &&
			    output->pan_entered[output->pan_edge])
				pan_frame_set_orange(output, output->pan_edge,
				    false);
			output->pan_edge     = edge;
			output->pan_enter_ms = now_ms;
		} else if (edge < 0 && output->pan_edge >= 0) {
			/* Pointer/view left the strip before dwell completed:
			 * disarm and clear any orange paint that may have been
			 * applied at the paint_threshold. */
			if (output->pan_entered[output->pan_edge])
				pan_frame_set_orange(output, output->pan_edge,
				    false);
			output->pan_edge = -1;
		}
	}
}
