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
#include <time.h>
#include <wayland-client.h>

#include "cow/config.h"
#include "cow/desk_ops.h"
#include "cow/memory.h"
#include "cow/output.h"
#include "cow/pan_frames.h"
#include "cow/shm.h"
#include "cow/compat.h"
#include "cow/wm.h"
#include "pointer-constraints-unstable-v1-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

/* Thickness of each pan frame surface in pixels */
#define PAN_FRAME_SIZE 5

/* Colours */
#define PAN_COL_CLEAR 0x00000000u
#define PAN_COL_ORANGE 0xCCFF8800u

/* Index helpers */
#define PAN_TOP 0
#define PAN_BOTTOM 1
#define PAN_LEFT 2
#define PAN_RIGHT 3

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
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	output->pan_edge     = edge;
	output->pan_enter_ms = ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;

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

	cow_wm.needs_manage_dirty = true;
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

	cow_wm.needs_manage_dirty = true;
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
	int cols    = cow_config->page_cols;
	int rows    = cow_config->page_rows;
	int cur_col = cow_page_col(output);
	int cur_row = cow_page_row(output);

	switch (edge) {
	case PAN_TOP:
		return rows > 1 && cur_row > 0;
	case PAN_BOTTOM:
		return rows > 1 && cur_row < rows - 1;
	case PAN_LEFT:
		return cols > 1 && cur_col > 0;
	case PAN_RIGHT:
		return cols > 1 && cur_col < cols - 1;
	}
	return false;
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

	/* Get a wl_pointer directly from the seat.  We skip the seat
	 * capabilities event because wl_seat_add_listener() silently fails
	 * if the proxy already has a listener (e.g. from a prior roundtrip),
	 * leaving wl_pointer_raw NULL.  Calling get_pointer() directly is
	 * safe; if the seat has no pointer the compositor will send an error
	 * which we will see as a protocol error (acceptable).
	 */
	if (cow_wm.wl_pointer_raw == NULL) {
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
