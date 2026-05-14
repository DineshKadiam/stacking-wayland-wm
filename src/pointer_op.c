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
#include "cow/pointer_op.h"

#include <stdlib.h>

#include "cow/compat.h"
#include "cow/config.h"
#include "cow/desk_ops.h"
#include "cow/output.h"
#include "cow/pan_frames.h"
#include "cow/rule.h"
#include "cow/seat.h"
#include "cow/status.h"
#include "cow/view.h"
#include "cow/wm.h"

#include "river-window-management-v1-client-protocol.h"

/*
 * Snap a coordinate to nearby edges.  Checks usable area boundaries,
 * output boundaries, and other visible window frame edges.
 *
 * frame_left/top/right/bottom are the frame edges of the window
 * being moved/resized.  We snap each edge independently.
 */
/*
 * Update (*best_d, *snap_pos) with 'candidate' if it is closer to
 * 'edge_pos' than the current best.  Used by snap_edges below.
 */
static inline void
try_snap(int edge_pos, int candidate, int *best_d, int *snap_pos)
{
	int d = abs(edge_pos - candidate);
	if (d < *best_d) {
		*best_d	  = d;
		*snap_pos = candidate;
	}
}

/* Try to snap each frame edge against one rectangle's left/top/right/
 * bottom edges. */
static inline void
snap_edges_against_rect(int fl, int ft, int fr, int fb, int rl, int rt, int rr,
    int rb, int *best_dl, int *best_dt, int *best_dr, int *best_db, int *snap_l,
    int *snap_t, int *snap_r, int *snap_b)
{
	try_snap(fl, rl, best_dl, snap_l);
	try_snap(fr, rr, best_dr, snap_r);
	try_snap(ft, rt, best_dt, snap_t);
	try_snap(fb, rb, best_db, snap_b);
}

static void
snap_edges(struct cow_view *view, int *frame_left, int *frame_top,
    int *frame_right, int *frame_bottom, bool snap_size)
{
	int sd = cow_config ? cow_config->snap_distance : 0; /* snap off */
	if (sd <= 0)
		return;

	int fw = *frame_right - *frame_left;
	int fh = *frame_bottom - *frame_top;
	int fl = *frame_left, ft = *frame_top;
	int fr = *frame_right, fb = *frame_bottom;

	/* Collect snap candidates for each edge */
	int best_dl = sd + 1, best_dr = sd + 1;
	int best_dt = sd + 1, best_db = sd + 1;
	int snap_l = fl, snap_r = fr;
	int snap_t = ft, snap_b = fb;

	/* Check all output boundaries and usable areas */
	struct cow_output *out;
	wl_list_for_each(out, &cow_wm.outputs, wm_link)
	{
		snap_edges_against_rect(fl, ft, fr, fb, out->x, out->y,
		    out->x + out->width, out->y + out->height, &best_dl,
		    &best_dt, &best_dr, &best_db, &snap_l, &snap_t, &snap_r,
		    &snap_b);

		struct cow_box *ua = &out->usable_area;
		snap_edges_against_rect(fl, ft, fr, fb, ua->x, ua->y,
		    ua->x + ua->width, ua->y + ua->height, &best_dl, &best_dt,
		    &best_dr, &best_db, &snap_l, &snap_t, &snap_r, &snap_b);
	}

	/* Check other visible window edges.  In addition to per-edge
	 * snapping, allow our left to snap to their right and our right
	 * to their left (and top/bottom analogues), so windows align
	 * touching their neighbours. */
	struct cow_view *other;
	wl_list_for_each(other, &cow_wm.views, wm_link)
	{
		if (other == view || !other->has_dimensions)
			continue;
		if (!cow_view_visible(other, view->output))
			continue;

		struct cow_box of  = cow_view_frame_box(other);
		int	       ol  = of.x;
		int	       ot  = of.y;
		int	       or_ = of.x + of.width;
		int	       ob  = of.y + of.height;

		try_snap(fl, or_, &best_dl, &snap_l);
		try_snap(fl, ol, &best_dl, &snap_l);
		try_snap(fr, ol, &best_dr, &snap_r);
		try_snap(fr, or_, &best_dr, &snap_r);
		try_snap(ft, ob, &best_dt, &snap_t);
		try_snap(ft, ot, &best_dt, &snap_t);
		try_snap(fb, ot, &best_db, &snap_b);
		try_snap(fb, ob, &best_db, &snap_b);
	}

	/* Apply snaps */
	if (snap_size) {
		/* Resize: snap individual edges */
		if (best_dl <= sd)
			*frame_left = snap_l;
		if (best_dr <= sd)
			*frame_right = snap_r;
		if (best_dt <= sd)
			*frame_top = snap_t;
		if (best_db <= sd)
			*frame_bottom = snap_b;
	} else {
		/* Move: snap the whole window (prefer closest edge) */
		if (best_dl <= sd && best_dl <= best_dr) {
			*frame_left  = snap_l;
			*frame_right = snap_l + fw;
		} else if (best_dr <= sd) {
			*frame_right = snap_r;
			*frame_left  = snap_r - fw;
		}
		if (best_dt <= sd && best_dt <= best_db) {
			*frame_top    = snap_t;
			*frame_bottom = snap_t + fh;
		} else if (best_db <= sd) {
			*frame_bottom = snap_b;
			*frame_top    = snap_b - fh;
		}
	}
}

void
cow_pointer_op_start_move(struct cow_seat *seat, struct cow_view *view)
{
	if (view == NULL || view->closed || seat->op_in_progress)
		return;

	seat->op.type	 = COW_POINTER_OP_MOVE;
	seat->op.view	 = view;
	seat->op.start_x = view->x;
	seat->op.start_y = view->y;
	/*
	 * Capture the offset from the pointer to the view's top-left.
	 * This stays constant for the duration of the move op (the
	 * window follows the cursor rigidly), and lets us compute
	 * view->x = pointer_x + offset_x without going through op_dx,
	 * which can lag pointer_position by one manage cycle across a
	 * server-initiated warp.
	 */
	if (seat->has_pointer_position) {
		seat->op.move_offset_x = view->x - seat->pointer_x;
		seat->op.move_offset_y = view->y - seat->pointer_y;
	} else {
		seat->op.move_offset_x = 0;
		seat->op.move_offset_y = 0;
	}
	seat->op_in_progress = true;
	seat->op_released    = false;
	seat->op_dx	     = 0;
	seat->op_dy	     = 0;

	river_seat_v1_op_start_pointer(seat->river_seat);
}

void
cow_pointer_op_start_resize(struct cow_seat *seat, struct cow_view *view,
    uint32_t edges)
{
	if (view == NULL || view->closed || seat->op_in_progress)
		return;

	seat->op.type	      = COW_POINTER_OP_RESIZE;
	seat->op.view	      = view;
	seat->op.start_x      = view->x;
	seat->op.start_y      = view->y;
	seat->op.start_width  = view->content_width;
	seat->op.start_height = view->content_height;
	seat->op.resize_edges = edges;
	seat->op_in_progress  = true;
	seat->op_released     = false;
	seat->op_dx	      = 0;
	seat->op_dy	      = 0;

	river_window_v1_inform_resize_start(view->river_window);
	river_seat_v1_op_start_pointer(seat->river_seat);
}

/*
 * Constrain (*new_x, *new_y) so the dragged view's frame doesn't slide off
 * the screen edge when no further page exists in the direction of travel.
 */
static void
clamp_drag_to_screen(const struct cow_view *view, int bw, int tt, int *new_x,
    int *new_y)
{
	if (view->output == NULL || cow_config == NULL)
		return;

	int cols    = cow_config->page_cols;
	int rows    = cow_config->page_rows;
	int cur_col = cow_page_col(view->output);
	int cur_row = cow_page_row(view->output);
	int ox	    = view->output->x;
	int oy	    = view->output->y;
	int ow	    = view->output->width;
	int oh	    = view->output->height;

	struct cow_box nfb = cow_view_frame_box_at(view, *new_x, *new_y);
	struct cow_box cfb = cow_view_frame_box(view);

	unsigned mask	    = cow_pan_edge_mask(cur_col, cur_row, cols, rows);
	bool	 can_left   = (mask & COW_PAN_EDGE_BIT(COW_PAN_LEFT)) != 0;
	bool	 can_right  = (mask & COW_PAN_EDGE_BIT(COW_PAN_RIGHT)) != 0;
	bool	 can_top    = (mask & COW_PAN_EDGE_BIT(COW_PAN_TOP)) != 0;
	bool	 can_bottom = (mask & COW_PAN_EDGE_BIT(COW_PAN_BOTTOM)) != 0;

	if (!can_left && nfb.x < cfb.x && nfb.x < ox)
		*new_x = ox + bw;
	if (!can_right && (nfb.x + nfb.width) > (cfb.x + cfb.width) &&
	    (nfb.x + nfb.width) > ox + ow)
		*new_x = ox + ow - view->content_width - bw;
	if (!can_top && nfb.y < cfb.y && nfb.y < oy)
		*new_y = oy + bw + tt;
	if (!can_bottom && (nfb.y + nfb.height) > (cfb.y + cfb.height) &&
	    (nfb.y + nfb.height) > oy + oh)
		*new_y = oy + oh - view->content_height - bw;
}

/*
 * Finalise a pointer op when seat->op_released is set.  Handles
 * resize-end notification, container-drag completion (uncontainerise,
 * containerise, or plain drop), drag-ghost teardown, output reassignment,
 * titlebar-press visuals, and the final river op_end + state reset.
 *
 * Split out of cow_pointer_op_process so the dispatcher reads as
 * "process op delta; finalise if released".
 */
static void
op_finalize(struct cow_seat *seat, struct cow_view *view)
{
	if (seat->op.type == COW_POINTER_OP_RESIZE)
		river_window_v1_inform_resize_end(view->river_window);

	/*
	 * Container drag (-C) completion.
	 *
	 * Source already in a container: always detach, regardless of
	 * where the pointer lands.  No target highlight needed.
	 *
	 * Source NOT in a container: group with whatever window is
	 * under the pointer (if any).
	 */
	if (seat->container_drag && seat->op.type == COW_POINTER_OP_MOVE &&
	    view->container_member != NULL) {
		/* Uncontainerise: detach, restore pre-container size,
		 * place at pointer */
		if (cow_wm.drag_drop_target != NULL) {
			cow_wm.drag_drop_target->dec.dirty = true;
			cow_wm.drag_drop_target		   = NULL;
		}
		int bw = view->dec.border_width;
		int tt = view->dec.title_thickness;
		/* cow_container_remove_view restores pre-container
		 * dimensions */
		cow_container_remove_view(view);
		/* Use restored (proposed) size for centring on pointer */
		int fw = (view->proposed_width > 0 ? view->proposed_width :
						     view->content_width) +
		    2 * bw;
		int fh = (view->proposed_height > 0 ? view->proposed_height :
						      view->content_height) +
		    bw + tt + bw;
		view->x		= seat->pointer_x - fw / 2 + bw;
		view->y		= seat->pointer_y - fh / 2 + bw + tt;
		view->dec.dirty = true;
		cow_wm_request_manage_dirty();
	} else if (seat->container_drag &&
	    seat->op.type == COW_POINTER_OP_MOVE &&
	    cow_wm.drag_drop_target != NULL &&
	    !cow_wm.drag_drop_target->closed &&
	    cow_wm.drag_drop_target != view) {
		/* Containerise: group source with target */
		struct cow_view *target = cow_wm.drag_drop_target;
		cow_wm.drag_drop_target = NULL;
		target->dec.dirty	= true;
		if (target->container_member != NULL)
			cow_container_add(target->container_member->container,
			    view);
		else
			cow_container_create(target, view);
	} else if (cow_wm.drag_drop_target != NULL) {
		/* Normal drop: clear any lingering indicator */
		cow_wm.drag_drop_target->dec.dirty = true;
		cow_wm.drag_drop_target		   = NULL;
	}
	seat->container_drag	= false;
	cow_wm.drag_drop_source = NULL;
	cow_drag_ghost_destroy();

	/* After move/resize, update the view's output assignment based
	 * on where the window ended up. */
	cow_view_update_output(view);

	/* Restore titlebar visuals if this was a title drag */
	if (seat->titlebar_press == COW_TITLEBAR_PRESS_TITLE &&
	    seat->pressed_view != NULL && !seat->pressed_view->closed) {
		seat->pressed_view->title_pressed = false;
		seat->pressed_view->dec.dirty	  = true;
	}
	seat->titlebar_press = COW_TITLEBAR_PRESS_NONE;
	seat->pressed_view   = NULL;

	enum cow_pointer_op_type finished_type = seat->op.type;

	river_seat_v1_op_end(seat->river_seat);
	seat->op_in_progress = false;
	seat->op_released    = false;
	seat->op.type	     = COW_POINTER_OP_NONE;

	/* Fire lifecycle rules for the completed op.  Skipped if the
	 * view was destroyed during the op (caller has already returned
	 * before reaching here). */
	if (view != NULL && !view->closed) {
		if (finished_type == COW_POINTER_OP_MOVE)
			cow_rule_fire(COW_RULE_ON_MOVE, view, view->desk,
			    view->output);
		else if (finished_type == COW_POINTER_OP_RESIZE)
			cow_rule_fire(COW_RULE_ON_RESIZE, view, view->desk,
			    view->output);
	}
}

void
cow_pointer_op_process(struct cow_seat *seat)
{
	if (!seat->op_in_progress)
		return;

	struct cow_view *view = seat->op.view;
	if (view == NULL || view->closed) {
		/* View was destroyed during the op, end it */
		river_seat_v1_op_end(seat->river_seat);
		seat->op_in_progress = false;
		seat->op.type	     = COW_POINTER_OP_NONE;
		return;
	}

	/*
	 * Pan-frame edge detection during a drag/resize.  River does not
	 * deliver pointer events to layer surfaces while a pointer-op is
	 * in progress, so the layer-surface pointer_enter listener never
	 * fires.  Use the seat's tracked pointer position to arm pan_edge
	 * by proximity instead.
	 */
	if (seat->has_pointer_position)
		cow_output_pan_op_arm(seat->pointer_x, seat->pointer_y);

	int dx = seat->op_dx;
	int dy = seat->op_dy;

	switch (seat->op.type) {
	case COW_POINTER_OP_MOVE: {
		int bw = view->dec.border_width;
		int tt = view->dec.title_thickness;

		/*
		 * Compute the view's new top-left from the cursor position
		 * and the captured offset.  Falling back to start + op_d
		 * when we don't yet have a pointer position (shouldn't
		 * happen during an op, but defensive).
		 */
		int cx, cy;
		if (seat->has_pointer_position) {
			cx = seat->pointer_x + seat->op.move_offset_x;
			cy = seat->pointer_y + seat->op.move_offset_y;
		} else {
			cx = seat->op.start_x + dx;
			cy = seat->op.start_y + dy;
		}
		int fl = cx - bw;
		int ft = cy - bw - tt;
		int fr = cx + view->content_width + bw;
		int fb = cy + view->content_height + bw;
		snap_edges(view, &fl, &ft, &fr, &fb, false);

		if (seat->container_drag) {
			/* Container drag (-C): keep source in place, move ghost
			 */
			cow_wm.drag_ghost_x = seat->pointer_x;
			cow_wm.drag_ghost_y = seat->pointer_y;
		} else {
			/* Normal move */
			int new_x = fl + bw;
			int new_y = ft + bw + tt;

			clamp_drag_to_screen(view, bw, tt, &new_x, &new_y);

			view->x = new_x;
			view->y = new_y;

			/*
			 * Use the cursor position (not the view centre) to
			 * pick the output.  The cursor represents the user's
			 * intent; the view geometry may straddle output
			 * boundaries mid-drag (especially right after a
			 * page-cross warp where the view is wider than the
			 * warp margin), and reassigning by centre would flip
			 * the view to a different output and remap its
			 * vx/vy into a coordinate frame the user did not
			 * intend.
			 */
			if (seat->has_pointer_position) {
				struct cow_output *target = cow_output_at(
				    seat->pointer_x, seat->pointer_y);
				if (target != NULL && target != view->output)
					cow_view_reassign_output(view, target);
			} else {
				cow_view_update_output(view);
			}
			cow_view_sync_vxy_from_global(view);
			cow_view_sync_container_pos(view);
		}
		break;
	}

	case COW_POINTER_OP_RESIZE: {
		uint32_t edges = seat->op.resize_edges;
		int	 new_w = seat->op.start_width;
		int	 new_h = seat->op.start_height;
		int	 new_x = seat->op.start_x;
		int	 new_y = seat->op.start_y;

		if (edges & COW_EDGE_RIGHT)
			new_w += dx;
		if (edges & COW_EDGE_LEFT) {
			new_w -= dx;
			new_x += dx;
		}
		if (edges & COW_EDGE_BOTTOM)
			new_h += dy;
		if (edges & COW_EDGE_TOP) {
			new_h -= dy;
			new_y += dy;
		}

		/* Enforce minimum size */
		if (new_w < 50) {
			if (edges & COW_EDGE_LEFT)
				new_x -= (50 - new_w);
			new_w = 50;
		}
		if (new_h < 50) {
			if (edges & COW_EDGE_TOP)
				new_y -= (50 - new_h);
			new_h = 50;
		}

		/* Respect dimension hints */
		if (view->min_width > 0 && new_w < view->min_width)
			new_w = view->min_width;
		if (view->min_height > 0 && new_h < view->min_height)
			new_h = view->min_height;
		if (view->max_width > 0 && new_w > view->max_width)
			new_w = view->max_width;
		if (view->max_height > 0 && new_h > view->max_height)
			new_h = view->max_height;

		/* Snap frame edges during resize */
		{
			int rbw = view->dec.border_width;
			int rtt = view->dec.title_thickness;
			int fl	= new_x - rbw;
			int ft	= new_y - rbw - rtt;
			int fr	= new_x + new_w + rbw;
			int fb	= new_y + new_h + rbw;
			snap_edges(view, &fl, &ft, &fr, &fb, true);
			if (edges & COW_EDGE_LEFT) {
				new_x = fl + rbw;
				new_w = fr - fl - 2 * rbw;
			}
			if (edges & COW_EDGE_RIGHT)
				new_w = fr - fl - 2 * rbw;
			if (edges & COW_EDGE_TOP) {
				new_y = ft + rbw + rtt;
				new_h = fb - ft - rbw - rtt - rbw;
			}
			if (edges & COW_EDGE_BOTTOM)
				new_h = fb - ft - rbw - rtt - rbw;
			if (new_w < 50)
				new_w = 50;
			if (new_h < 50)
				new_h = 50;
		}

		view->x		      = new_x;
		view->y		      = new_y;
		view->proposed_width  = new_w;
		view->proposed_height = new_h;
		river_window_v1_propose_dimensions(view->river_window, new_w,
		    new_h);

		/*
		 * Sync container position and dimensions so that
		 * river_node_v1_set_position uses the updated values and lx/ly
		 * hit-detection remains correct.  Use the already-computed
		 * new_x/y/w/h directly because view->content_width/height
		 * haven't been updated yet (they're updated when the dimensions
		 * event arrives).
		 */
		cow_view_sync_vxy_from_global(view);
		cow_view_sync_container_geometry(view, new_x, new_y, new_w,
		    new_h);
		break;
	}

	case COW_POINTER_OP_NONE:
		break;
	}

	if (seat->op_released)
		op_finalize(seat, view);
}
