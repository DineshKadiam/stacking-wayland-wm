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
#include <xkbcommon/xkbcommon-keysyms.h>

#include "cow/compat.h"
#include "cow/config.h"
#include "cow/desk_ops.h"
#include "cow/draw_exec.h"
#include "cow/geometry_window.h"
#include "cow/log.h"
#include "cow/memory.h"
#include "cow/output.h"
#include "cow/pan_frames.h"
#include "cow/pointer_input.h"
#include "cow/rule.h"
#include "cow/seat.h"
#include "cow/selection.h"
#include "cow/style.h"
#include "cow/titlebar.h"
#include "cow/view.h"
#include "cow/update.h"
#include "cow/wm.h"

#include "river-window-management-v1-client-protocol.h"
#include "river-xkb-bindings-v1-client-protocol.h"

static void
attached_pointer_pressed(void *data,
    __unused struct river_pointer_binding_v1 *binding)
{
	struct cow_seat *seat = data;
	seat->op_released = true;
	cow_update_request(COW_UPDATE_MANAGE);
}

static void
attached_pointer_released(__unused void *data,
    __unused struct river_pointer_binding_v1 *binding)
{
}

static const struct river_pointer_binding_v1_listener
    attached_pointer_listener = {
	    .pressed  = attached_pointer_pressed,
	    .released = attached_pointer_released,
    };

static void
attached_escape_pressed(void *data,
    __unused struct river_xkb_binding_v1 *binding)
{
	struct cow_seat *seat = data;
	seat->op.cancelled    = true;
	seat->op_released = true;
	cow_update_request(COW_UPDATE_MANAGE);
}

static void
attached_escape_released(__unused void *data,
    __unused struct river_xkb_binding_v1 *binding)
{
}

static void
attached_escape_stop_repeat(__unused void *data,
    __unused struct river_xkb_binding_v1 *binding)
{
}

static const struct river_xkb_binding_v1_listener attached_escape_listener = {
	.pressed     = attached_escape_pressed,
	.released    = attached_escape_released,
	.stop_repeat = attached_escape_stop_repeat,
};

void
cow_pointer_op_disarm_attached(struct cow_seat *seat)
{
	if (seat == NULL)
		return;
	for (size_t i = 0; i < nitems(seat->op_finish_bindings); i++) {
		if (seat->op_finish_bindings[i] == NULL)
			continue;
		river_pointer_binding_v1_destroy(seat->op_finish_bindings[i]);
		seat->op_finish_bindings[i] = NULL;
	}
	if (seat->op_escape_binding != NULL) {
		river_xkb_binding_v1_destroy(seat->op_escape_binding);
		seat->op_escape_binding = NULL;
	}
	if (seat->op_escape_modified_binding != NULL) {
		river_xkb_binding_v1_destroy(seat->op_escape_modified_binding);
		seat->op_escape_modified_binding = NULL;
	}
}

static void
pointer_op_arm_attached(struct cow_seat *seat)
{
	if (!seat->attach_next_pointer_op && !seat->container_drag &&
	    !cow_container_is_split(seat->op.view))
		return;

	static const uint32_t buttons[] = { BTN_LEFT, BTN_MIDDLE, BTN_RIGHT };
	for (size_t i = 0; i < nitems(buttons); i++) {
		if (!seat->attach_next_pointer_op)
			break;
		seat->op_finish_bindings[i] =
		    river_seat_v1_get_pointer_binding(seat->river_seat,
			buttons[i], RIVER_SEAT_V1_MODIFIERS_NONE);
		river_pointer_binding_v1_add_listener(
		    seat->op_finish_bindings[i], &attached_pointer_listener,
		    seat);
		river_pointer_binding_v1_enable(seat->op_finish_bindings[i]);
	}
	if (cow_wm.xkb_bindings != NULL) {
		seat->op_escape_binding = river_xkb_bindings_v1_get_xkb_binding(
		    cow_wm.xkb_bindings, seat->river_seat, XKB_KEY_Escape,
		    RIVER_SEAT_V1_MODIFIERS_NONE);
		river_xkb_binding_v1_add_listener(seat->op_escape_binding,
		    &attached_escape_listener, seat);
		river_xkb_binding_v1_enable(seat->op_escape_binding);
		if (seat->active_modifiers != 0) {
			seat->op_escape_modified_binding =
			    river_xkb_bindings_v1_get_xkb_binding(
				cow_wm.xkb_bindings, seat->river_seat,
				XKB_KEY_Escape, seat->active_modifiers);
			river_xkb_binding_v1_add_listener(
			    seat->op_escape_modified_binding,
			    &attached_escape_listener, seat);
			river_xkb_binding_v1_enable(
			    seat->op_escape_modified_binding);
		}
	}
}

static const char *
pointer_op_type_name(enum cow_pointer_op_type type)
{
	switch (type) {
	case COW_POINTER_OP_MOVE:
		return ("move");
	case COW_POINTER_OP_RESIZE:
		return ("resize");
	case COW_POINTER_OP_ICON_MOVE:
		return ("icon-move");
	case COW_POINTER_OP_SCROLL_DRAG:
		return ("scroll-drag");
	case COW_POINTER_OP_DRAW_EXEC:
		return ("draw-exec");
	case COW_POINTER_OP_MENU_SELECT:
		return ("menu-select");
	case COW_POINTER_OP_NONE:
	default:
		return ("none");
	}
}

static void
pointer_op_group_clear(struct cow_pointer_op *op)
{
	xfree(op->group);
	op->group = NULL;
	op->group_count = 0;
}

static bool
seat_is_moving_view(const struct cow_seat *seat, const struct cow_view *view)
{
	return seat->op_in_progress && seat->op.type == COW_POINTER_OP_MOVE &&
	    !seat->container_drag && seat->op.view != NULL &&
	    !seat->op.view->closed && (view == NULL || seat->op.view == view);
}

struct cow_view *
cow_pointer_op_active_move_view(void)
{
	struct cow_seat *seat;
	wl_list_for_each(seat, &cow_wm.seats, wm_link)
	{
		if (seat_is_moving_view(seat, NULL))
			return seat->op.view;
	}
	return NULL;
}

bool
cow_pointer_op_is_moving_view(const struct cow_view *view)
{
	struct cow_seat *seat;
	wl_list_for_each(seat, &cow_wm.seats, wm_link)
	{
		if (seat_is_moving_view(seat, view))
			return true;
	}
	return false;
}

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

static bool
pointer_op_group_contains(const struct cow_pointer_op *op,
    const struct cow_view *view)
{
	if (op == NULL || op->type != COW_POINTER_OP_MOVE)
		return false;
	for (size_t i = 0; i < op->group_count; i++) {
		if (op->group[i].view == view)
			return true;
	}
	return false;
}

static void
snap_edges(struct cow_view *view, int *frame_left, int *frame_top,
    int *frame_right, int *frame_bottom, bool snap_size,
    const struct cow_pointer_op *op)
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
	struct cow_container *container = cow_container_top_group(view);
	struct cow_view	     *other;
	wl_list_for_each(other, &cow_wm.views, wm_link)
	{
		if (other == view || pointer_op_group_contains(op, other) ||
		    !other->has_dimensions ||
		    other->snap_skip)
			continue;
		/* Panes move with their frame, so cannot be snap targets for it
		 * or for another pane in the same container.
		 */
		if (container != NULL &&
		    cow_container_top_group(other) == container)
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

static void
pointer_op_frame_geometry(const struct cow_view *view, int *frame_w,
    int *frame_h, int *content_x, int *content_y)
{
	if (cow_container_is_split(view)) {
		struct cow_box frame = cow_container_frame_at(view, view->x,
		    view->y);
		*frame_w	     = frame.width;
		*frame_h	     = frame.height;
		*content_x	     = view->x - frame.x;
		*content_y	     = view->y - frame.y;
		return;
	}
	int cw, ch;
	cow_view_effective_content_size(view, &cw, &ch);
	cow_titlebar_frame_geometry(view, cw, ch, frame_w, frame_h,
	    content_x, content_y);
}

static void
pointer_op_save_container(struct cow_pointer_op *op, struct cow_view *view)
{
	struct cow_container *c	    = cow_view_container(view);
	op->container_id	    = c->id;
	op->container_frame	    = c->frame;
	op->container_restore_frame = c->restore_frame;
	op->container_maximized_h   = c->maximized_h;
	op->container_maximized_v   = c->maximized_v;
	op->container_keep_h	    = view->maximized_h_keep_geometry;
	op->container_keep_v	    = view->maximized_v_keep_geometry;
	op->container_ignore_h	    = view->maximized_h_ignore_reserved;
	op->container_ignore_v	    = view->maximized_v_ignore_reserved;
}

void
cow_pointer_op_start_move(struct cow_seat *seat, struct cow_view *view)
{
	if (view == NULL || view->closed || seat->op_in_progress ||
	    cow_container_is_attached_client(view))
		return;
	pointer_op_group_clear(&seat->op);
	seat->op.cancelled = false;
	if (cow_container_is_split(view))
		pointer_op_save_container(&seat->op, view);

	seat->op.type	 = COW_POINTER_OP_MOVE;
	seat->op.drop_output = NULL;
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
	seat->op.unmax_done  = false;
	cow_log_debug("pointer operation begin (type=move window=%s "
	    "geometry=%d,%d %dx%d)",
	    view->identifier ? view->identifier : "unknown", view->x, view->y,
	    view->content_width, view->content_height);

	/*
	 * Titlebar-press visual feedback: while a move is in progress
	 * the title strip renders pressed.  The flags are cleared by
	 * op_finalize when the drag ends.  Set them here so any caller
	 * (the wm.c decoration-click dispatch, the `window-move`
	 * command fired from a bind-decoration, ...) gets the same
	 * visual behaviour without duplicating the bookkeeping.
	 */
	cow_seat_clear_decoration_press(seat);
	view->title_pressed  = true;
	view->dec.dirty	     = true;
	seat->titlebar_press = COW_TITLEBAR_PRESS_TITLE;
	seat->pressed_view   = view;

	cow_pointer_cursor_hold_start();
	river_seat_v1_op_start_pointer(seat->river_seat);
	pointer_op_arm_attached(seat);
}

void
cow_pointer_op_start_group_move(struct cow_seat *seat, struct cow_view *view,
    const char *selection)
{
	struct cow_view **views = NULL;
	size_t count = cow_selection_collect(selection, &views);

	cow_pointer_op_start_move(seat, view);
	if (!seat->op_in_progress || seat->op.type != COW_POINTER_OP_MOVE) {
		xfree(views);
		return;
	}
	for (size_t i = 0; i < count; i++) {
		struct cow_view *member = views[i];
		if (member == view || member == NULL || member->closed ||
		    !member->has_dimensions || cow_view_is_iconified(member) ||
		    cow_container_is_attached_client(member))
			continue;
		cow_style_cancel_pending_initial(member);
		seat->op.group = xrealloc(seat->op.group,
		    seat->op.group_count + 1, sizeof(*seat->op.group));
		seat->op.group[seat->op.group_count++] =
		    (struct cow_pointer_op_member) {
			.view = member,
			.start_x = member->x,
			.start_y = member->y,
		};
	}
	xfree(views);
}

void
cow_pointer_op_start_icon_move(struct cow_seat *seat, struct cow_view *view)
{
	if (seat == NULL || view == NULL || view->closed ||
	    seat->op_in_progress)
		return;
	if (!cow_view_is_iconified(view))
		return;

	seat->op.type	 = COW_POINTER_OP_ICON_MOVE;
	seat->op.view	 = view;
	seat->op.start_x = view->icon_x;
	seat->op.start_y = view->icon_y;
	if (seat->has_pointer_position) {
		seat->op.move_offset_x = view->icon_x - seat->pointer_x;
		seat->op.move_offset_y = view->icon_y - seat->pointer_y;
	} else {
		seat->op.move_offset_x = 0;
		seat->op.move_offset_y = 0;
	}
	seat->op.icon_moved = false;
	seat->op_in_progress = true;
	seat->op_released    = false;
	seat->op_dx	     = 0;
	seat->op_dy	     = 0;
	cow_log_debug("pointer operation begin (type=icon-move window=%s "
	    "position=%d,%d)",
	    view->identifier ? view->identifier : "unknown", view->icon_x,
	    view->icon_y);

	river_seat_v1_op_start_pointer(seat->river_seat);
}

void
cow_pointer_op_start_scroll(struct cow_seat *seat, struct cow_output *output,
    bool reverse)
{
	if (seat == NULL || output == NULL || seat->op_in_progress)
		return;

	struct cow_desk *desk = output->current_desk;
	if (desk == NULL)
		return;

	seat->op.type		  = COW_POINTER_OP_SCROLL_DRAG;
	seat->op.view		  = NULL;
	seat->op.scroll_output	  = output;
	seat->op.scroll_anchor_vx = desk->page_vx;
	seat->op.scroll_anchor_vy = desk->page_vy;
	seat->op.scroll_reverse	  = reverse;
	seat->op_in_progress	  = true;
	seat->op_released	  = false;
	seat->op_dx		  = 0;
	seat->op_dy		  = 0;
	cow_log_debug("pointer operation begin (type=scroll-drag output=%s "
	    "viewport=%d,%d reverse=%s)",
	    output->name ? output->name : "unknown", desk->page_vx,
	    desk->page_vy, reverse ? "yes" : "no");

	river_seat_v1_op_start_pointer(seat->river_seat);
}

static void
resize_clear_maximized_axes(struct cow_view *view, uint32_t edges)
{
	bool clear_h = view->maximized_h &&
	    (edges & (COW_EDGE_LEFT | COW_EDGE_RIGHT)) != 0;
	bool clear_v = view->maximized_v &&
	    (edges & (COW_EDGE_TOP | COW_EDGE_BOTTOM)) != 0;

	if (!clear_h && !clear_v)
		return;
	if (clear_h) {
		view->maximized_h = false;
		view->maximized_h_ignore_reserved = false;
		view->maximized_h_keep_geometry = false;
	}
	if (clear_v) {
		view->maximized_v = false;
		view->maximized_v_ignore_reserved = false;
		view->maximized_v_keep_geometry = false;
	}
	view->maximized = view->maximized_h || view->maximized_v;
	if (!view->maximized) {
		cow_view_set_pending_maximized_hint(view, false);
		view->icon_inverted_button = -1;
	}
	view->dec.dirty = true;
	cow_update_request(COW_UPDATE_STATUS);
}

void
cow_pointer_op_start_draw_exec(struct cow_seat *seat)
{
	if (seat == NULL || seat->op_in_progress)
		return;

	seat->op.type	     = COW_POINTER_OP_DRAW_EXEC;
	seat->op.view	     = NULL;
	seat->op.start_x     = seat->has_pointer_position ? seat->pointer_x : 0;
	seat->op.start_y     = seat->has_pointer_position ? seat->pointer_y : 0;
	seat->op_in_progress = true;
	seat->op_released    = false;
	seat->op_dx	     = 0;
	seat->op_dy	     = 0;
	cow_log_debug("pointer operation begin (type=draw-exec position=%d,%d)",
	    seat->op.start_x, seat->op.start_y);

	river_seat_v1_op_start_pointer(seat->river_seat);
}

void
cow_pointer_op_start_menu_select(struct cow_seat *seat)
{
	if (seat == NULL || seat->op_in_progress)
		return;

	seat->op.type	     = COW_POINTER_OP_MENU_SELECT;
	seat->op.view	     = NULL;
	seat->op_in_progress = true;
	seat->op_released    = false;
	seat->op_dx	     = 0;
	seat->op_dy	     = 0;
	river_seat_v1_op_start_pointer(seat->river_seat);
}

bool
cow_pointer_op_finish_menu_select(struct cow_seat *seat)
{
	if (seat == NULL || !seat->op_in_progress ||
	    seat->op.type != COW_POINTER_OP_MENU_SELECT)
		return (true);
	if (!seat->op_released)
		return (false);

	river_seat_v1_op_end(seat->river_seat);
	seat->op.type	     = COW_POINTER_OP_NONE;
	seat->op_in_progress = false;
	seat->op_released    = false;
	seat->op_dx	     = 0;
	seat->op_dy	     = 0;
	return (true);
}

static bool
pointer_op_start_resize(struct cow_seat *seat, struct cow_view *view,
    uint32_t edges, bool indicate, bool warp, int warp_x, int warp_y)
{
	if (seat == NULL || view == NULL || view->closed || seat->op_in_progress)
		return false;
	if (cow_container_is_attached_client(view))
		return false;
	bool wants_w = (edges & (COW_EDGE_LEFT | COW_EDGE_RIGHT)) != 0;
	bool wants_h = (edges & (COW_EDGE_TOP | COW_EDGE_BOTTOM)) != 0;
	bool fixed_w = view->min_width > 0 && view->max_width > 0 &&
	    view->min_width == view->max_width;
	bool fixed_h = view->min_height > 0 && view->max_height > 0 &&
	    view->min_height == view->max_height;
	if ((!wants_w || fixed_w) && (!wants_h || fixed_h))
		return false;
	if (warp) {
		river_seat_v1_pointer_warp(seat->river_seat, warp_x, warp_y);
		seat->pointer_x = warp_x;
		seat->pointer_y = warp_y;
		seat->has_pointer_position = true;
	}
	seat->op.container_resize = cow_container_is_split(view);
	seat->op.cancelled	  = false;
	if (seat->op.container_resize) {
		pointer_op_save_container(&seat->op, view);
		seat->op.pane_frame	 = cow_view_pane(view)->layout.box;
		cow_container_resize_save(view, edges,
		    seat->op.container_dividers, seat->op.container_shares);
		cow_view_set_maximized(view, false);
	} else {
		resize_clear_maximized_axes(view, edges);
	}

	seat->op.type	      = COW_POINTER_OP_RESIZE;
	seat->op.view	      = view;
	seat->op.start_x      = view->x;
	seat->op.start_y      = view->y;
	seat->op.start_width  = view->content_width;
	seat->op.start_height = view->content_height;
	seat->op.resize_edges = edges;
	view->resize_indicator_edges = indicate &&
	    view->cfg.has_border_resize ? edges : 0;
	if (view->resize_indicator_edges != 0)
		view->dec.dirty = true;
	seat->op_in_progress  = true;
	seat->op_released     = false;
	seat->op_dx	      = 0;
	seat->op_dy	      = 0;
	cow_log_debug("pointer operation begin (type=resize window=%s "
	    "geometry=%d,%d %dx%d edges=0x%x)",
	    view->identifier ? view->identifier : "unknown", view->x, view->y,
	    view->content_width, view->content_height, edges);

	cow_view_clear_expanded_state(view);
	river_window_v1_inform_resize_start(view->river_window);
	cow_pointer_cursor_hold_start();
	river_seat_v1_op_start_pointer(seat->river_seat);
	pointer_op_arm_attached(seat);
	return true;
}

bool
cow_pointer_op_start_resize(struct cow_seat *seat, struct cow_view *view,
    uint32_t edges, bool indicate)
{
	return pointer_op_start_resize(seat, view, edges, indicate, false, 0, 0);
}

bool
cow_pointer_op_start_resize_warped(struct cow_seat *seat,
    struct cow_view *view, uint32_t edges, bool indicate, int warp_x,
    int warp_y)
{
	return pointer_op_start_resize(seat, view, edges, indicate, true, warp_x,
	    warp_y);
}

/*
 * Constrain (*new_x, *new_y) so the dragged view's frame doesn't slide off
 * the output edge when no further page exists in the direction of travel.
 */
static void
clamp_drag_to_output(const struct cow_view *view,
    const struct cow_output *output, int *new_x, int *new_y)
{
	if (output == NULL || cow_config == NULL)
		return;

	int cols    = cow_config->page_cols;
	int rows    = cow_config->page_rows;
	int cur_col = cow_page_col(output);
	int cur_row = cow_page_row(output);
	int ox	    = output->x;
	int oy	    = output->y;
	int ow	    = output->width;
	int oh	    = output->height;
	int frame_w, frame_h, content_x, content_y;
	pointer_op_frame_geometry(view, &frame_w, &frame_h, &content_x,
	    &content_y);

	struct cow_box nfb = cow_view_frame_box_at(view, *new_x, *new_y);
	struct cow_box cfb = cow_view_frame_box(view);

	unsigned mask	    = cow_pan_edge_mask(cur_col, cur_row, cols, rows);
	bool	 can_left   = (mask & COW_PAN_EDGE_BIT(COW_PAN_LEFT)) != 0;
	bool	 can_right  = (mask & COW_PAN_EDGE_BIT(COW_PAN_RIGHT)) != 0;
	bool	 can_top    = (mask & COW_PAN_EDGE_BIT(COW_PAN_TOP)) != 0;
	bool	 can_bottom = (mask & COW_PAN_EDGE_BIT(COW_PAN_BOTTOM)) != 0;

	if (!can_left && nfb.x < cfb.x && nfb.x < ox && frame_w <= ow)
		*new_x = ox + content_x;
	if (!can_right && (nfb.x + nfb.width) > (cfb.x + cfb.width) &&
	    (nfb.x + nfb.width) > ox + ow && frame_w <= ow)
		*new_x = ox + ow - frame_w + content_x;
	if (!can_top && nfb.y < cfb.y && nfb.y < oy && frame_h <= oh)
		*new_y = oy + content_y;
	if (!can_bottom && (nfb.y + nfb.height) > (cfb.y + cfb.height) &&
	    (nfb.y + nfb.height) > oy + oh && frame_h <= oh)
		*new_y = oy + oh - frame_h + content_y;
}

/*
 * Clamp to the bounding rectangle containing every output. This permits
 * uninterrupted movement through rectangular output grids. Deliberate gaps
 * remain traversable, but cow_output_for_box() never assigns a gap as the
 * window's owner.
 */
static void
clamp_drag_to_all_outputs(const struct cow_view *view,
    const struct cow_output *current, int *new_x, int *new_y)
{
	if (current == NULL || cow_config == NULL)
		return;

	int cols    = cow_config->page_cols;
	int rows    = cow_config->page_rows;
	int cur_col = cow_page_col(current);
	int cur_row = cow_page_row(current);
	struct cow_box bounds;
	if (!cow_outputs_box(&bounds))
		return;

	int frame_w, frame_h, content_x, content_y;
	pointer_op_frame_geometry(view, &frame_w, &frame_h, &content_x,
	    &content_y);

	struct cow_box nfb = cow_view_frame_box_at(view, *new_x, *new_y);
	struct cow_box cfb = cow_view_frame_box(view);

	unsigned mask	    = cow_pan_edge_mask(cur_col, cur_row, cols, rows);
	bool	 can_left   = (mask & COW_PAN_EDGE_BIT(COW_PAN_LEFT)) != 0;
	bool	 can_right  = (mask & COW_PAN_EDGE_BIT(COW_PAN_RIGHT)) != 0;
	bool	 can_top    = (mask & COW_PAN_EDGE_BIT(COW_PAN_TOP)) != 0;
	bool	 can_bottom = (mask & COW_PAN_EDGE_BIT(COW_PAN_BOTTOM)) != 0;

	if (!can_left && nfb.x < cfb.x && nfb.x < bounds.x &&
	    frame_w <= bounds.width)
		*new_x = bounds.x + content_x;
	if (!can_right && nfb.x + nfb.width > cfb.x + cfb.width &&
	    nfb.x + nfb.width > bounds.x + bounds.width &&
	    frame_w <= bounds.width)
		*new_x = bounds.x + bounds.width - frame_w + content_x;
	if (!can_top && nfb.y < cfb.y && nfb.y < bounds.y &&
	    frame_h <= bounds.height)
		*new_y = bounds.y + content_y;
	if (!can_bottom && nfb.y + nfb.height > cfb.y + cfb.height &&
	    nfb.y + nfb.height > bounds.y + bounds.height &&
	    frame_h <= bounds.height)
		*new_y = bounds.y + bounds.height - frame_h + content_y;
}

static void
pointer_op_reassign_output(struct cow_view *view, int x, int y, int width,
    int height, bool has_pointer_position)
{
	struct cow_box content = {
		.x = x,
		.y = y,
		.width = width,
		.height = height,
	};
	struct cow_output *owner = cow_output_for_box(&content, view->output);
	if (cow_container_is_split(view)) {
		content = cow_container_frame_at(view, x, y);
		owner	= cow_output_for_box(&content, view->output);
	}

	if (owner != NULL && owner != view->output) {
		cow_view_reassign_output(view, owner);
		cow_view_clear_orphan(view);
	} else if (owner == NULL && !has_pointer_position) {
		cow_view_update_output(view);
	}
}

static void
pointer_op_move_group(struct cow_seat *seat, int delta_x, int delta_y)
{
	for (size_t i = 0; i < seat->op.group_count; i++) {
		struct cow_view *view = seat->op.group[i].view;
		if (view == NULL || view->closed)
			continue;
		if ((delta_x != 0 || delta_y != 0) &&
		    (view->maximized || view->expanded)) {
			if (view->maximized)
				cow_view_set_maximized(view, false);
			cow_view_clear_expanded_state(view);
			view->icon_inverted_button = -1;
		}
		int new_x = seat->op.group[i].start_x + delta_x;
		int new_y = seat->op.group[i].start_y + delta_y;
		if (new_x == view->x && new_y == view->y)
			continue;
		view->x = new_x;
		view->y = new_y;
		pointer_op_reassign_output(view, new_x, new_y,
		    view->content_width, view->content_height, false);
		cow_view_sync_vxy_from_global(view);
		cow_view_sync_container_pos(view);
		view->dec.dirty = true;
	}
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
	enum cow_pointer_op_type finished_type = seat->op.type;
	bool icon_moved     = seat->op.icon_moved;
	bool joined_container = false;
	bool			   detached_container = false;
	bool detach_pane = false;
	if (seat->pane_drag && seat->has_pointer_position &&
	    !seat->op.cancelled) {
		struct cow_container *c = cow_view_container(view);
		if (c != NULL) {
			struct cow_box frame = c->frame_view ?
			    cow_view_frame_box(c->frame_view) : c->frame;
			detach_pane = !cow_box_contains_point(&frame,
			    seat->pointer_x, seat->pointer_y);
		}
	}
	struct cow_container_pane *empty_pane	    = NULL;
	struct cow_container	  *empty_container  = NULL;
	if (!seat->op.cancelled && seat->container_drag &&
	    finished_type == COW_POINTER_OP_MOVE &&
	    view->container_member == NULL && !cow_container_window_owner(view))
		empty_container = cow_container_empty_at(seat->pointer_x,
		    seat->pointer_y, view, &empty_pane);
	struct cow_output *drop_output = seat->op.drop_output;
	int drop_edge = seat->op.drop_edge;
	cow_pan_cancel();

	if (seat->op.type == COW_POINTER_OP_RESIZE)
		river_window_v1_inform_resize_end(view->river_window);
	if (finished_type == COW_POINTER_OP_RESIZE &&
	    view->resize_indicator_edges != 0) {
		view->resize_indicator_edges = 0;
		view->dec.dirty = true;
	}

	/*
	 * Container drag (-C) completion.
	 *
	 * Source already in a container: always detach, regardless of
	 * where the pointer lands.  No target highlight needed.
	 *
	 * Source NOT in a container: group with whatever window is
	 * under the pointer (if any).
	 */
	if (seat->op.cancelled) {
		bool restored = false;
		if (seat->op.type == COW_POINTER_OP_RESIZE &&
		    seat->op.container_resize) {
			struct cow_container *c = cow_view_container(view);
			if (c != NULL && c->id == seat->op.container_id)
				restored = cow_container_resize_restore(view,
				    seat->op.resize_edges,
				    seat->op.container_frame,
				    seat->op.container_dividers,
				    seat->op.container_shares);
		} else if (seat->op.type == COW_POINTER_OP_MOVE &&
		    !seat->container_drag && cow_container_is_split(view)) {
			struct cow_container *c = cow_view_container(view);
			if (c->id == seat->op.container_id)
				restored = cow_container_arrange(c,
				    seat->op.container_frame);
		}
		if (restored) {
			struct cow_container *c = cow_view_container(view);
			if (seat->op.container_maximized_h)
				cow_container_maximize(view, true, false, false,
				    true, seat->op.container_ignore_h);
			if (seat->op.container_maximized_v)
				cow_container_maximize(view, false, true, false,
				    true, seat->op.container_ignore_v);
			c->restore_frame = seat->op.container_restore_frame;
			struct cow_container_member *m;
			wl_list_for_each(m, &c->members, link)
			{
				m->view->x = m->view->pending_x;
				m->view->y = m->view->pending_y;
				cow_view_sync_vxy_from_global(m->view);
				m->view->maximized_h_keep_geometry =
				    seat->op.container_keep_h;
				m->view->maximized_v_keep_geometry =
				    seat->op.container_keep_v;
			}
		}
		if (cow_wm.drag_drop_target != NULL)
			cow_wm.drag_drop_target->dec.dirty = true;
		cow_wm.drag_drop_target = NULL;
	} else if (seat->pane_drag && !detach_pane) {
		struct cow_view *target	    = cow_wm.drag_drop_target;
		uint32_t	 edge	    = cow_wm.drag_drop_edge;
		bool		 rearranged = false;
		if (target != NULL) {
			target->dec.dirty = true;
			rearranged = cow_container_rearrange(target, view,
			    edge == 0 ? COW_CONTAINER_LEAF :
				(edge & (COW_EDGE_LEFT | COW_EDGE_RIGHT)) ?
					COW_CONTAINER_HORIZONTAL :
					COW_CONTAINER_VERTICAL,
			    (edge & (COW_EDGE_LEFT | COW_EDGE_TOP)) != 0);
		}
		seat->op.cancelled	= !rearranged;
		cow_wm.drag_drop_target = NULL;
	} else if (empty_container != NULL) {
		joined_container = cow_container_insert(empty_container,
		    empty_pane, view);
	} else if (seat->container_drag &&
	    seat->op.type == COW_POINTER_OP_MOVE &&
	    view->container_member != NULL) {
		/* Uncontainerise: detach, restore pre-container size,
		 * place at pointer */
		if (cow_wm.drag_drop_target != NULL) {
			cow_wm.drag_drop_target->dec.dirty = true;
			cow_wm.drag_drop_target		   = NULL;
		}
		/* cow_container_remove_view restores pre-container
		 * dimensions */
		cow_container_remove_view(view);
		/* Detach queues the restored size, not yet proposed to River.
		 * Replace its old position with the drop position as well.
		 */
		int rw = view->pending_resize ? view->pending_width :
						view->content_width;
		int rh = view->pending_resize ? view->pending_height :
						view->content_height;
		struct cow_output *output = cow_output_at(seat->pointer_x,
		    seat->pointer_y);
		if (output && output->current_desk) {
			cow_view_reassign_output(view, output);
			cow_view_assign_desk(view, output->current_desk->nr);
			cow_view_clear_orphan(view);
		}
		int frame_w, frame_h, content_x, content_y;
		cow_titlebar_frame_geometry(view, rw, rh, &frame_w, &frame_h,
		    &content_x, &content_y);
		view->x		= seat->pointer_x - frame_w / 2 + content_x;
		view->y		= seat->pointer_y - frame_h / 2 + content_y;
		/* Keep the content origin on the drop page, even near an edge.
		 */
		if (output) {
			if (view->x < output->x)
				view->x = output->x;
			if (view->y < output->y)
				view->y = output->y;
		}
		cow_view_sync_vxy_from_global(view);
		cow_view_set_pending_geometry(view, view->x, view->y, rw, rh);
		detached_container = true;
		view->dec.dirty = true;
		cow_update_request(COW_UPDATE_MANAGE);
	} else if (seat->container_drag &&
	    seat->op.type == COW_POINTER_OP_MOVE &&
	    cow_wm.drag_drop_target != NULL &&
	    !cow_wm.drag_drop_target->closed &&
	    cow_wm.drag_drop_target != view) {
		/* Containerise: group source with target */
		struct cow_view *target = cow_wm.drag_drop_target;
		cow_wm.drag_drop_target = NULL;
		target->dec.dirty	= true;
		bool	 added		= target->container_member != NULL ?
				 cow_container_add_to(target, view) :
				 cow_container_create(target, view) != NULL;
		uint32_t edge		= cow_wm.drag_drop_edge;
		if (added && edge != 0 &&
		    !cow_container_split(target, view,
			(edge & (COW_EDGE_LEFT | COW_EDGE_RIGHT)) ?
			    COW_CONTAINER_HORIZONTAL :
			    COW_CONTAINER_VERTICAL,
			(edge & (COW_EDGE_LEFT | COW_EDGE_TOP)) != 0))
			cow_container_remove_view(view);
		joined_container = added && view->container_member != NULL;
	} else if (cow_wm.drag_drop_target != NULL) {
		/* Normal drop: clear any lingering indicator */
		cow_wm.drag_drop_target->dec.dirty = true;
		cow_wm.drag_drop_target		   = NULL;
	}
	/* Even a cancelled pane drag must not reassign the layout's output. */
	joined_container |= seat->pane_drag;
	seat->pane_drag		= false;
	seat->container_drag	= false;
	cow_wm.drag_drop_pane	= NULL;
	cow_wm.drag_drop_source = NULL;
	cow_drag_ghost_destroy();
	cow_geometry_window_hide();

	/* Joining adopts the target's output, not the source's old geometry. */
	if (finished_type != COW_POINTER_OP_ICON_MOVE && !joined_container &&
	    !detached_container) {
		int width = view->proposed_width > 0 ? view->proposed_width :
		    view->content_width;
		int height = view->proposed_height > 0 ? view->proposed_height :
		    view->content_height;
		pointer_op_reassign_output(view, view->x, view->y, width, height,
		    seat->has_pointer_position);
	}
	if (finished_type == COW_POINTER_OP_MOVE) {
		for (size_t i = 0; i < seat->op.group_count; i++) {
			struct cow_view *member = seat->op.group[i].view;
			if (member == NULL || member->closed)
				continue;
			pointer_op_reassign_output(member, member->x, member->y,
			    member->content_width, member->content_height, false);
		}
	}

	if (finished_type == COW_POINTER_OP_ICON_MOVE) {
		cow_log_debug("pointer operation end (type=%s window=%s "
		    "position=%d,%d moved=%s)",
		    pointer_op_type_name(finished_type),
		    view->identifier ? view->identifier : "unknown",
		    view->icon_x, view->icon_y, icon_moved ? "yes" : "no");
	} else {
		cow_log_debug("pointer operation end (type=%s window=%s "
		    "geometry=%d,%d %dx%d)",
		    pointer_op_type_name(finished_type),
		    view->identifier ? view->identifier : "unknown", view->x,
		    view->y, view->content_width, view->content_height);
	}

	cow_seat_clear_decoration_press(seat);

	cow_pointer_op_disarm_attached(seat);
	river_seat_v1_op_end(seat->river_seat);
	seat->op_in_progress = false;
	seat->op_released    = false;
	seat->op.icon_moved  = false;
	seat->op.type	     = COW_POINTER_OP_NONE;
	cow_pointer_cursor_hold_end();

	if (finished_type == COW_POINTER_OP_ICON_MOVE && !icon_moved) {
		cow_view_deiconify(view);
		return;
	}
	if (finished_type == COW_POINTER_OP_ICON_MOVE)
		cow_update_request(COW_UPDATE_STATUS);

	/* Fire lifecycle rules for the completed op.  Skipped if the
	 * view was destroyed during the op (caller has already returned
	 * before reaching here). */
	if (!seat->op.cancelled && view != NULL && !view->closed) {
		if (finished_type == COW_POINTER_OP_MOVE)
			cow_rule_fire(COW_RULE_ON_MOVE, view, view->desk,
			    view->output);
		else if (finished_type == COW_POINTER_OP_RESIZE)
			cow_rule_fire(COW_RULE_ON_RESIZE, view, view->desk,
			    view->output);
	}
	if (!seat->op.cancelled && finished_type == COW_POINTER_OP_MOVE) {
		for (size_t i = 0; i < seat->op.group_count; i++) {
			struct cow_view *member = seat->op.group[i].view;
			if (member != NULL && !member->closed)
				cow_rule_fire(COW_RULE_ON_MOVE, member, member->desk,
				    member->output);
		}
	}
	pointer_op_group_clear(&seat->op);
	if (finished_type == COW_POINTER_OP_MOVE)
		cow_update_request(COW_UPDATE_STATUS);
	if (!seat->op.cancelled && finished_type == COW_POINTER_OP_MOVE)
		cow_pan_finish_drop(view, drop_output, drop_edge);
}

void
cow_pointer_op_process(struct cow_seat *seat)
{
	if (!seat->op_in_progress)
		return;
	/* Popup and multi-select clicks need no deferred action, but their
	 * pointer grab must still end when the button is released.
	 */
	if (seat->op.type == COW_POINTER_OP_MENU_SELECT) {
		(void)cow_pointer_op_finish_menu_select(seat);
		return;
	}

	/*
	 * SCROLL_DRAG is view-less: pan the bound output's viewport
	 * from cumulative deltas (op_dx / op_dy).  The view-anchored
	 * MOVE / RESIZE paths below all assume seat->op.view is set;
	 * scroll-drag has its own short-circuit here and never reaches
	 * them.
	 */
	if (seat->op.type == COW_POINTER_OP_SCROLL_DRAG) {
		/*
		 * Defensive lookup: if the bound output was disconnected
		 * mid-drag the cached scroll_output pointer is dangling.
		 * Walk the live outputs list to confirm it's still around.
		 */
		struct cow_output *out = NULL;
		struct cow_output *cand;
		wl_list_for_each(cand, &cow_wm.outputs, wm_link)
		{
			if (cand == seat->op.scroll_output) {
				out = cand;
				break;
			}
		}
		if (out != NULL) {
			int dx = seat->op_dx;
			int dy = seat->op_dy;
			if (seat->op.scroll_reverse) {
				/* "Grab and drag" the viewport: cursor
				 * moves right, the desk appears to move
				 * right (viewport offset moves LEFT). */
				dx = -dx;
				dy = -dy;
			}
			int new_vx = seat->op.scroll_anchor_vx + dx;
			int new_vy = seat->op.scroll_anchor_vy + dy;
			/* No wrap during interactive drag: a wrap would
			 * teleport the viewport to the opposite edge mid
			 * gesture and feel broken.  Clamp instead. */
			cow_page_scroll_to(out, new_vx, new_vy, false);
		}
		if (out == NULL || seat->op_released) {
			if (out != NULL && out->current_desk != NULL) {
				cow_log_debug("pointer operation end (type=scroll-drag "
				    "output=%s viewport=%d,%d)",
				    out->name ? out->name : "unknown",
				    out->current_desk->page_vx,
				    out->current_desk->page_vy);
			} else {
				cow_log_debug("pointer operation cancelled "
				    "(type=scroll-drag output-removed=yes)");
			}
			river_seat_v1_op_end(seat->river_seat);
			seat->op_in_progress   = false;
			seat->op_released      = false;
			seat->op.type	       = COW_POINTER_OP_NONE;
			seat->op.scroll_output = NULL;
			cow_pointer_cursor_hold_end();
		}
		return;
	}

	if (seat->op.type == COW_POINTER_OP_DRAW_EXEC) {
		cow_draw_exec_update(seat);
		if (seat->op_released) {
			cow_log_debug("pointer operation end (type=draw-exec "
			    "delta=%d,%d)", seat->op_dx, seat->op_dy);
			river_seat_v1_op_end(seat->river_seat);
			seat->op_in_progress = false;
			seat->op_released = false;
			seat->op.type = COW_POINTER_OP_NONE;
			cow_draw_exec_finish(seat);
		}
		return;
	}

	struct cow_view *view = seat->op.view;
	if (view == NULL || view->closed) {
		cow_pan_cancel();
		cow_log_debug("pointer operation cancelled (type=%s "
		    "window-unavailable=yes)",
		    pointer_op_type_name(seat->op.type));
		if (cow_wm.drag_drop_target != NULL &&
		    !cow_wm.drag_drop_target->closed)
			cow_wm.drag_drop_target->dec.dirty = true;
		cow_wm.drag_drop_target = NULL;
		cow_wm.drag_drop_source = NULL;
		cow_wm.drag_drop_pane	= NULL;
		seat->container_drag = false;
		seat->pane_drag		= false;
		cow_drag_ghost_destroy();

		cow_pointer_op_disarm_attached(seat);
		river_seat_v1_op_end(seat->river_seat);
		seat->op_in_progress = false;
		seat->op_released    = false;
		seat->op.type	     = COW_POINTER_OP_NONE;
		pointer_op_group_clear(&seat->op);
		cow_pointer_cursor_hold_end();
		cow_geometry_window_hide();
		return;
	}

	/*
	 * Pan-frame edge detection during a drag/resize.  River does not
	 * deliver pointer events to layer surfaces while a pointer-op is
	 * in progress, so the layer-surface pointer_enter listener never
	 * fires.  Use the seat's tracked pointer position to arm pan_edge
	 * by proximity instead.
	 */
	if (seat->has_pointer_position && !seat->pane_drag)
		cow_output_pan_op_arm(seat);

	int dx = seat->op_dx;
	int dy = seat->op_dy;

	switch (seat->op.type) {
	case COW_POINTER_OP_MOVE: {
		/*
		 * Deferred unmaximise: on the first frame that carries
		 * actual motion, drop the view's maximised state so the
		 * drag can move it freely.  A press with no motion (e.g.
		 * the first press of a double-click) skips this entirely,
		 * leaving the view's geometry untouched for a follow-up
		 * 2-click bind-decoration action to see the pre-press
		 * state.
		 */
		if (!seat->pane_drag && !seat->op.unmax_done &&
		    (dx != 0 || dy != 0) &&
		    (view->maximized || view->expanded)) {
			if (view->maximized)
				cow_view_set_maximized(view, false);
			cow_view_clear_expanded_state(view);
			view->icon_inverted_button = -1;
			view->dec.dirty		   = true;
			seat->op.unmax_done	   = true;
			/* Recompute the offset so the cursor stays at the
			 * same surface-relative point on the now-unmaxed
			 * view.  Without this, the post-unmax view jumps
			 * away from the cursor and tracking feels stuck. */
			if (seat->has_pointer_position) {
				seat->op.move_offset_x = view->x -
				    seat->pointer_x;
				seat->op.move_offset_y = view->y -
				    seat->pointer_y;
			}
		}
		int frame_w, frame_h, content_x, content_y;
		pointer_op_frame_geometry(view, &frame_w, &frame_h,
		    &content_x, &content_y);

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
		int fl = cx - content_x;
		int ft = cy - content_y;
		int fr = fl + frame_w;
		int fb = ft + frame_h;
		snap_edges(view, &fl, &ft, &fr, &fb, false, &seat->op);

		if (seat->container_drag) {
			/* Container drag (-C): keep source in place, move ghost
			 */
			cow_wm.drag_ghost_x = seat->pointer_x;
			cow_wm.drag_ghost_y = seat->pointer_y;
		} else {
			/* Normal move */
			int new_x = fl + content_x;
			int new_y = ft + content_y;
			struct cow_output *clamp_output = view->output;
			if (seat->has_pointer_position) {
				struct cow_output *exact = cow_output_at_exact(
				    seat->pointer_x, seat->pointer_y);
				if (exact != NULL)
					clamp_output = exact;
			}

			if (cow_config->clamp_mode == COW_CLAMP_TO_OUTPUT) {
				clamp_drag_to_output(view, clamp_output, &new_x, &new_y);
			} else if (cow_config->clamp_mode == COW_CLAMP_TO_ALL_OUTPUTS) {
				clamp_drag_to_all_outputs(view, clamp_output, &new_x,
					&new_y);
			}
			bool position_changed = new_x != view->x || new_y != view->y;

			view->x = new_x;
			view->y = new_y;
			pointer_op_move_group(seat, new_x - seat->op.start_x,
			    new_y - seat->op.start_y);

			/*
			 * Clamp against the output under the pointer so an adjoining
			 * output does not act as a barrier.  Transfer ownership only
			 * when the content origin enters that output, matching the
			 * origin-based vx/vy page visibility model.  This distinction
			 * matters for vertical titlebars, where the pointer and frame
			 * can cross an output boundary before the content does.
			 */
			/* Ownership follows the content origin when possible,
			 * matching page visibility. Overlap provides a deterministic
			 * fallback when that origin lies in an output gap.
			 */
			pointer_op_reassign_output(view, new_x, new_y,
			    view->content_width, view->content_height,
			    seat->has_pointer_position);
			cow_view_sync_vxy_from_global(view);
			cow_view_sync_container_pos(view);
			if (position_changed)
				cow_update_request(COW_UPDATE_STATUS);
			if (!seat->container_drag &&
			    cow_config->geometry_window !=
				COW_GEOMETRY_WINDOW_NONE &&
			    cow_config->geometry_window !=
				COW_GEOMETRY_WINDOW_RESIZE) {
				cow_geometry_window_update_move(view->output, view->x,
				    view->y);
			}
		}
		break;
	}

	case COW_POINTER_OP_ICON_MOVE: {
		int new_x, new_y;

		if (seat->has_pointer_position) {
			new_x = seat->pointer_x + seat->op.move_offset_x;
			new_y = seat->pointer_y + seat->op.move_offset_y;
		} else {
			new_x = seat->op.start_x + dx;
			new_y = seat->op.start_y + dy;
		}
		if (new_x != view->icon_x || new_y != view->icon_y) {
			view->icon_x = new_x;
			view->icon_y = new_y;
			view->icon_manual_position = true;
			view->icon_auto_position_valid = false;
			seat->op.icon_moved = true;
			if (view->icon_node != NULL)
				river_node_v1_set_position(view->icon_node,
				    view->icon_x, view->icon_y);
		}
		break;
	}

	case COW_POINTER_OP_RESIZE: {
		if (seat->op.container_resize) {
			struct cow_container *c = cow_view_container(view);
			if (c != NULL && c->id == seat->op.container_id)
				cow_container_resize(view,
				    seat->op.resize_edges,
				    seat->op.container_frame,
				    seat->op.pane_frame, dx, dy);
			break;
		}
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
			int frame_w, frame_h, content_x, content_y;
			cow_titlebar_frame_geometry(view, new_w, new_h,
			    &frame_w, &frame_h, &content_x, &content_y);
			int frame_extra_w = frame_w - new_w;
			int frame_extra_h = frame_h - new_h;
			int fl		  = new_x - content_x;
			int ft		  = new_y - content_y;
			int fr		  = fl + frame_w;
			int fb		  = ft + frame_h;
			snap_edges(view, &fl, &ft, &fr, &fb, true, NULL);
			if (edges & COW_EDGE_LEFT) {
				new_x = fl + content_x;
				new_w = fr - fl - frame_extra_w;
			}
			if (edges & COW_EDGE_RIGHT)
				new_w = fr - fl - frame_extra_w;
			if (edges & COW_EDGE_TOP) {
				new_y = ft + content_y;
				new_h = fb - ft - frame_extra_h;
			}
			if (edges & COW_EDGE_BOTTOM)
				new_h = fb - ft - frame_extra_h;
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

		/* Use the proposed dimensions: the dimensions event has not
		 * updated view->content_width/height yet.
		 */
		pointer_op_reassign_output(view, new_x, new_y, new_w, new_h,
		    seat->has_pointer_position);
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
		{
			struct cow_output *output = NULL;
			if (seat->has_pointer_position)
				output = cow_output_at(seat->pointer_x,
				    seat->pointer_y);
			if (output == NULL)
				output = view->output;
			if (cow_config->geometry_window !=
				COW_GEOMETRY_WINDOW_NONE &&
			    cow_config->geometry_window !=
				COW_GEOMETRY_WINDOW_MOVE)
				cow_geometry_window_update_resize(output, new_w,
				    new_h);
		}
		break;
	}

	case COW_POINTER_OP_NONE:
	case COW_POINTER_OP_SCROLL_DRAG:
	case COW_POINTER_OP_DRAW_EXEC:
	case COW_POINTER_OP_MENU_SELECT:
		/* View-less operations are handled above and never reach here.
		 * Listed for completeness so -Wswitch stays clean. */
		break;
	}

	if (seat->op_released)
		op_finalize(seat, view);
}
