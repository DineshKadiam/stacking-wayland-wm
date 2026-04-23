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
static void	       do_grow(struct cow_view *, const char *, struct cmd_q *);
static void do_shrink(struct cow_view *, const char *, struct cmd_q *);
static void do_set_size(struct cow_view *, const char *, const char *,
    struct cmd_q *);
static void do_interactive(struct cow_view *);

struct cmd_entry cmd_resize_entry = {
	"window-resize",
	"t:gsd:w:h:",
	0,
	0,
	"[-gs] [-d dir] [-w width] [-h height]",
	cmd_resize_exec,
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
		do_grow(view, dir_val, cmdq);
		return (CMD_RETURN_NORMAL);
	}

	if (args_has(args, 's')) {
		do_shrink(view, dir_val, cmdq);
		return (CMD_RETURN_NORMAL);
	}

	if (w_val != NULL || h_val != NULL) {
		do_set_size(view, w_val, h_val, cmdq);
		return (CMD_RETURN_NORMAL);
	}

	do_interactive(view);
	return (CMD_RETURN_NORMAL);
}

static void
do_grow(struct cow_view *view, const char *dir, struct cmd_q *cmdq)
{
	if (dir == NULL) {
		cmdq_error(cmdq, "window-resize grow: -d direction required");
		return;
	}

	int step = cow_config ? cow_config->step : 100;
	int w	 = view->content_width;
	int h	 = view->content_height;
	int x = view->x, y = view->y;

	if (strcmp(dir, "up") == 0) {
		h += step;
		y -= step;
	} else if (strcmp(dir, "down") == 0) {
		h += step;
	} else if (strcmp(dir, "left") == 0) {
		w += step;
		x -= step;
	} else if (strcmp(dir, "right") == 0) {
		w += step;
	} else {
		cmdq_error(cmdq, "window-resize grow: unknown direction: %s",
		    dir);
		return;
	}

	cow_view_set_pending_geometry(view, x, y, w, h);
	cow_view_sync_container_geometry(view, x, y, w, h);
}

static void
do_shrink(struct cow_view *view, const char *dir, struct cmd_q *cmdq)
{
	if (dir == NULL) {
		cmdq_error(cmdq, "window-resize shrink: -d direction required");
		return;
	}

	int step = cow_config ? cow_config->step : 100;
	int w	 = view->content_width;
	int h	 = view->content_height;
	int x = view->x, y = view->y;

	if (strcmp(dir, "up") == 0) {
		h -= step;
		y += step;
	} else if (strcmp(dir, "down") == 0) {
		h -= step;
	} else if (strcmp(dir, "left") == 0) {
		w -= step;
		x += step;
	} else if (strcmp(dir, "right") == 0) {
		w -= step;
	} else {
		cmdq_error(cmdq, "window-resize shrink: unknown direction: %s",
		    dir);
		return;
	}

	if (w < 50)
		w = 50;
	if (h < 50)
		h = 50;

	cow_view_set_pending_geometry(view, x, y, w, h);
	cow_view_sync_container_geometry(view, x, y, w, h);
}

static void
do_set_size(struct cow_view *view, const char *ws, const char *hs,
    struct cmd_q *cmdq)
{
	const char *errstr;
	int	    w = view->content_width;
	int	    h = view->content_height;

	if (ws != NULL) {
		w = (int)strtonum(ws, 1, 32767, &errstr);
		if (errstr != NULL) {
			cmdq_error(cmdq, "window-resize: bad width: %s", ws);
			return;
		}
	}
	if (hs != NULL) {
		h = (int)strtonum(hs, 1, 32767, &errstr);
		if (errstr != NULL) {
			cmdq_error(cmdq, "window-resize: bad height: %s", hs);
			return;
		}
	}

	if (w < 50)
		w = 50;
	if (h < 50)
		h = 50;

	cow_view_set_pending_geometry(view, view->x, view->y, w, h);
	cow_view_sync_container_geometry(view, view->x, view->y, w, h);
}

static void
do_interactive(struct cow_view *view)
{
	/* Only valid inside a manage sequence — see cmd-move.c */
	if (cow_wm.sequence != COW_WM_SEQUENCE_MANAGE)
		return;
	struct cow_seat *seat = cow_seat_primary();
	if (seat != NULL)
		cow_pointer_op_start_resize(seat, view,
		    COW_EDGE_BOTTOM | COW_EDGE_RIGHT);
}
