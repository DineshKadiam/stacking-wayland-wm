/*
 * Copyright (c) 2013 Nicholas Marriott <nicm@users.sourceforge.net>
 * Copyright (c) 2026 Thomas Adam <thomas@xteddy.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF MIND, USE, DATA OR PROFITS, WHETHER
 * IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING
 * OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/types.h>

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cow/cmd.h"
#include "cow/output.h"

/* Create new command queue. */
struct cmd_q *
cmdq_new(void)
{
	struct cmd_q *cmdq;

	cmdq		 = xcalloc(1, sizeof *cmdq);
	cmdq->references = 1;
	cmdq->dead	 = 0;

	TAILQ_INIT(&cmdq->queue);
	cmdq->item = NULL;
	cmdq->cmd  = NULL;

	return (cmdq);
}

/* Free command queue */
int
cmdq_free(struct cmd_q *cmdq)
{
	if (--cmdq->references != 0)
		return (cmdq->dead);

	cmdq_flush(cmdq);
	xfree(cmdq->event_json);
	xfree(cmdq);
	return (1);
}

/*
 * Attach (or replace) the per-event JSON payload on the queue.
 * Takes ownership of `json` (caller must not free).
 */
void
cmdq_set_event_json(struct cmd_q *cmdq, char *json)
{
	if (cmdq == NULL)
		return;
	xfree(cmdq->event_json);
	cmdq->event_json = json;
}

/* Show error from command. */
void printflike2
cmdq_error(struct cmd_q *cmdq, const char *fmt, ...)
{
	struct cmd *cmd = cmdq->cmd;
	va_list	    ap;
	char	   *msg, *cause;

	va_start(ap, fmt);
	vasprintf(&msg, fmt, ap);
	va_end(ap);

	xasprintf(&cause, "%s:%u: %s", cmd->file, cmd->line, msg);
	ARRAY_ADD(&cfg_causes, cause);

	xfree(msg);
}

/* Add command list to queue and begin processing if needed. */
void
cmdq_run(struct cmd_q *cmdq, struct cmd_list *cmdlist)
{
	cmdq_append(cmdq, cmdlist);

	if (cmdq->item == NULL) {
		cmdq->cmd = NULL;
		cmdq_continue(cmdq);
	}
}

/* Add command list to queue. */
void
cmdq_append(struct cmd_q *cmdq, struct cmd_list *cmdlist)
{
	struct cmd_q_item *item;

	item	      = xcalloc(1, sizeof *item);
	item->cmdlist = cmdlist;
	TAILQ_INSERT_TAIL(&cmdq->queue, item, qentry);
	cmdlist->references++;
}

/* Continue processing command queue. Returns 1 if finishes empty. */
int
cmdq_continue(struct cmd_q *cmdq)
{
	struct cmd_q_item *next;
	enum cmd_retval	   retval;
	int		   empty;

	empty = TAILQ_EMPTY(&cmdq->queue);
	if (empty)
		goto empty;

	if (cmdq->item == NULL) {
		cmdq->item = TAILQ_FIRST(&cmdq->queue);
		cmdq->cmd  = TAILQ_FIRST(&cmdq->item->cmdlist->list);
	} else
		cmdq->cmd = TAILQ_NEXT(cmdq->cmd, qentry);

	do {
		next = TAILQ_NEXT(cmdq->item, qentry);

		while (cmdq->cmd != NULL) {
			/* Update the current output based on pointer
			 * position.
			 *
			 * We want to ensure the pointer is deriving the
			 * correct output.
			 */
			cow_output_update_current();

			cmdq->time = time(NULL);
			cmdq->number++;

			/* Resolve -t/-s context specifiers.
			 *
			 * Commands marked CMD_OWN_TS (e.g. `rule`) interpret
			 * -t / -s with their own semantics and must not have
			 * them pre-resolved as ctx specifiers, because the
			 * referenced view typically does not exist at
			 * registration time. */
			memset(&cmdq->target, 0, sizeof(cmdq->target));
			memset(&cmdq->source, 0, sizeof(cmdq->source));

			bool own_ts = (cmdq->cmd->entry->flags & CMD_OWN_TS) !=
			    0;

			const char *t_spec    = own_ts ?
			       NULL :
			       args_get(cmdq->cmd->args, 't');
			const char *s_spec    = own_ts ?
			       NULL :
			       args_get(cmdq->cmd->args, 's');
			char	   *ctx_cause = NULL;

			if (t_spec != NULL) {
				if (!cmd_ctx_parse(t_spec, &cmdq->target,
					&ctx_cause)) {
					cmdq_error(cmdq, "-t: %s", ctx_cause);
					xfree(ctx_cause);
					retval = CMD_RETURN_ERROR;
					break;
				}
			} else if (!own_ts) {
				/*
				 * No -t given.  If a default target was
				 * seeded on the queue (by the rule
				 * subsystem), inherit it so commands inside
				 * a rule body see the event's view / desk /
				 * output without having to spell out -t on
				 * every line.
				 */
				cmdq->target = cmdq->default_target;
			}
			if (s_spec != NULL) {
				if (!cmd_ctx_parse(s_spec, &cmdq->source,
					&ctx_cause)) {
					cmdq_error(cmdq, "-s: %s", ctx_cause);
					xfree(ctx_cause);
					retval = CMD_RETURN_ERROR;
					break;
				}
			}

			retval = cmdq->cmd->entry->exec(cmdq->cmd, cmdq);

			if (retval == CMD_RETURN_ERROR)
				break;

			cmdq->cmd = TAILQ_NEXT(cmdq->cmd, qentry);
		}

		TAILQ_REMOVE(&cmdq->queue, cmdq->item, qentry);
		cmd_list_free(cmdq->item->cmdlist);
		xfree(cmdq->item);

		cmdq->item = next;
		if (cmdq->item != NULL)
			cmdq->cmd = TAILQ_FIRST(&cmdq->item->cmdlist->list);
	} while (cmdq->item != NULL);

empty:
	if (cmdq->emptyfn != NULL)
		cmdq->emptyfn(cmdq); /* may free cmdq */
	empty = 1;

	return (empty);
}

/* Flush command queue. */
void
cmdq_flush(struct cmd_q *cmdq)
{
	struct cmd_q_item *item, *item1;

	TAILQ_FOREACH_SAFE (item, &cmdq->queue, qentry, item1) {
		TAILQ_REMOVE(&cmdq->queue, item, qentry);
		cmd_list_free(item->cmdlist);
		xfree(item);
	}
	cmdq->item = NULL;
}

void
cmdq_set_default_target(struct cmd_q *cmdq, struct cow_view *window,
    struct cow_desk *desk, struct cow_output *output)
{
	cmdq->default_target.window = window;
	cmdq->default_target.desk   = desk;
	cmdq->default_target.output = output;
}
