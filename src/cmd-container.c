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
#include "cow/view.h"
#include "cow/wm.h"

static enum cmd_retval cmd_container_exec(struct cmd *, struct cmd_q *);

struct cmd_entry cmd_container_entry = {
	"window-container",
	"t:s:arnpNS",
	0,
	0,
	"[-t target] [-s source] [-arnpNS]",
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

	if (args_has(args, 'a')) {
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
		if (source->container_member != NULL &&
		    source->container_member->container == ctr) {
			cmdq_error(cmdq,
			    "container add: source is already in this container");
			return (CMD_RETURN_ERROR);
		}

		/* If source is in a different container, remove it first */
		if (source->container_member != NULL)
			cow_container_remove_view(source);

		if (ctr != NULL) {
			cow_container_add(ctr, source);
		} else {
			cow_container_create(target, source);
		}
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

	/*
	 * XXX: 'P' is not in cmd_container_entry.args_template ("t:s:arnpNS")
	 * so args_parse() currently rejects -P before exec runs, making this
	 * branch unreachable.  Either add 'P' to the template and document
	 * swap-prev in the usage string, or delete this block.
	 */
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
