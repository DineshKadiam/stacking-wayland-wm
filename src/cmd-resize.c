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

#include <stdlib.h>
#include <string.h>

#include "cow/compat.h"
#include "cow/cmd.h"
#include "cow/config.h"
#include "cow/output.h"
#include "cow/pointer_op.h"
#include "cow/seat.h"
#include "cow/titlebar.h"
#include "cow/view.h"
#include "cow/wm.h"

#include "river-window-management-v1-client-protocol.h"

static enum cmd_retval cmd_resize_exec(struct cmd *, struct cmd_q *);
static void	       do_resize_step(struct cow_view *, const char *, int,
	       struct cmd_q *);
static void do_set_size(struct cow_view *, const char *, const char *,
    struct cmd_q *);
static void do_interactive(struct cow_view *, uint32_t, bool, bool);
static bool parse_edges(const char *, uint32_t *);
static bool automatic_edges(struct cow_view *, struct cow_seat *, uint32_t *,
    struct cmd_q *);
static void resize_warp_position(struct cow_view *, struct cow_seat *,
    uint32_t, int *, int *);

struct cmd_entry cmd_resize_entry = {
	"window-resize",
	"t:gsWd:w:h:e:",
	0,
	0,
	"[-gs] [-d dir] [-e edges|automatic] [-W] [-w width] [-h height]",
	cmd_resize_exec,
	0,
};

static enum cmd_retval
cmd_resize_exec(struct cmd *self, struct cmd_q *cmdq)
{
	struct args	*args = self->args;
	struct cow_view *view = cmd_require_target_window(cmdq, "resize");

	if (view == NULL)
		return (CMD_RETURN_ERROR);

	const char *dir_val = args_get(args, 'd');
	const char *w_val   = args_get(args, 'w');
	const char *h_val   = args_get(args, 'h');
	const char *edges_str = args_get(args, 'e');
	bool	    warp = args_has(args, 'W');
	bool	    sized = args_has(args, 'g') || args_has(args, 's') ||
	    w_val != NULL || h_val != NULL;

	if (sized && (edges_str != NULL || warp)) {
		cmdq_error(cmdq,
		    "window-resize: -e and -W apply only to interactive resize");
		return (CMD_RETURN_ERROR);
	}

	if (args_has(args, 'g')) {
		do_resize_step(view, dir_val, +1, cmdq);
		return (CMD_RETURN_NORMAL);
	}

	if (args_has(args, 's')) {
		do_resize_step(view, dir_val, -1, cmdq);
		return (CMD_RETURN_NORMAL);
	}

	if (w_val != NULL || h_val != NULL) {
		do_set_size(view, w_val, h_val, cmdq);
		return (CMD_RETURN_NORMAL);
	}

	uint32_t edges = COW_EDGE_BOTTOM | COW_EDGE_RIGHT;
	struct cow_seat *seat = cow_wm.current_interaction_seat;
	if (seat == NULL)
		seat = cow_seat_primary();
	if (edges_str != NULL) {
		if (strcmp(edges_str, "automatic") == 0) {
			if (!automatic_edges(view, seat, &edges, cmdq))
				return (CMD_RETURN_ERROR);
			if (edges == 0)
				return (CMD_RETURN_NORMAL);
		} else if (!parse_edges(edges_str, &edges)) {
			cmdq_error(cmdq,
			    "window-resize: -e: bad edge spec '%s' "
			    "(expected n|s|e|w|ne|nw|se|sw|automatic)",
			    edges_str);
			return (CMD_RETURN_ERROR);
		}
	}
	do_interactive(view, edges, warp, edges_str != NULL);
	return (CMD_RETURN_NORMAL);
}

static bool
automatic_edges(struct cow_view *view, struct cow_seat *seat, uint32_t *edges,
    struct cmd_q *cmdq)
{
	if (seat == NULL || !seat->has_pointer_position) {
		cmdq_error(cmdq,
		    "window-resize: automatic direction requires a pointer");
		return false;
	}

	struct cow_box frame = cow_view_frame_box(view);
	int px = seat->pointer_x - frame.x;
	int py = seat->pointer_y - frame.y;
	if (px < 0 || py < 0 || px >= frame.width || py >= frame.height) {
		cmdq_error(cmdq,
		    "window-resize: pointer is outside the target window");
		return false;
	}

	uint32_t found = 0;
	if (px * 3 < frame.width)
		found |= COW_EDGE_LEFT;
	else if (px * 3 >= frame.width * 2)
		found |= COW_EDGE_RIGHT;
	if (py * 3 < frame.height)
		found |= COW_EDGE_TOP;
	else if (py * 3 >= frame.height * 2)
		found |= COW_EDGE_BOTTOM;
	if (found == 0) {
		*edges = 0;
		return true;
	}

	*edges = found;
	return true;
}

static void
resize_warp_position(struct cow_view *view, struct cow_seat *seat,
    uint32_t edges, int *warp_x, int *warp_y)
{
	struct cow_box frame = cow_view_frame_box(view);
	int x = seat->has_pointer_position ? seat->pointer_x :
	    frame.x + frame.width / 2;
	int y = seat->has_pointer_position ? seat->pointer_y :
	    frame.y + frame.height / 2;
	if (x < frame.x)
		x = frame.x;
	else if (x >= frame.x + frame.width)
		x = frame.x + frame.width - 1;
	if (y < frame.y)
		y = frame.y;
	else if (y >= frame.y + frame.height)
		y = frame.y + frame.height - 1;

	if (edges & COW_EDGE_LEFT)
		x = frame.x;
	else if (edges & COW_EDGE_RIGHT)
		x = frame.x + frame.width - 1;
	if (edges & COW_EDGE_TOP)
		y = frame.y;
	else if (edges & COW_EDGE_BOTTOM)
		y = frame.y + frame.height - 1;

	*warp_x = x;
	*warp_y = y;
}

/*
 * Parse a compass edge specifier (case-insensitive: "n", "se",
 * "nw", ...) into the cow_pointer_op edge bitmap.  Used by -e on
 * `window-resize` so the decoration-click binding for each edge /
 * corner can express its drag direction in command form rather
 * than having a separate hard-coded path.
 */
static bool
parse_edges(const char *spec, uint32_t *out)
{
	if (spec == NULL || *spec == '\0')
		return false;
	uint32_t edges = 0;
	for (const char *p = spec; *p != '\0'; p++) {
		switch (*p) {
		case 'n':
		case 'N':
			edges |= COW_EDGE_TOP;
			break;
		case 's':
		case 'S':
			edges |= COW_EDGE_BOTTOM;
			break;
		case 'e':
		case 'E':
			edges |= COW_EDGE_RIGHT;
			break;
		case 'w':
		case 'W':
			edges |= COW_EDGE_LEFT;
			break;
		default:
			return false;
		}
	}
	/* Reject contradictory pairs like "ns" or "ew". */
	if ((edges & COW_EDGE_TOP) && (edges & COW_EDGE_BOTTOM))
		return false;
	if ((edges & COW_EDGE_LEFT) && (edges & COW_EDGE_RIGHT))
		return false;
	if (edges == 0)
		return false;
	*out = edges;
	return true;
}

/*
 * Resize by +/- one step in the given cardinal direction.
 * sign == +1 grows, sign == -1 shrinks.
 */
static void
do_resize_step(struct cow_view *view, const char *dir, int sign,
    struct cmd_q *cmdq)
{
	const char *label = (sign > 0) ? "grow" : "shrink";

	if (dir == NULL) {
		cmdq_error(cmdq, "window-resize %s: -d direction required",
		    label);
		return;
	}

	int step = (cow_config ? cow_config->step : COW_CFG_DEFAULT_STEP) *
	    sign;
	int w = view->content_width;
	int h = view->content_height;
	int x = view->x;
	int y = view->y;
	bool anchor_right = false;
	bool anchor_bottom = false;

	switch (cow_parse_direction(dir)) {
	case COW_DIR_UP:
		h += step;
		y -= step;
		anchor_bottom = true;
		break;
	case COW_DIR_DOWN:
		h += step;
		break;
	case COW_DIR_LEFT:
		w += step;
		x -= step;
		anchor_right = true;
		break;
	case COW_DIR_RIGHT:
		w += step;
		break;
	case COW_DIR_NONE:
	default:
		cmdq_error(cmdq, "window-resize %s: unknown direction: %s",
		    label, dir);
		return;
	}

	if (sign < 0)
		cow_clamp_dimensions(&w, &h);

	cow_view_clear_expanded_state(view);
	cow_view_set_tiled_edges(view, RIVER_WINDOW_V1_EDGES_TOP |
	    RIVER_WINDOW_V1_EDGES_BOTTOM | RIVER_WINDOW_V1_EDGES_LEFT |
	    RIVER_WINDOW_V1_EDGES_RIGHT);
	cow_view_set_pending_geometry_anchored(view, x, y, w, h,
	    anchor_right, anchor_bottom);
	if (!anchor_right && !anchor_bottom)
		cow_view_sync_container_geometry(view, x, y, w, h);
}

static void
do_set_size(struct cow_view *view, const char *ws, const char *hs,
    struct cmd_q *cmdq)
{
	int	  w = view->content_width;
	int	  h = view->content_height;
	struct cmd_length wl = { 0 }, hl = { 0 };
	int frame_w, frame_h, content_x, content_y;
	cow_titlebar_frame_geometry(view, w, h, &frame_w, &frame_h,
	    &content_x, &content_y);
	int frame_extra_w = frame_w - w;
	int frame_extra_h = frame_h - h;

	if (ws != NULL) {
		if (!cmd_parse_length(cmdq, "window-resize", "width", ws, 0,
			32767, &wl))
			return;
		if (wl.unit == CMD_LENGTH_VH) {
			cmdq_error(cmdq,
			    "window-resize: width: vh is not valid for width");
			return;
		}
		if (!cmd_length_resolve(cmdq, "window-resize", "width", &wl,
			view->output, &w))
			return;
		if (wl.unit == CMD_LENGTH_VW)
			w -= frame_extra_w;
		if (w < 1) {
			cmdq_error(cmdq,
			    "window-resize: width: value is too small for the frame");
			return;
		}
	}
	if (hs != NULL) {
		if (!cmd_parse_length(cmdq, "window-resize", "height", hs, 0,
			32767, &hl))
			return;
		if (hl.unit == CMD_LENGTH_VW) {
			cmdq_error(cmdq,
			    "window-resize: height: vw is not valid for height");
			return;
		}
		if (!cmd_length_resolve(cmdq, "window-resize", "height", &hl,
			view->output, &h))
			return;
		if (hl.unit == CMD_LENGTH_VH)
			h -= frame_extra_h;
		if (h < 1) {
			cmdq_error(cmdq,
			    "window-resize: height: value is too small for the frame");
			return;
		}
	}

	cow_clamp_dimensions(&w, &h);

	cow_view_clear_expanded_state(view);
	cow_view_set_tiled_edges(view, RIVER_WINDOW_V1_EDGES_TOP |
	    RIVER_WINDOW_V1_EDGES_BOTTOM | RIVER_WINDOW_V1_EDGES_LEFT |
	    RIVER_WINDOW_V1_EDGES_RIGHT);
	cow_view_set_pending_geometry(view, view->x, view->y, w, h);
	cow_view_sync_container_geometry(view, view->x, view->y, w, h);
}

static void
do_interactive(struct cow_view *view, uint32_t edges, bool warp,
    bool indicate)
{
	/* Only valid inside a manage sequence — see cmd-move.c */
	if (cow_wm.sequence != COW_WM_SEQUENCE_MANAGE)
		return;
	/*
	 * See cmd-move.c: prefer the seat that initiated the click
	 * (set by wm.c's decoration-click dispatch), falling back to
	 * the primary seat for non-decoration callers like keybindings.
	 */
	struct cow_seat *seat = cow_wm.current_interaction_seat;
	if (seat == NULL)
		seat = cow_seat_primary();
	if (seat == NULL)
		return;
	if (!warp) {
		(void)cow_pointer_op_start_resize(seat, view, edges, indicate);
		return;
	}
	int warp_x, warp_y;
	resize_warp_position(view, seat, edges, &warp_x, &warp_y);
	(void)cow_pointer_op_start_resize_warped(seat, view, edges, indicate,
	    warp_x, warp_y);
}
