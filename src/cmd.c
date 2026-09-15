/*
 * Copyright (c) 2013 Thomas Adam <thomas@xteddy.org>
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

/* Routines for generic command parsing. */

#include <stdio.h>
#include <string.h>
#include <ctype.h>

#include "cow/compat.h"
#include "cow/alias.h"
#include "cow/cmd.h"
#include "cow/cmd_scan.h"
#include "cow/util.h"
#include "cow/view.h"

struct causelist cfg_causes;

struct cmd_entry *cmd_table[] = {
	&cmd_alias_entry,
	&cmd_bind_entry,
	&cmd_bind_decoration_entry,
	&cmd_container_entry,
	&cmd_decor_entry,
	&cmd_desk_entry,
	&cmd_desk_name_entry,
	&cmd_draw_exec_entry,
	&cmd_exec_entry,
	&cmd_focus_entry,
	&cmd_icon_reset_entry,
	&cmd_icon_placement_entry,
	&cmd_include_entry,
	&cmd_keyboard_layout_entry,
	&cmd_menu_add_entry,
	&cmd_menu_clear_entry,
	&cmd_menu_destroy_entry,
	&cmd_menu_show_entry,
	&cmd_menu_style_entry,
	&cmd_mouse_entry,
	&cmd_nop_entry,
	&cmd_move_entry,
	&cmd_pick_window_entry,
	&cmd_quit_entry,
	&cmd_resize_entry,
	&cmd_rule_entry,
	&cmd_style_entry,
	&cmd_script_entry,
	&cmd_scroll_entry,
	&cmd_set_entry,
	&cmd_show_entry,
	&cmd_window_expand_entry,
	&cmd_window_list_entry,
	&cmd_window_select_entry,
	&cmd_window_maximize_entry,
	&cmd_window_tile_entry,
	&cmd_window_shade_entry,
	&cmd_winops_entry,
	NULL,
};

struct cmd_entry *
cmd_find_cmd(const char *cmd_name)
{
	struct cmd_entry **cmd_ent = NULL;
	struct cmd_entry  *cmd_p   = NULL;

	if (cmd_name == NULL)
		log_fatal("command name was NULL");

	for (cmd_ent = cmd_table; *cmd_ent != NULL; cmd_ent++) {
		if (strcmp((*cmd_ent)->name, cmd_name) == 0) {
			cmd_p = *cmd_ent;
			break;
		}
	}

	if (cmd_p == NULL && cow_alias_find(cmd_name) != NULL)
		cmd_p = &cmd_alias_invoke_entry;
	return (cmd_p);
}

char **
cmd_copy_argv(int argc, char *const *argv)
{
	char **new_argv;
	int    i;

	if (argc == 0)
		return (NULL);
	new_argv = xcalloc(argc, sizeof *new_argv);
	for (i = 0; i < argc; i++) {
		if (argv[i] != NULL)
			new_argv[i] = xstrdup(argv[i]);
	}
	return (new_argv);
}

/*
 * Join argv[start..argc-1] into a single space-separated string.
 * Returns a newly allocated string (caller frees with xfree).
 */
char *
cmd_argv_to_string(int argc, char **argv, int start)
{
	size_t len;
	int    i;
	char  *buf;

	len = 0;
	for (i = start; i < argc; i++)
		len += strlen(argv[i]) + 1; /* +1 for space or NUL */

	if (len == 0)
		return (xstrdup(""));

	buf = xmalloc(len);
	strlcpy(buf, argv[start], len);
	for (i = start + 1; i < argc; i++) {
		strlcat(buf, " ", len);
		strlcat(buf, argv[i], len);
	}

	return (buf);
}

/*
 * Join argv[start..argc-1] into a command string that cmd_string_parse()
 * can later parse back to the same argv.
 */
char *
cmd_argv_to_command_string(int argc, char **argv, int start)
{
	if (start >= argc)
		return (xstrdup(""));
	char *command = cow_command_from_argv((size_t)(argc - start),
	    (const char *const *)&argv[start]);

	if (command == NULL)
		return (xstrdup(""));
	return (command);
}

/*
 * Build a command string used as a deferred action.  A single argument may
 * itself contain the complete command (for example, "menu-show RootMenu"),
 * while separate arguments must be quoted so that reparsing preserves their
 * boundaries.
 */
char *
cmd_argv_to_action_string(int argc, char **argv, int start)
{
	if (argc - start == 1)
		return xstrdup(argv[start]);
	return cmd_argv_to_command_string(argc, argv, start);
}

char *
cmd_argv_to_command_string_or_body(int argc, char **argv, int start)
{
	char *cmd;
	size_t end;

	if (argc - start == 1 && argv[start][0] == '{')
		cmd = xstrdup(argv[start]);
	else
		cmd = cmd_argv_to_command_string(argc, argv, start);

	if (argc - start != 1 || cmd[0] != '{')
		return cmd;
	if (cow_cmd_find_matching_brace(cmd, strlen(cmd), &end) !=
	    COW_CMD_SCAN_COMPLETE)
		return cmd;
	for (size_t i = end + 1; cmd[i] != '\0'; i++) {
		if (!isspace((unsigned char)cmd[i]))
			return cmd;
	}

	char *body = xstrdup(cmd + 1);
	body[end - 1] = '\0';
	xfree(cmd);
	return body;
}

void
cmd_free_argv(int argc, char **argv)
{
	int i;

	if (argc == 0)
		return;
	for (i = 0; i < argc; i++)
		xfree(argv[i]);
	xfree(argv);
}

size_t
cmd_print(struct cmd *cmd, char *buf, size_t len)
{
	size_t off, used;

	const char *name = cmd->invoked_name != NULL ? cmd->invoked_name :
	    cmd->entry->name;
	off = snprintf(buf, len, "%s ", name);
	if (off + 1 < len) {
		used = args_print(cmd->args, buf + off, len - off - 1);
		if (used == 0)
			off--;
		else
			off += used;
		buf[off] = '\0';
	}
	return (off);
}

struct cmd *
cmd_parse(int argc, char **argv, const char *file, u_int line, char **cause)
{
	const struct cmd_entry *entry;
	struct cmd	       *cmd;
	struct args	       *args;

	*cause = NULL;
	if (argc == 0) {
		xasprintf(cause, "no command");
		return (NULL);
	}

	if ((entry = cmd_find_cmd(argv[0])) == NULL && file != NULL)
		entry = &cmd_alias_invoke_entry;
	if (entry == NULL) {
		xasprintf(cause, "unknown command: %s", argv[0]);
		return (NULL);
	}

	args = args_parse(entry->args_template, argc, argv);
	if (args == NULL)
		goto usage;
	if (entry->args_lower != -1 && args->argc < entry->args_lower)
		goto usage;
	if (entry->args_upper != -1 && args->argc > entry->args_upper)
		goto usage;

	cmd	   = xcalloc(1, sizeof *cmd);
	cmd->entry = entry;
	cmd->args  = args;
	if (entry == &cmd_alias_invoke_entry)
		cmd->invoked_name = xstrdup(argv[0]);

	if (file != NULL)
		cmd->file = xstrdup(file);
	cmd->line = line;

	return (cmd);

usage:
	if (args != NULL)
		args_free(args);
	xasprintf(cause, "usage: %s %s", entry->name, entry->usage);
	return (NULL);
}

enum cow_direction
cow_parse_direction(const char *s)
{
	if (s == NULL)
		return COW_DIR_NONE;
	if (strcmp(s, "up") == 0)
		return COW_DIR_UP;
	if (strcmp(s, "down") == 0)
		return COW_DIR_DOWN;
	if (strcmp(s, "left") == 0)
		return COW_DIR_LEFT;
	if (strcmp(s, "right") == 0)
		return COW_DIR_RIGHT;
	return COW_DIR_NONE;
}

struct cow_view *
cmd_require_target_window(struct cmd_q *cmdq, const char *cmd_name)
{
	struct cow_view *view = cmd_ctx_target_window(cmdq);

	if (view == NULL || view->closed) {
		cmdq_error(cmdq, "%s: no target window", cmd_name);
		return NULL;
	}
	return view;
}

bool
cmd_parse_bool(const char *s, bool *out)
{
	if (s == NULL)
		return false;
	if (strcmp(s, "true") == 0 || strcmp(s, "yes") == 0 ||
	    strcmp(s, "on") == 0 || strcmp(s, "1") == 0) {
		*out = true;
		return true;
	}
	if (strcmp(s, "false") == 0 || strcmp(s, "no") == 0 ||
	    strcmp(s, "off") == 0 || strcmp(s, "0") == 0) {
		*out = false;
		return true;
	}
	return false;
}
