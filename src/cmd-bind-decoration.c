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
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "cow/cmd.h"
#include "cow/log.h"
#include "cow/config.h"
#include "cow/memory.h"
#include "cow/pointer_op.h"
#include "cow/seat.h"
#include "cow/view.h"
#include "cow/wm.h"

#include "river-window-management-v1-client-protocol.h"

static enum cmd_retval cmd_bind_decoration_exec(struct cmd *, struct cmd_q *);

/*
 * bind-decoration uses an empty getopt template because the bound
 * command's argv (everything after REGION) contains its own flags
 * (-e, -d, -m, ...) that must NOT be interpreted by bind-decoration's
 * own option parser.  We parse the optional leading -2 flag manually
 * by inspecting args->argv[0].
 *
 * Accepted shapes:
 *   bind-decoration REGION cmd [args...]
 *   bind-decoration -2 REGION cmd [args...]
 * REGION may also be a titlebar button number from 0 through 9.
 */
struct cmd_entry cmd_bind_decoration_entry = { "bind-decoration", "", 2, -1,
	"bind-decoration [-2] REGION command [args ...]",
	cmd_bind_decoration_exec, 0 };

static enum cmd_retval
cmd_bind_decoration_exec(struct cmd *self, struct cmd_q *cmdq)
{
	struct args *args   = self->args;
	int	     clicks = 1;
	int	     idx    = 0;

	if (args->argc >= 1 && strcmp(args->argv[0], "-2") == 0) {
		clicks = 2;
		idx    = 1;
	}

	if (args->argc < idx + 2) {
		cmdq_error(cmdq,
		    "bind-decoration: usage: [-2] REGION command [args ...]");
		return (CMD_RETURN_ERROR);
	}

	enum cow_dec_region region = cow_dec_region_parse(args->argv[idx]);
	if (region == COW_DEC_REGION_NONE) {
		cmdq_error(cmdq,
		    "bind-decoration: unknown region '%s' "
		    "(expected N|S|E|W|NE|NW|SE|SW|titlebar|0..9)",
		    args->argv[idx]);
		return (CMD_RETURN_ERROR);
	}

	/*
	 * Overwrite any existing binding for the same (region, clicks)
	 * pair.
	 */
	struct cow_dec_binding_entry *existing, *tmp;
	wl_list_for_each_safe(existing, tmp, &cow_config->decoration_bindings,
	    link)
	{
		if (existing->region == region && existing->clicks == clicks) {
			wl_list_remove(&existing->link);
			xfree(existing->action_string);
			xfree(existing);
		}
	}

	struct cow_dec_binding_entry *entry = xcalloc(1, sizeof(*entry));
	entry->region			    = region;
	entry->clicks			    = clicks;
	entry->action_string = cmd_argv_to_action_string(args->argc,
	    args->argv, idx + 1);

	wl_list_insert(&cow_config->decoration_bindings, &entry->link);

	return (CMD_RETURN_NORMAL);
}

/*
 * Insert a (region, clicks) binding directly, bypassing the cmd
 * surface.  Skips entries the user has already defined.
 */
static void
register_default(enum cow_dec_region region, int clicks, const char *action)
{
	if (cow_dec_binding_lookup(region, clicks) != NULL)
		return;

	struct cow_dec_binding_entry *entry = xcalloc(1, sizeof(*entry));
	entry->region			    = region;
	entry->clicks			    = clicks;
	entry->action_string		    = xstrdup(action);

	wl_list_insert(&cow_config->decoration_bindings, &entry->link);
}

void
cow_decoration_register_default_bindings(void)
{
	if (cow_config == NULL)
		return;

	/*
	 * Built-in defaults.  Each is expressible as a normal cow
	 * command, so the wm.c dispatch can call cow_dec_binding_fire
	 * uniformly and never has to know which built-in to run as a
	 * fallback.  Per (region, clicks) pair: only install if the
	 * user hasn't already declared one.
	 */
	register_default(COW_DEC_REGION_N, 1, "window-resize -e n");
	register_default(COW_DEC_REGION_S, 1, "window-resize -e s");
	register_default(COW_DEC_REGION_E, 1, "window-resize -e e");
	register_default(COW_DEC_REGION_W, 1, "window-resize -e w");
	register_default(COW_DEC_REGION_NE, 1, "window-resize -e ne");
	register_default(COW_DEC_REGION_NW, 1, "window-resize -e nw");
	register_default(COW_DEC_REGION_SE, 1, "window-resize -e se");
	register_default(COW_DEC_REGION_SW, 1, "window-resize -e sw");
	register_default(COW_DEC_REGION_TITLEBAR, 1, "window-move");
	register_default(COW_DEC_REGION_TITLEBAR, 2,
	    "window-shade -d titlebar");
	register_default(COW_DEC_REGION_BUTTON_0, 1, "window-maximize -T");
	register_default(COW_DEC_REGION_BUTTON_1, 1, "winops -c");
	register_default(COW_DEC_REGION_BUTTON_2, 1, "winops -i");
}

static bool
run_cmd_string(struct cow_view *view, const char *cmd_str)
{
	struct cmd_list *cmdlist = NULL;
	char		*cause	 = NULL;
	if (cmd_string_parse(cmd_str, &cmdlist, NULL, 0, &cause) != 0) {
		if (cause != NULL) {
			cow_log_error("bind-decoration command: %s", cause);
			xfree(cause);
		}
		return false;
	}
	if (cmdlist == NULL)
		return false;

	struct cmd_q *cmdq = cmdq_new();
	cmdq->client_fd	   = -1;
	cmdq_set_default_target(cmdq, view, view->desk, view->output);
	cmdq_run(cmdq, cmdlist);
	cmd_list_free(cmdlist);
	cmdq_free(cmdq);
	return true;
}

bool
cow_dec_binding_fire(struct cow_view *view, enum cow_dec_region region,
    int clicks)
{
	if (view == NULL || view->closed)
		return false;
	const char *cmd_str = cow_dec_binding_lookup(region, clicks);
	if (cmd_str == NULL)
		return false;
	return run_cmd_string(view, cmd_str);
}

/*
 * Cancel an in-flight pointer op started by a 1-click bind-decoration
 * action.  A 2-click on the same view+region within DOUBLE_CLICK_MS
 * "claims" the second press: the op (begun on the first press for
 * press-and-hold semantics) is ended without committing any motion,
 * and the 2-click binding runs against the pre-press state.
 *
 * window-move defers its unmaximise side-effect until the first
 * non-zero motion event (see cow_pointer_op_process), so cancelling
 * a press-only op leaves the view geometry untouched.
 */
static void
cancel_first_press_op(struct cow_seat *seat)
{
	if (!seat->op_in_progress)
		return;
	river_seat_v1_op_end(seat->river_seat);
	seat->op_in_progress = false;
	seat->op.type	     = COW_POINTER_OP_NONE;
	seat->op.view	     = NULL;
	seat->op_released    = false;
	seat->op_dx	     = 0;
	seat->op_dy	     = 0;
}

bool
cow_dec_binding_dispatch(struct cow_seat *seat, struct cow_view *view,
    enum cow_dec_region region, int clicks)
{
	if (view == NULL || view->closed || seat == NULL)
		return false;

	if (clicks == 2) {
		cancel_first_press_op(seat);
		const char *cmd_str = cow_dec_binding_lookup(region, 2);
		if (cmd_str != NULL)
			return run_cmd_string(view, cmd_str);
		const char *one = cow_dec_binding_lookup(region, 1);
		return one != NULL ? run_cmd_string(view, one) : false;
	}

	const char *cmd_str = cow_dec_binding_lookup(region, 1);
	if (cmd_str == NULL)
		return false;
	return run_cmd_string(view, cmd_str);
}
