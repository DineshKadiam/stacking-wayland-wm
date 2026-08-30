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

#include "cow/cmd.h"
#include "cow/memory.h"
#include "cow/policy.h"
#include "cow/style.h"

static enum cmd_retval cmd_style_exec(struct cmd *, struct cmd_q *);

struct cmd_entry cmd_style_entry = {
	"style",
	"+dgn:t:s:",
	0,
	1,
	"style -g | -t output[:desk] -n name [-s selector] { ... }\n"
	"style -d -n name",
	cmd_style_exec,
	CMD_OWN_TS | CMD_CONFIG,
};

static enum cmd_retval
cmd_style_exec(struct cmd *self, struct cmd_q *cmdq)
{
	struct args *args = self->args;
	const char *name = args_get(args, 'n');
	if (name == NULL) {
		cmdq_error(cmdq, "style: missing -n name");
		return (CMD_RETURN_ERROR);
	}
	if (args_has(args, 'd')) {
		if (args->argc != 0 || args_has(args, 'g') ||
		    args_get(args, 't') != NULL || args_get(args, 's') != NULL) {
			cmdq_error(cmdq, "usage: style -d -n name");
			return (CMD_RETURN_ERROR);
		}
		if (!cow_style_remove_by_name(name)) {
			cmdq_error(cmdq, "style: no style named '%s'", name);
			return (CMD_RETURN_ERROR);
		}
		return (CMD_RETURN_NORMAL);
	}

	bool global = args_has(args, 'g');
	const char *target = args_get(args, 't');
	if (global == (target != NULL)) {
		cmdq_error(cmdq, "style: one of -g or -t is required");
		return (CMD_RETURN_ERROR);
	}
	if (args->argc != 1) {
		cmdq_error(cmdq, "style: exactly one brace-delimited body is required");
		return (CMD_RETURN_ERROR);
	}
	if (cow_style_find_by_name(name) != NULL) {
		cmdq_error(cmdq, "style: a style named '%s' already exists", name);
		return (CMD_RETURN_ERROR);
	}

	struct cow_style *style = xcalloc(1, sizeof(*style));
	cow_policy_init(&style->policy);
	style->policy.name = xstrdup(name);
	style->policy.is_global = global;
	const char *selector = args_get(args, 's');
	if (selector != NULL)
		style->policy.match_spec = xstrdup(selector);
	wl_list_init(&style->entries);

	if (target != NULL) {
		char *cause = NULL;
		if (!cow_policy_parse_scope(target, &style->policy, &cause)) {
			cmdq_error(cmdq, "style: %s",
			    cause != NULL ? cause : "bad -t spec");
			xfree(cause);
			cow_policy_fini(&style->policy);
			xfree(style);
			return (CMD_RETURN_ERROR);
		}
	}
	if (!cow_style_parse_body(style, args->argv[0], cmdq)) {
		cow_style_destroy(style);
		return (CMD_RETURN_ERROR);
	}
	cow_style_add(style);
	return (CMD_RETURN_NORMAL);
}
