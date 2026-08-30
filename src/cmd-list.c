/*
 * Copyright (c) 2009 Nicholas Marriott <nicm@users.sourceforge.net>
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

#include <stdlib.h>
#include <string.h>

#include "cow/cmd.h"
#include "cow/cmd_scan.h"
#include "cow/memory.h"
#include "cow/util.h"

struct cmd_list *
cmd_list_parse(int argc, char **argv, const char *file, u_int line,
    char **cause)
{
	struct cmd_list *cmdlist;
	struct cmd	*cmd;
	int		 i, lastsplit;
	size_t		 arglen, new_argc;
	char	       **copy_argv, **new_argv;

	copy_argv = cmd_copy_argv(argc, argv);

	cmdlist		    = xcalloc(1, sizeof *cmdlist);
	cmdlist->references = 1;
	TAILQ_INIT(&cmdlist->list);

	lastsplit = 0;
	for (i = 0; i < argc; i++) {
		arglen = strlen(copy_argv[i]);
		if (arglen == 0 || copy_argv[i][arglen - 1] != ';')
			continue;
		copy_argv[i][arglen - 1] = '\0';

		if (arglen > 1 && copy_argv[i][arglen - 2] == '\\') {
			copy_argv[i][arglen - 2] = ';';
			continue;
		}

		new_argc = i - lastsplit;
		new_argv = copy_argv + lastsplit;
		if (arglen != 1)
			new_argc++;

		cmd = cmd_parse(new_argc, new_argv, file, line, cause);
		if (cmd == NULL)
			goto bad;
		TAILQ_INSERT_TAIL(&cmdlist->list, cmd, qentry);

		lastsplit = i + 1;
	}

	if (lastsplit != argc) {
		cmd = cmd_parse(argc - lastsplit, copy_argv + lastsplit, file,
		    line, cause);
		if (cmd == NULL)
			goto bad;
		TAILQ_INSERT_TAIL(&cmdlist->list, cmd, qentry);
	}

	cmd_free_argv(argc, copy_argv);
	return (cmdlist);

bad:
	cmd_list_free(cmdlist);
	cmd_free_argv(argc, copy_argv);
	return (NULL);
}

void
cmd_list_free(struct cmd_list *cmdlist)
{
	struct cmd *cmd, *cmd1;

	if (--cmdlist->references != 0)
		return;

	TAILQ_FOREACH_SAFE (cmd, &cmdlist->list, qentry, cmd1) {
		TAILQ_REMOVE(&cmdlist->list, cmd, qentry);
		args_free(cmd->args);
		xfree(cmd->file);
		xfree(cmd);
	}

	xfree(cmdlist);
}

size_t
cmd_list_print(struct cmd_list *cmdlist, char *buf, size_t len)
{
	struct cmd *cmd;
	size_t	    off;

	off = 0;
	TAILQ_FOREACH (cmd, &cmdlist->list, qentry) {
		if (off >= len)
			break;
		off += cmd_print(cmd, buf + off, len - off);
		if (off >= len)
			break;
		if (TAILQ_NEXT(cmd, qentry) != NULL)
			off += snprintf(buf + off, len - off, " ; ");
	}
	return (off);
}

static void
config_append(char **buf, size_t *len, const char *text)
{
	size_t add = strlen(text);

	*buf = xrealloc(*buf, 1, *len + add + 1);
	memcpy(*buf + *len, text, add + 1);
	*len += add;
}

static void
config_append_arg(char **buf, size_t *len, const char *arg)
{
	size_t end;
	bool brace_group = arg[0] == '{' &&
	    cow_cmd_find_matching_brace(arg, strlen(arg), &end) ==
	    COW_CMD_SCAN_COMPLETE && arg[end + 1] == '\0';

	config_append(buf, len, " ");
	if (brace_group) {
		config_append(buf, len, arg);
		return;
	}

	const char *argv[] = { arg };
	char *quoted = cow_command_from_argv(1, argv);
	if (quoted != NULL) {
		config_append(buf, len, quoted);
		xfree(quoted);
	}
}

char *
cmd_to_config(struct cmd *cmd)
{
	char *buf = xstrdup("");
	size_t len = 0;

	config_append(&buf, &len, cmd->entry->name);

	struct args_entry *entry;
	RB_FOREACH (entry, args_tree, &cmd->args->tree) {
		if (entry->value != NULL)
			continue;
		char option[] = { '-', (char)entry->flag, '\0' };
		config_append_arg(&buf, &len, option);
	}
	RB_FOREACH (entry, args_tree, &cmd->args->tree) {
		if (entry->value == NULL)
			continue;
		char option[] = { '-', (char)entry->flag, '\0' };
		config_append_arg(&buf, &len, option);
		config_append_arg(&buf, &len, entry->value);
	}
	for (int i = 0; i < cmd->args->argc; i++)
		config_append_arg(&buf, &len, cmd->args->argv[i]);
	return (buf);
}

/* Format a command list for embedding in a script block. */
char *
cmd_list_to_config(struct cmd_list *cmdlist)
{
	struct cmd *cmd;
	char *buf = xstrdup("");
	size_t len = 0;

	TAILQ_FOREACH (cmd, &cmdlist->list, qentry) {
		char *command = cmd_to_config(cmd);
		if (len != 0)
			config_append(&buf, &len, "\n");
		config_append(&buf, &len, command);
		xfree(command);
	}
	return (buf);
}
