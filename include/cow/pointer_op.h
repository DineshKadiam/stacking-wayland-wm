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
#if !defined(COW_POINTER_OP_H)
#define COW_POINTER_OP_H

#include <stdbool.h>
#include <stdint.h>

struct cow_seat;
struct cow_view;
struct cow_output;

/*
 * Type of interactive pointer operation.
 */
enum cow_pointer_op_type {
	COW_POINTER_OP_NONE,
	COW_POINTER_OP_MOVE,
	COW_POINTER_OP_ICON_MOVE,
	COW_POINTER_OP_RESIZE,
	COW_POINTER_OP_DRAW_EXEC,
	/* Wait for a menu-selection button to be released. */
	COW_POINTER_OP_MENU_SELECT,
	/*
	 * Drag-the-viewport pan (the cow analogue of fvwm3's
	 * `Scroll reverse`).  No view is involved; pointer motion
	 * pans the bound output's desk viewport for as long as the
	 * triggering mouse button is held.
	 */
	COW_POINTER_OP_SCROLL_DRAG,
};

/*
 * Resize edge flags (matching river's river_window_v1.edges enum).
 */
#define COW_EDGE_TOP 1
#define COW_EDGE_BOTTOM 2
#define COW_EDGE_LEFT 4
#define COW_EDGE_RIGHT 8

/*
 * State for an in-progress pointer operation.
 */
struct cow_pointer_op {
	enum cow_pointer_op_type type;
	struct cow_view		*view;

	/* Initial position of the view when the op started */
	int start_x, start_y;

	int move_offset_x, move_offset_y;

	/*
	 * For MOVE: whether the view has been unmaximised in response
	 * to the drag actually starting.  Unmax is deferred to the
	 * first non-zero motion event so a press without subsequent
	 * motion (e.g. the first press of a double-click) leaves the
	 * view's geometry untouched, letting the 2-click action see
	 * the pre-press state.
	 */
	bool unmax_done;
	bool icon_moved;

	/* Initial dimensions when the op started */
	int start_width, start_height;

	/* For resize: which edges are being dragged */
	uint32_t resize_edges;

	/*
	 * SCROLL_DRAG state.  Pointer motion is interpreted relative
	 * to (scroll_anchor_vx, scroll_anchor_vy) -- the page_vx /
	 * page_vy of the bound output's current desk at op-start.
	 * scroll_output is held so the same output is panned for the
	 * duration of the drag even if the pointer wanders onto
	 * another output (matching fvwm: `Scroll reverse` always
	 * scrolls the monitor where the drag started).
	 *
	 * scroll_reverse=true makes motion pan the viewport in the
	 * opposite direction (the cursor appears to grab and drag the
	 * desk).  scroll_reverse=false makes motion pan in the same
	 * direction (the cursor pushes against an edge to scroll).
	 */
	struct cow_output *scroll_output;
	int		   scroll_anchor_vx, scroll_anchor_vy;
	bool		   scroll_reverse;
};

/*
 * Begin an interactive move of the given view.
 * Must be called during a manage sequence.
 */
void cow_pointer_op_start_move(struct cow_seat *, struct cow_view *);
void cow_pointer_op_start_icon_move(struct cow_seat *, struct cow_view *);

/*
 * Begin an interactive resize of the given view.
 * Must be called during a manage sequence.
 */
void cow_pointer_op_start_resize(struct cow_seat *, struct cow_view *,
    uint32_t);

/*
 * Begin an interactive scroll-drag on the given output.  Pointer
 * motion pans the output's current desk viewport relative to its
 * page_vx / page_vy at op start.  reverse selects whether the
 * cursor "grabs and drags" (true) or "pushes" (false) the viewport.
 * The op ends when the triggering mouse button is released.
 */
void cow_pointer_op_start_scroll(struct cow_seat *, struct cow_output *, bool);
void cow_pointer_op_start_draw_exec(struct cow_seat *);
void cow_pointer_op_start_menu_select(struct cow_seat *);
bool cow_pointer_op_finish_menu_select(struct cow_seat *);
void cow_pointer_op_disarm_attached(struct cow_seat *);

/* Return the view in an active non-container move, or NULL. */
struct cow_view *cow_pointer_op_active_move_view(void);

/* Return whether the view is in an active non-container move. */
bool cow_pointer_op_is_moving_view(const struct cow_view *);

/*
 * Process pointer operation state during a manage sequence.
 * Applies op_delta to the view's position/dimensions.
 */
void cow_pointer_op_process(struct cow_seat *);

#endif
