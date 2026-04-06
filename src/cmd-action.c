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
 * Command: action -- define a named action (user command).
 *
 * usage: action name command ...
 */

#include "cow/cmd.h"
#include "cow/config.h"
#include "cow/memory.h"

static enum cmd_retval cmd_action_exec(struct cmd *, struct cmd_q *);

struct cmd_entry cmd_action_entry = { "action", "", 2, -1,
	"action name command ...", cmd_action_exec };

static enum cmd_retval
cmd_action_exec(struct cmd *self, __unused struct cmd_q *cmdq)
{
	struct args *args = self->args;

	struct cow_action_entry *entry = xcalloc(1, sizeof(*entry));
	entry->name = xstrdup(args->argv[0]);
	entry->command = cmd_argv_to_string(args->argc, args->argv, 1);

	/* TODO -- verify? */
	wl_list_insert(&cow_config->actions, &entry->link);

	return (CMD_RETURN_NORMAL);
}
