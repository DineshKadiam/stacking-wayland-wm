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
#include "cow/view.h"
#include "cow/wm.h"

static enum cmd_retval cmd_resize_exec(struct cmd *, struct cmd_q *);
static void	       do_resize_step(struct cow_view *, const char *, int,
	       struct cmd_q *);
static void do_set_size(struct cow_view *, const char *, const char *,
    struct cmd_q *);
static void do_interactive(struct cow_view *, uint32_t edges);
static bool parse_edges(const char *, uint32_t *);

struct cmd_entry cmd_resize_entry = {
	"window-resize",
	"t:gsd:w:h:e:",
	0,
	0,
	"[-gs] [-d dir] [-e edges] [-w width] [-h height]",
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

	uint32_t    edges     = COW_EDGE_BOTTOM | COW_EDGE_RIGHT;
	const char *edges_str = args_get(args, 'e');
	if (edges_str != NULL && !parse_edges(edges_str, &edges)) {
		cmdq_error(cmdq,
		    "window-resize: -e: bad edge spec '%s' "
		    "(expected n|s|e|w|ne|nw|se|sw)",
		    edges_str);
		return (CMD_RETURN_ERROR);
	}
	do_interactive(view, edges);
	return (CMD_RETURN_NORMAL);
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

	switch (cow_parse_direction(dir)) {
	case COW_DIR_UP:
		h += step;
		y -= step;
		break;
	case COW_DIR_DOWN:
		h += step;
		break;
	case COW_DIR_LEFT:
		w += step;
		x -= step;
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

	cow_view_set_pending_geometry(view, x, y, w, h);
	cow_view_sync_container_geometry(view, x, y, w, h);
}

static void
do_set_size(struct cow_view *view, const char *ws, const char *hs,
    struct cmd_q *cmdq)
{
	int	  w = view->content_width;
	int	  h = view->content_height;
	long long ll;

	if (ws != NULL) {
		if (!cmd_parse_int(cmdq, "window-resize", "width", ws, 1, 32767,
			&ll))
			return;
		w = (int)ll;
	}
	if (hs != NULL) {
		if (!cmd_parse_int(cmdq, "window-resize", "height", hs, 1,
			32767, &ll))
			return;
		h = (int)ll;
	}

	cow_clamp_dimensions(&w, &h);

	cow_view_set_pending_geometry(view, view->x, view->y, w, h);
	cow_view_sync_container_geometry(view, view->x, view->y, w, h);
}

static void
do_interactive(struct cow_view *view, uint32_t edges)
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
	if (seat != NULL)
		cow_pointer_op_start_resize(seat, view, edges);
}
