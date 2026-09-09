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

#include "cow/cmd.h"
#include "cow/selection.h"
#include "cow/view.h"

static enum cmd_retval cmd_window_select_exec(struct cmd *, struct cmd_q *);

struct cmd_entry cmd_window_select_entry = {
	"window-select",
	"t:n:",
	0,
	1,
	"window-select [-t target] [-n name] [add|remove|toggle|clear]",
	cmd_window_select_exec,
	0,
};

static bool
selection_name_valid(struct cmd_q *cmdq, const char *command, const char *name)
{
	if (name == NULL)
		return true;
	if (name[0] != '\0')
		return true;
	cmdq_error(cmdq, "%s: selection name cannot be empty", command);
	return false;
}

static enum cmd_retval
cmd_window_select_exec(struct cmd *self, struct cmd_q *cmdq)
{
	struct args *args = self->args;
	const char  *name = args_get(args, 'n');
	const char  *op = args->argc == 0 ? "toggle" : args->argv[0];

	if (!selection_name_valid(cmdq, "window-select", name))
		return CMD_RETURN_ERROR;
	if (strcmp(op, "clear") == 0) {
		cow_selection_clear(name);
		return CMD_RETURN_NORMAL;
	}

	struct cow_view *view = cmd_require_target_window(cmdq,
	    "window-select");
	if (view == NULL)
		return CMD_RETURN_ERROR;

	if (strcmp(op, "add") == 0)
		(void)cow_selection_add(view, name);
	else if (strcmp(op, "remove") == 0)
		(void)cow_selection_remove(view, name);
	else if (strcmp(op, "toggle") == 0)
		(void)cow_selection_toggle(view, name);
	else {
		cmdq_error(cmdq, "window-select: unknown operation: %s", op);
		return CMD_RETURN_ERROR;
	}
	return CMD_RETURN_NORMAL;
}
