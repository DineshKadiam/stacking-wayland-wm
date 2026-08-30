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
 * Command: exec -- execute a shell command.
 *
 * During startup (config loading), exec commands are deferred
 * until the WM is fully connected.  Call cow_exec_run_deferred()
 * after init to execute them.
 *
 * usage: exec command ...
 */

#include <stdlib.h>
#include <string.h>

#include "cow/cmd.h"
#include "cow/command.h"
#include "cow/ipc.h"
#include "cow/memory.h"

static enum cmd_retval cmd_exec_exec(struct cmd *, struct cmd_q *);

struct cmd_entry cmd_exec_entry = {
	"exec",
	"",
	1,
	-1,
	"exec command ...",
	cmd_exec_exec,
	CMD_CONFIG_STARTUP,
};

/* Deferred exec list for startup mode. */
static char **deferred_execs;
static int    num_deferred;
static int    deferred_cap;

static enum cmd_retval
cmd_exec_exec(struct cmd *self, struct cmd_q *cmdq)
{
	struct args *args    = self->args;
	char	    *command = cmd_argv_to_string(args->argc, args->argv, 0);

	if (cow_ipc.startup_mode) {
		/* Defer until WM is connected */
		if (num_deferred >= deferred_cap) {
			deferred_cap   = deferred_cap ? deferred_cap * 2 : 8;
			deferred_execs = xrealloc(deferred_execs, deferred_cap,
			    sizeof(char *));
		}
		deferred_execs[num_deferred++] = command;
	} else {
		/*
		 * Forward the per-event JSON payload (if any) from the
		 * cmd_q so the spawned shell can read it as $COW_EVENT.
		 * Set by the rule subsystem for events that carry a
		 * structured payload (e.g. on-layer-change).
		 */
		cow_command_execute_with_event(command, cmdq->event_json);
		xfree(command);
	}

	return (CMD_RETURN_NORMAL);
}

void
cow_exec_run_deferred(void)
{
	for (int i = 0; i < num_deferred; i++) {
		cow_command_execute(deferred_execs[i]);
		xfree(deferred_execs[i]);
	}
	xfree(deferred_execs);
	deferred_execs = NULL;
	num_deferred   = 0;
	deferred_cap   = 0;
}
