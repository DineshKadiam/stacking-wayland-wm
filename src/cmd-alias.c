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

#include <ctype.h>
#include <string.h>

#include "cow/alias.h"
#include "cow/cmd.h"
#include "cow/config.h"
#include "cow/memory.h"

#define COW_ALIAS_MAX_DEPTH 32

static enum cmd_retval cmd_alias_exec(struct cmd *, struct cmd_q *);
static enum cmd_retval cmd_alias_invoke(struct cmd *, struct cmd_q *);

struct cmd_entry cmd_alias_entry = {
	"alias",
	"+d",
	1,
	-1,
	"alias NAME command ... | alias -d NAME",
	cmd_alias_exec,
	CMD_CONFIG,
};

/* Used internally for commands which are not built-ins. */
struct cmd_entry cmd_alias_invoke_entry = {
	"<alias>",
	"",
	0,
	-1,
	"",
	cmd_alias_invoke,
	0,
};

static bool
alias_name_valid(const char *name)
{
	if (name == NULL || !(isalpha((unsigned char)name[0]) ||
		name[0] == '_'))
		return (false);
	for (const unsigned char *p = (const unsigned char *)name + 1;
	    *p != '\0'; p++) {
		if (!isalnum(*p) && *p != '_' && *p != '-')
			return (false);
	}
	return (true);
}

static bool
alias_name_reserved(const char *name)
{
	static const char *reserved[] = {
		"let", "for", "in", "if", "else", "break", "continue",
		"where", "and", "or", "not", "matches", "true", "false",
		"nil", "self", "windows", "desks", "outputs", "screens",
		"rules", "selections", NULL,
	};
	for (const char **word = reserved; *word != NULL; word++) {
		if (strcmp(name, *word) == 0)
			return (true);
	}
	return (false);
}

static enum cmd_retval
cmd_alias_exec(struct cmd *self, struct cmd_q *cmdq)
{
	struct args *args = self->args;
	const char *name = args->argv[0];

	if (!alias_name_valid(name) || alias_name_reserved(name)) {
		cmdq_error(cmdq, "alias: invalid name: %s", name);
		return (CMD_RETURN_ERROR);
	}
	for (struct cmd_entry **entry = cmd_table; *entry != NULL; entry++) {
		if (*entry != &cmd_alias_invoke_entry &&
		    strcmp((*entry)->name, name) == 0) {
			cmdq_error(cmdq,
			    "alias: cannot replace built-in command: %s", name);
			return (CMD_RETURN_ERROR);
		}
	}

	if (args_has(args, 'd')) {
		if (args->argc != 1) {
			cmdq_error(cmdq, "usage: alias -d NAME");
			return (CMD_RETURN_ERROR);
		}
		if (!cow_alias_remove(name)) {
			cmdq_error(cmdq, "alias: no alias named '%s'", name);
			return (CMD_RETURN_ERROR);
		}
		return (CMD_RETURN_NORMAL);
	}

	if (args->argc < 2) {
		cmdq_error(cmdq, "usage: alias NAME command ...");
		return (CMD_RETURN_ERROR);
	}

	char *command = cmd_argv_to_action_string(args->argc, args->argv, 1);
	struct cmd_list *cmdlist = NULL;
	char *cause = NULL;
	if (cmd_string_parse(command, &cmdlist, NULL, 0, &cause) != 0 ||
	    cmdlist == NULL) {
		cmdq_error(cmdq, "alias: %s",
		    cause != NULL ? cause : "invalid command");
		xfree(cause);
		xfree(command);
		return (CMD_RETURN_ERROR);
	}
	cmd_list_free(cmdlist);
	xfree(cause);

	bool ok = cow_alias_set(name, command);
	xfree(command);
	if (!ok) {
		cmdq_error(cmdq, "alias: cannot define '%s'", name);
		return (CMD_RETURN_ERROR);
	}
	return (CMD_RETURN_NORMAL);
}

static enum cmd_retval
cmd_alias_invoke(struct cmd *self, struct cmd_q *cmdq)
{
	const char *name = self->invoked_name;
	struct cow_alias *alias = cow_alias_find(name);
	if (alias == NULL) {
		cmdq_error(cmdq, "unknown command: %s", name);
		return (CMD_RETURN_ERROR);
	}
	if (self->args->argc != 0) {
		cmdq_error(cmdq, "alias '%s' does not accept arguments", name);
		return (CMD_RETURN_ERROR);
	}
	if (cmdq->alias_depth >= COW_ALIAS_MAX_DEPTH) {
		cmdq_error(cmdq, "alias: recursion limit reached at '%s'", name);
		return (CMD_RETURN_ERROR);
	}

	struct cmd_list *cmdlist = NULL;
	char *cause = NULL;
	if (cmd_string_parse(alias->command, &cmdlist, NULL, 0, &cause) != 0 ||
	    cmdlist == NULL) {
		cmdq_error(cmdq, "alias '%s': %s", name,
		    cause != NULL ? cause : "invalid command");
		xfree(cause);
		return (CMD_RETURN_ERROR);
	}
	xfree(cause);

	struct cmd_q *nested = cmdq_new();
	nested->client_fd = cmdq->client_fd;
	nested->default_target = cmdq->target;
	nested->in_rule = cmdq->in_rule;
	nested->in_style = cmdq->in_style;
	nested->alias_depth = cmdq->alias_depth + 1;
	if (cmdq->event_json != NULL)
		cmdq_set_event_json(nested, xstrdup(cmdq->event_json));
	cmdq_run(nested, cmdlist);
	bool ok = !nested->failed;
	cmdq_free(nested);
	cmd_list_free(cmdlist);
	return (ok ? CMD_RETURN_NORMAL : CMD_RETURN_ERROR);
}
