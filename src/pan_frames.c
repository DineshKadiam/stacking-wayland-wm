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

/* Cooldown after a fire so the post-warp cursor doesn't re-arm. */
#define PAN_GIVEUP_COOLDOWN_MS 200

/* Map a wl_surface back to (output, edge) */
bool
cow_pan_frame_for_surface(struct wl_surface *surf,
    struct cow_output **out_output, int *out_edge)
{
	struct cow_output *output;
	wl_list_for_each(output, &cow_wm.outputs, wm_link)
	{
		for (int e = 0; e < 4; e++) {
			if (output->pan_surf[e] == surf) {
				*out_output = output;
				*out_edge = e;
				return (true);
			}
		}
	}
	return (false);
}

bool
cow_pan_frame_point(struct cow_output *output, int edge, int sx, int sy,
    int *gx, int *gy)
{
	if (output == NULL || edge < 0 || edge > 3)
		return (false);

	*gx = output->x + sx;
	*gy = output->y + sy;
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

void
cow_pan_frame_pointer_enter(struct cow_output *output, int edge)
{
	if (output == NULL || edge < 0 || edge > 3)
		return;

	output->pan_entered[edge] = true;
	long now = cow_mono_ms();
	if (now < output->pan_giveup_until_ms ||
	    now < cow_wm.pan_global_cooldown_ms)
		return;
	pan_arm_edge(output, edge);
}

void
cow_pan_frame_pointer_leave(struct cow_output *output, int edge)
{
	if (output == NULL || edge < 0 || edge > 3)
		return;

	output->pan_entered[edge] = false;
	if (cow_wm.pan_lock_output == output && cow_wm.pan_lock_edge == edge)
		pan_disarm();
}

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
		int w	= cow_page_width(output);
		int h	= cow_page_height(output);
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
