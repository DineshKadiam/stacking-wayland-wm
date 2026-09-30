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
#include "cow/alias.h"
#include "cow/dsl.h"
#include "cow/log.h"
#include "cow/config.h"
#include "cow/memory.h"
#include "cow/pointer_op.h"
#include "cow/seat.h"
#include "cow/view.h"
#include "cow/wm.h"
#include "dsl-ast.h"

#include "river-window-management-v1-client-protocol.h"

static enum cmd_retval cmd_bind_decoration_exec(struct cmd *, struct cmd_q *);

/* Only inspect command positions: mentioning a command in a comment,
 * message, or a newly defined binding must not give this button its role.
 * A depth limit also bounds recursive aliases and nested script blocks.
 */
#define BUTTON_ACTION_DEPTH 32
static bool action_can_maximize(const char *, unsigned int);

static bool
script_can_maximize(const struct dsl_node *node, unsigned int depth)
{
	if (node == NULL || depth >= BUTTON_ACTION_DEPTH)
		return false;
	switch (node->kind) {
	case DSL_N_BLOCK:
		for (size_t i = 0; i < node->u.block.n; i++) {
			if (script_can_maximize(node->u.block.stmts[i], depth + 1))
				return true;
		}
		return false;
	case DSL_N_IF:
		return script_can_maximize(node->u.ifstmt.body, depth + 1) ||
		    script_can_maximize(node->u.ifstmt.otherwise, depth + 1);
	case DSL_N_FOR:
		return script_can_maximize(node->u.forstmt.body, depth + 1);
	case DSL_N_CMD:
		if (strcmp(node->u.cmd.verb, "window-maximize") == 0)
			return true;
		if (strcmp(node->u.cmd.verb, "script") == 0 && node->u.cmd.n == 1) {
			const struct dsl_cmd_arg *arg = &node->u.cmd.args[0];
			size_t size = 1;
			for (size_t i = 0; i < arg->n; i++) {
				if (!arg->pieces[i].is_raw)
					return false;
				size += strlen(arg->pieces[i].u.raw);
			}
			char *body = xcalloc(size, 1), *next = body;
			for (size_t i = 0; i < arg->n; i++) {
				size_t len = strlen(arg->pieces[i].u.raw);
				memcpy(next, arg->pieces[i].u.raw, len);
				next += len;
			}
			struct dsl_error error = { 0 };
			struct dsl_ast *ast = dsl_parse(body, NULL, &error);
			bool found = ast != NULL &&
			    script_can_maximize(ast->root, depth + 1);
			dsl_free(ast);
			dsl_error_fini(&error);
			xfree(body);
			return found;
		}
		struct cow_alias *alias = cow_alias_find(node->u.cmd.verb);
		return alias != NULL &&
		    action_can_maximize(alias->command, depth + 1);
	default:
		return false;
	}
}

static bool
action_can_maximize(const char *action, unsigned int depth)
{
	if (action == NULL || depth >= BUTTON_ACTION_DEPTH)
		return false;
	struct cmd_list *list = NULL;
	char *cause = NULL;
	bool found = false;
	if (cmd_string_parse(action, &list, NULL, 0, &cause) == 0 && list != NULL) {
		struct cmd *cmd;
		TAILQ_FOREACH(cmd, &list->list, qentry) {
			if (strcmp(cmd->entry->name, "window-maximize") == 0)
				found = true;
			else if (strcmp(cmd->entry->name, "script") == 0) {
				char *body = cmd_argv_to_string(cmd->args->argc,
				    cmd->args->argv, 0);
				struct dsl_error error = { 0 };
				struct dsl_ast *ast = dsl_parse(body, NULL, &error);
				found = ast != NULL &&
				    script_can_maximize(ast->root, depth + 1);
				dsl_free(ast);
				dsl_error_fini(&error);
				xfree(body);
			} else if (cmd->invoked_name != NULL) {
				struct cow_alias *alias = cow_alias_find(cmd->invoked_name);
				found = alias != NULL &&
				    action_can_maximize(alias->command, depth + 1);
			}
			if (found)
				break;
		}
	}
	if (list != NULL)
		cmd_list_free(list);
	xfree(cause);
	return found;
}

bool
cow_dec_binding_can_maximize(const char *action)
{
	return action_can_maximize(action, 0);
}

static void
refresh_maximize_buttons(void)
{
	int button = cow_view_maximize_button_index();
	struct cow_view *view;
	wl_list_for_each(view, &cow_wm.views, wm_link) {
		if (view->maximized && view->icon_inverted_button != button) {
			view->icon_inverted_button = button;
			view->dec.dirty = true;
		}
	}
}

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
	cmd_bind_decoration_exec, CMD_CONFIG };

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
	refresh_maximize_buttons();

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
	if (seat->pane_drag) {
		seat->op.cancelled = true;
		seat->op_released  = true;
		cow_pointer_op_process(seat);
		return;
	}
	if (seat->op.type == COW_POINTER_OP_RESIZE && seat->op.view != NULL &&
	    !seat->op.view->closed) {
		river_window_v1_inform_resize_end(
		    seat->op.view->river_window);
		seat->op.view->resize_indicator_edges = 0;
		seat->op.view->dec.dirty = true;
	}
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
