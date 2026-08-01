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
/*
 * Command: draw-exec -- draw a frame before running a command.
 *
 * usage: draw-exec [-F expr] command ...
 */

#include "cow/cmd.h"
#include "cow/cmd-filter.h"
#include "cow/draw_exec.h"
#include "cow/memory.h"
#include "cow/pointer_op.h"
#include "cow/seat.h"
#include "cow/wm.h"

static enum cmd_retval cmd_draw_exec_exec(struct cmd *, struct cmd_q *);

struct cmd_entry cmd_draw_exec_entry = {
	"draw-exec",
	"F:",
	1,
	-1,
	"draw-exec [-F expr] command ...",
	cmd_draw_exec_exec,
	0,
};

static enum cmd_retval
cmd_draw_exec_exec(struct cmd *self, struct cmd_q *cmdq)
{
	struct args *args = self->args;
	struct cow_seat *seat = cow_wm.current_interaction_seat;
	struct cmd_filter filter;
	char *cmd_str;

	if (cow_wm.sequence != COW_WM_SEQUENCE_MANAGE) {
		cmdq_error(cmdq, "draw-exec: not in manage sequence");
		return CMD_RETURN_ERROR;
	}

	if (seat == NULL || seat->removed || !seat->has_pointer_position)
		seat = cow_seat_primary();
	if (seat == NULL || !seat->has_pointer_position)
		seat = cow_seat_first_with_pointer();
	if (seat == NULL || !seat->has_pointer_position) {
		cmdq_error(cmdq, "draw-exec: no pointer seat");
		return CMD_RETURN_ERROR;
	}
	if (seat->op_in_progress) {
		cmdq_error(cmdq, "draw-exec: pointer operation in progress");
		return CMD_RETURN_ERROR;
	}

	if (!cmd_filter_parse(&filter, cmdq, "draw-exec", "-F",
		args_get(args, 'F')))
		return CMD_RETURN_ERROR;

	cmd_str = cow_draw_exec_make_command_string(args->argc, args->argv);
	if (!cow_draw_exec_begin(seat, cmd_str, &filter)) {
		cmd_filter_free(&filter);
		xfree(cmd_str);
		cmdq_error(cmdq, "draw-exec: cannot start draw operation");
		return CMD_RETURN_ERROR;
	}
	xfree(cmd_str);

	cow_pointer_op_start_draw_exec(seat);
	return CMD_RETURN_NORMAL;
}
