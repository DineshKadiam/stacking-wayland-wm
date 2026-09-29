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

#include "cow/cmd.h"
#include "cow/memory.h"
#include "cow/view.h"
#include "cow/wm.h"

static enum cmd_retval cmd_container_exec(struct cmd *, struct cmd_q *);

struct cmd_entry cmd_container_entry = {
	"window-container",
	"t:s:e:f:arcqnpNSPHU",
	0,
	0,
	"[-t target] [-s source] [-a | -e direction | -f direction | -rcqnpNPHU]",
	cmd_container_exec,
	0,
};

static enum cmd_retval
cmd_container_exec(struct cmd *self, struct cmd_q *cmdq)
{
	struct args *args = self->args;

	/* Resolve the target window (-t or focused) */
	struct cow_view *target = cmd_require_target_window(cmdq, "container");
	if (target == NULL)
		return (CMD_RETURN_ERROR);
	if (args_has(args, 'H') || args_has(args, 'U')) {
		bool ok = args_has(args, 'H') ?
		    cow_container_title_hide(target) :
		    cow_container_titles_restore(target);
		if (!ok)
			cmdq_error(cmdq,
			    "container: cannot change a protected, tabbed or collapsed titlebar");
		return ok ? CMD_RETURN_NORMAL : CMD_RETURN_ERROR;
	}
	if (args_has(args, 'c') || args_has(args, 'q')) {
		bool ok = args_has(args, 'c') ? cow_container_collapse(target) :
						cow_container_equalize(target);
		if (!ok)
			cmdq_error(cmdq, "container: pane cannot be changed");
		return ok ? CMD_RETURN_NORMAL : CMD_RETURN_ERROR;
	}
	if (args_has(args, 'f')) {
		const char	*direction = args_get(args, 'f');
		uint32_t	 edge = strcmp(direction, "left") == 0 ? 4 :
			    strcmp(direction, "right") == 0	       ? 8 :
			    strcmp(direction, "up") == 0	       ? 1 :
			    strcmp(direction, "down") == 0	       ? 2 :
									 0;
		struct cow_view *next = edge ?
		    cow_container_neighbor(target, edge) :
		    NULL;
		if (next == NULL) {
			cmdq_error(cmdq,
			    "container: no pane in that direction");
			return CMD_RETURN_ERROR;
		}
		cow_container_activate(cow_view_container(next),
		    next->container_member);
		return CMD_RETURN_NORMAL;
	}

	if (args_has(args, 'a') || args_has(args, 'e')) {
		const char	       *edge   = args_get(args, 'e');
		enum cow_container_axis axis   = COW_CONTAINER_LEAF;
		bool			before = false;
		if (edge != NULL) {
			if (strcmp(edge, "left") == 0 ||
			    strcmp(edge, "right") == 0)
				axis = COW_CONTAINER_HORIZONTAL;
			else if (strcmp(edge, "up") == 0 ||
			    strcmp(edge, "down") == 0)
				axis = COW_CONTAINER_VERTICAL;
			else {
				cmdq_error(cmdq,
				    "container: expected left, right, up or down");
				return CMD_RETURN_ERROR;
			}
			before = strcmp(edge, "left") == 0 ||
			    strcmp(edge, "up") == 0;
		}
		/* Resolve source window (-s flag, else focused) */
		struct cow_view *source = NULL;
		const char	*s_str	= args_get(args, 's');
		if (s_str != NULL) {
			struct cmd_ctx src_ctx;
			char	      *cause = NULL;
			if (!cmd_ctx_parse(s_str, &src_ctx, &cause)) {
				cmdq_error(cmdq,
				    "container add: bad -s specifier: %s",
				    cause ? cause : "unknown");
				xfree(cause);
				return (CMD_RETURN_ERROR);
			}
			source = src_ctx.window;
		} else {
			source = cow_wm.focused_view;
		}

		if (source == NULL || source->closed) {
			cmdq_error(cmdq, "container add: no source window");
			return (CMD_RETURN_ERROR);
		}

		/*
		 * If no -t was given, target defaulted to focused (same as
		 * source). In that case, use the next focusable window in
		 * stacking order as the target so that -a does the right
		 * thing.
		 */
		if (target == source) {
			target = cow_view_walk(source, true,
			    cow_view_focusable);
			if (target == NULL || target == source) {
				cmdq_error(cmdq,
				    "container add: no other window to group with");
				return (CMD_RETURN_ERROR);
			}
		}

		if (source == target) {
			cmdq_error(cmdq,
			    "container add: source and target are the same window");
			return (CMD_RETURN_ERROR);
		}

		struct cow_container *ctr = cow_view_container(target);
		if (source->parent != NULL || target->parent != NULL ||
		    source->fullscreen || target->fullscreen) {
			cmdq_error(cmdq,
			    "container: cannot group dialogs or fullscreen windows");
			return CMD_RETURN_ERROR;
		}
		if (source->container_member != NULL &&
		    source->container_member->container == ctr) {
			if (edge != NULL &&
			    cow_container_split(target, source, axis, before))
				return CMD_RETURN_NORMAL;
			cmdq_error(cmdq,
			    "container add: source is already in this container");
			return (CMD_RETURN_ERROR);
		}

		/* Keep a failed transfer from destroying the source layout. */
		if (source->container_member != NULL) {
			cmdq_error(cmdq,
			    "container: detach the source from its current group first");
			return CMD_RETURN_ERROR;
		}

		if (ctr != NULL) {
			if (!cow_container_add_to(target, source)) {
				cmdq_error(cmdq,
				    "container: source does not fit");
				return CMD_RETURN_ERROR;
			}
		} else {
			ctr = cow_container_create(target, source);
		}
		if (edge != NULL &&
		    !cow_container_split(target, source, axis, before)) {
			cow_container_remove_view(source);
			cmdq_error(cmdq, "container: split does not fit");
			return CMD_RETURN_ERROR;
		}
		cow_container_activate(ctr, source->container_member);
		return (CMD_RETURN_NORMAL);
	}

	if (args_has(args, 'r')) {
		if (target->container_member == NULL) {
			cmdq_error(cmdq,
			    "container remove: window is not in a container");
			return (CMD_RETURN_ERROR);
		}
		cow_container_remove_view(target);
		return (CMD_RETURN_NORMAL);
	}

	if (target->container_member != NULL &&
	    (args_has(args, 'n') || args_has(args, 'p') ||
		args_has(args, 'N') || args_has(args, 'P')))
		cow_container_activate(cow_view_container(target),
		    target->container_member);

	if (args_has(args, 'n')) {
		struct cow_container *ctr = cow_view_container(target);
		if (ctr == NULL) {
			cmdq_error(cmdq,
			    "container next: window is not in a container");
			return (CMD_RETURN_ERROR);
		}
		cow_container_activate_next(ctr);
		return (CMD_RETURN_NORMAL);
	}

	if (args_has(args, 'p')) {
		struct cow_container *ctr = cow_view_container(target);
		if (ctr == NULL) {
			cmdq_error(cmdq,
			    "container prev: window is not in a container");
			return (CMD_RETURN_ERROR);
		}
		cow_container_activate_prev(ctr);
		return (CMD_RETURN_NORMAL);
	}

	if (args_has(args, 'N')) {
		struct cow_container *ctr = cow_view_container(target);
		if (ctr == NULL) {
			cmdq_error(cmdq,
			    "container swap-next: window is not in a container");
			return (CMD_RETURN_ERROR);
		}
		cow_container_swap_next(ctr);
		return (CMD_RETURN_NORMAL);
	}

	if (args_has(args, 'P')) {
		struct cow_container *ctr = cow_view_container(target);
		if (ctr == NULL) {
			cmdq_error(cmdq,
			    "container swap-prev: window is not in a container");
			return (CMD_RETURN_ERROR);
		}
		cow_container_swap_prev(ctr);
		return (CMD_RETURN_NORMAL);
	}

	cmdq_error(cmdq, "container: unknown command: %s", args->argv[0]);
	return (CMD_RETURN_ERROR);
}
