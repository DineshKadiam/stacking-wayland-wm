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

#include "cow/cmd.h"
#include "cow/config.h"
#include "cow/memory.h"
#include "cow/view.h"

static enum cmd_retval cmd_bind_decoration_exec(struct cmd *, struct cmd_q *);

struct cmd_entry cmd_bind_decoration_entry = {
	"bind-decoration",
	"",
	2,
	-1,
	"bind-decoration REGION command [args ...]",
	cmd_bind_decoration_exec,
	0
};

static enum cmd_retval
cmd_bind_decoration_exec(struct cmd *self, struct cmd_q *cmdq)
{
	struct args	   *args   = self->args;
	enum cow_dec_region region = cow_dec_region_parse(args->argv[0]);

	if (region == COW_DEC_REGION_NONE) {
		cmdq_error(cmdq,
		    "bind-decoration: unknown region '%s' "
		    "(expected N|S|E|W|NE|NW|SE|SW|titlebar)",
		    args->argv[0]);
		return (CMD_RETURN_ERROR);
	}

	/*
	 * Overwrite any existing binding for the same (region, clicks)
	 * pair.
	 */
	struct cow_dec_binding_entry *existing, *tmp;
	wl_list_for_each_safe(existing, tmp, &cow_config->decoration_bindings,
	    link) {
		if (existing->region == region && existing->clicks == clicks) {
			wl_list_remove(&existing->link);
			xfree(existing->action_string);
			xfree(existing);
		}
	}

	struct cow_dec_binding_entry *entry = xcalloc(1, sizeof(*entry));
	entry->region			    = region;
	entry->action_string = cmd_argv_to_string(args->argc, args->argv, 1);

	wl_list_insert(&cow_config->decoration_bindings, &entry->link);

	return (CMD_RETURN_NORMAL);
}

/*
 * Insert a (region, clicks) binding directly, bypassing the cmd
 * surface.  
 */
static void
register_default(enum cow_dec_region region, int clicks, const char *action)
{
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
	 * fallback.
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
	register_default(COW_DEC_REGION_TITLEBAR, 2, "window-shade -d north");
}

bool
cow_dec_binding_fire(struct cow_view *view, enum cow_dec_region region)
{
	if (view == NULL || view->closed)
		return false;
	const char *cmd_str = cow_dec_binding_lookup(region);
	if (cmd_str == NULL)
		return false;

	/*
	 * Parse + run the bound command string with the clicked view
	 * as the implicit -t target.
	 */
	struct cmd_list *cmdlist = NULL;
	char		*cause	 = NULL;
	if (cmd_string_parse(cmd_str, &cmdlist, NULL, 0, &cause) != 0) {
		if (cause != NULL) {
			fprintf(stderr,
			    "cow: bind-decoration command error: %s\n", cause);
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
