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
#include "cow/output.h"
#include <time.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cow/colour.h"
#include "cow/compat.h"
#include "cow/config.h"
#include "cow/layer_shell.h"
#include "cow/memory.h"
#include "cow/shm.h"
#include "cow/wm.h"
#include "cow/desk_ops.h"
#include "cow/pan_frames.h"
#include "cow/seat.h"
#include "cow/view.h"

#include "river-window-management-v1-client-protocol.h"

/*
 * Recompute the usable area from the output dimensions (or layer shell
 * non_exclusive_area), subtracting the configured reserved area.
 */
void
cow_output_apply_reserved_area(struct cow_output *output)
{
	if (cow_config == NULL)
		return;

	int rt = cow_config->reserved.top;
	int rb = cow_config->reserved.bottom;
	int rl = cow_config->reserved.left;
	int rr = cow_config->reserved.right;

	if (rt == 0 && rb == 0 && rl == 0 && rr == 0)
		return;

	output->usable_area.x += rl;
	output->usable_area.y += rt;
	output->usable_area.width -= rl + rr;
	output->usable_area.height -= rt + rb;

	if (output->usable_area.width < 100)
		output->usable_area.width = 100;
	if (output->usable_area.height < 100)
		output->usable_area.height = 100;
}

/*
 * river_output_v1 event handlers
 */

static void
output_handle_removed(void *data, __unused struct river_output_v1 *river_output)
{
	struct cow_output *output = data;

	fprintf(stderr, "cow: output removed\n");
	output->removed = true;
}

static void
output_handle_wl_output(void	    *data,
    __unused struct river_output_v1 *river_output, uint32_t name)
{
	struct cow_output *output = data;
	output->wl_output_name	  = name;

	/* Try to match a bound wl_output to get the human-readable name */
	struct cow_wl_output_entry *entry;
	wl_list_for_each(entry, &cow_wm.wl_outputs, link)
	{
		if (entry->global_name == name) {
			output->wl_output = entry->wl_output;
			if (entry->name != NULL && output->name == NULL)
				output->name = xstrdup(entry->name);
			break;
		}
	}

	/* wl_output is now known — try to create pan frames if dimensions
	 * are also already set and the config has finished loading. */
	if (!cow_wm.exec_pending && output->wl_output != NULL &&
	    output->width > 0 && output->height > 0)
		pan_frames_update_output(output);
}

static void
output_handle_position(void	    *data,
    __unused struct river_output_v1 *river_output, int32_t x, int32_t y)
{
	struct cow_output *output  = data;
	output->x		   = x;
	output->y		   = y;
	output->usable_area.x	   = x;
	output->usable_area.y	   = y;
	output->usable_area.width  = output->width;
	output->usable_area.height = output->height;
	cow_output_apply_reserved_area(output);
}

static void
output_handle_dimensions(void	    *data,
    __unused struct river_output_v1 *river_output, int32_t width,
    int32_t height)
{
	struct cow_output *output  = data;
	output->width		   = width;
	output->height		   = height;
	output->usable_area.x	   = output->x;
	output->usable_area.y	   = output->y;
	output->usable_area.width  = width;
	output->usable_area.height = height;

	/* At runtime (after startup) apply reserved immediately.
	 * During startup this is skipped — wm.c applies reserved once
	 * after the second roundtrip when all initial events have settled. */
	if (!cow_wm.exec_pending)
		cow_output_apply_reserved_area(output);

	/* If config has already been loaded, (re-)create pan frames now that
	 * we have valid dimensions.  During startup this is skipped and
	 * pan_frames_update_all() is called once at end of exec_pending. */
	if (!cow_wm.exec_pending)
		pan_frames_update_output(output);
}

static const struct river_output_v1_listener output_listener = {
	.removed    = output_handle_removed,
	.wl_output  = output_handle_wl_output,
	.position   = output_handle_position,
	.dimensions = output_handle_dimensions,
};

struct cow_output *
cow_output_create(struct river_output_v1 *river_output)
{
	struct cow_output *output = xcalloc(1, sizeof(*output));

	output->river_output = river_output;
	output->removed	     = false;
	output->pan_edge     = -1;
	wl_list_init(&output->visible_views);

	/* Initialize sheets */
	for (int i = 0; i < COW_NR_OF_DESKS; i++) {
		output->desks[i].nr	= i;
		output->desks[i].output = output;
		wl_list_init(&output->desks[i].views);
	}

	output->current_desk = &output->desks[0];

	river_output_v1_add_listener(river_output, &output_listener, output);

	wl_list_insert(&cow_wm.outputs, &output->wm_link);

	/* Set as current output if we don't have one yet */
	if (cow_wm.current_output == NULL) {
		cow_wm.current_output = output;
	}

	/* Register with layer shell for non_exclusive_area events */
	cow_layer_shell_init_output(output);

	return output;
}

void
cow_output_destroy(struct cow_output *output)
{
	pan_frames_destroy_output(output);
	cow_layer_shell_fini_output(output);
	wl_list_remove(&output->wm_link);

	if (cow_wm.current_output == output) {
		if (!wl_list_empty(&cow_wm.outputs)) {
			cow_wm.current_output =
			    wl_container_of(cow_wm.outputs.next,
				cow_wm.current_output, wm_link);
		} else {
			cow_wm.current_output = NULL;
		}
	}

	/* Background shell surface resources */
	if (output->bg_buffer != NULL)
		cow_shm_buffer_destroy(output->bg_buffer);
	if (output->bg_node != NULL)
		river_node_v1_destroy(output->bg_node);
	if (output->bg_shell_surface != NULL)
		river_shell_surface_v1_destroy(output->bg_shell_surface);
	if (output->bg_surface != NULL)
		wl_surface_destroy(output->bg_surface);

	xfree(output->name);
	river_output_v1_destroy(output->river_output);
	xfree(output);
}

void
cow_output_prepare_background(struct cow_output *output)
{
	if (cow_config == NULL || !cow_config->has_output_bg_colour)
		return;
	if (output->width <= 0 || output->height <= 0)
		return;

	/* Create wl_surface + shell_surface during manage sequence */
	if (output->bg_surface == NULL) {
		output->bg_surface = wl_compositor_create_surface(
		    cow_wm.compositor);
		if (output->bg_surface == NULL)
			return;
	}

	if (output->bg_shell_surface == NULL) {
		output->bg_shell_surface =
		    river_window_manager_v1_get_shell_surface(cow_wm.manager,
			output->bg_surface);
		if (output->bg_shell_surface == NULL)
			return;
	}

	if (output->bg_node == NULL) {
		output->bg_node = river_shell_surface_v1_get_node(
		    output->bg_shell_surface);
	}
}

void
cow_output_render_background(struct cow_output *output)
{
	/* Use per-output colour if set, otherwise fall back to global */
	bool has_colour = output->has_bg_colour ||
	    (cow_config != NULL && cow_config->has_output_bg_colour);
	if (!has_colour)
		return;
	if (output->bg_node == NULL || output->bg_surface == NULL ||
	    output->bg_shell_surface == NULL)
		return;
	if (output->width <= 0 || output->height <= 0)
		return;

	/* Only re-render if dirty or dimensions changed */
	bool needs_render = output->bg_buffer == NULL || output->bg_dirty ||
	    output->bg_buffer->width != output->width ||
	    output->bg_buffer->height != output->height;

	if (needs_render) {
		/* Free old buffer if wrong size or busy */
		if (output->bg_buffer != NULL &&
		    (output->bg_buffer->width != output->width ||
			output->bg_buffer->height != output->height ||
			output->bg_buffer->busy)) {
			cow_shm_buffer_destroy(output->bg_buffer);
			output->bg_buffer = NULL;
		}

		if (output->bg_buffer == NULL) {
			output->bg_buffer = cow_shm_buffer_create(output->width,
			    output->height);
			if (output->bg_buffer == NULL)
				return;
		}

		/* Fill with background colour (per-output overrides global) */
		const float *bg	       = output->has_bg_colour ?
			   output->bg_colour :
			   cow_config->output_bg_colour;
		uint32_t     px	       = cow_colour_to_pixel(bg);
		int	     stride_px = output->bg_buffer->stride / 4;
		uint32_t    *data      = output->bg_buffer->data;
		for (int y = 0; y < output->height; y++) {
			for (int x = 0; x < output->width; x++) {
				data[y * stride_px + x] = px;
			}
		}
		output->bg_dirty = false;
	}

	if (output->bg_buffer == NULL)
		return;

	/* Position at output origin, place at bottom */
	river_node_v1_set_position(output->bg_node, output->x, output->y);
	river_node_v1_place_bottom(output->bg_node);

	/* Commit the surface */
	wl_surface_attach(output->bg_surface, output->bg_buffer->buffer, 0, 0);
	wl_surface_damage_buffer(output->bg_surface, 0, 0, output->width,
	    output->height);
	river_shell_surface_v1_sync_next_commit(output->bg_shell_surface);
	wl_surface_commit(output->bg_surface);
	output->bg_buffer->busy = true;
}

struct cow_output *
cow_output_at(int x, int y)
{
	struct cow_output *output;
	wl_list_for_each(output, &cow_wm.outputs, wm_link)
	{
		if (x >= output->x && x < output->x + output->width &&
		    y >= output->y && y < output->y + output->height) {
			return output;
		}
	}
	return cow_wm.current_output;
}

struct cow_output *
cow_output_find(struct river_output_v1 *river_output)
{
	struct cow_output *output;
	wl_list_for_each(output, &cow_wm.outputs, wm_link)
	{
		if (output->river_output == river_output)
			return output;
	}
	return NULL;
}

struct cow_output *
cow_output_find_by_name(const char *name)
{
	if (name == NULL)
		return NULL;

	struct cow_output *output;
	wl_list_for_each(output, &cow_wm.outputs, wm_link)
	{
		if (output->name != NULL && strcmp(output->name, name) == 0)
			return output;
	}
	return NULL;
}

void
cow_output_update_current(void)
{
	/* TODO -- make this a setting?  To either say the current output is
	 * based on focused window, or pointer?
	 *
	 * For now, assume pointer, not focused window.
	 */

	if (!cow_wm.running)
		return;

	if (wl_list_empty(&cow_wm.seats))
		return;

	struct cow_seat *ps;
	wl_list_for_each(ps, &cow_wm.seats, wm_link)
	{
		if (ps->has_pointer_position) {
			struct cow_output *this = cow_output_at(ps->pointer_x,
			    ps->pointer_y);
			if (this != NULL && this != cow_wm.current_output)
				cow_wm.current_output = this;
		}
	}
}

static long
mono_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static int
pan_dwell_delay(const struct cow_output *output, long now_ms)
{
	int delay = cow_config->edge_scroll_delay_ms > 0 ?
	    cow_config->edge_scroll_delay_ms : 300;
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
 * arming during a drag. */
static struct cow_view *
pan_find_drag_view(void)
{
	struct cow_seat *s;
	wl_list_for_each(s, &cow_wm.seats, wm_link)
	{
		if (s->op_in_progress && s->op.type == COW_POINTER_OP_MOVE &&
		    s->op.view != NULL && !s->op.view->closed)
			return s->op.view;
	}
	return NULL;
}

static bool
pan_pointer_position(int *out_x, int *out_y)
{
	struct cow_seat *s;
	wl_list_for_each(s, &cow_wm.seats, wm_link)
	{
		if (s->has_pointer_position) {
			*out_x = s->pointer_x;
			*out_y = s->pointer_y;
			return true;
		}
	}
	return false;
}

/* Compute view's frame edges (incl. border + titlebar) in global coords. */
static void
pan_view_frame(const struct cow_view *v, int *l, int *t, int *r, int *b)
{
	int bw = v->dec.border_width;
	int tt = v->dec.title_thickness;
	*l     = v->x - bw;
	*t     = v->y - bw - tt;
	*r     = v->x + v->content_width + bw;
	*b     = v->y + v->content_height + bw;
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

	int v_l = px, v_r = px, v_t = py, v_b = py;
	if (drag != NULL && drag->output == output)
		pan_view_frame(drag, &v_l, &v_t, &v_r, &v_b);
	if (rows <= 0 || cols <= 0)
		return (-1);

	if (cur_row > 0 &&
	    (py <= output->y + strip ||
		(view_edge_ok && v_t <= output->y + strip)))
		return 0;
	if (cur_row < rows - 1 &&
	    (py >= output->y + output->height - strip ||
		(view_edge_ok &&
		    v_b >= output->y + output->height - strip)))
		return 1;
	if (cur_col > 0 &&
	    (px <= output->x + strip ||
		(view_edge_ok && v_l <= output->x + strip)))
		return 2;
	if (cur_col < cols - 1 &&
	    (px >= output->x + output->width - strip ||
		(view_edge_ok &&
		    v_r >= output->x + output->width - strip)))
		return 3;

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

	long now_ms  = mono_ms();
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
	pan_pointer_position(&ptr_x, &ptr_y);
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

	cow_wm.pending_warp = true;
	cow_wm.warp_x	    = warp_x;
	cow_wm.warp_y	    = warp_y;

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

	long		   now_ms = mono_ms();
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
	cow_wm.needs_manage_dirty = true;
}

void
cow_output_pan_pointer_enter(void)
{
	/* Layer-shell-unavailable fallback only. */
	if (cow_wm.wl_layer_shell != NULL)
		return;
	struct cow_output *output;
	wl_list_for_each(output, &cow_wm.outputs, wm_link)
	    output->pan_edge	  = -1;
	cow_wm.needs_manage_dirty = true;
}

void
cow_output_pan_op_arm(int px, int py)
{
	if (cow_config == NULL || !cow_config->edge_scroll)
		return;
	if (cow_config->page_cols <= 1 && cow_config->page_rows <= 1)
		return;

	const int strip = 8;
	const int	 cooldown = 600;
	long		 now_ms	  = mono_ms();
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
