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

#include <bsd/string.h>
#include <stdio.h>
#include <string.h>

#include "cow/cmd.h"

struct causelist cfg_causes;

struct cmd_entry *cmd_table[] = {&cmd_action_entry,
                                 &cmd_bind_entry,
                                 &cmd_container_entry,
                                 &cmd_desk_entry,
                                 &cmd_exec_entry,
                                 &cmd_focus_entry,
                                 &cmd_list_commands_entry,
                                 &cmd_mouse_entry,
                                 &cmd_move_entry,
                                 &cmd_quit_entry,
                                 &cmd_resize_entry,
                                 &cmd_rule_entry,
                                 &cmd_set_entry,
                                 &cmd_show_entry,
                                 &cmd_window_expand_entry,
                                 &cmd_window_maximize_entry,
                                 &cmd_window_shade_entry,
                                 &cmd_winops_entry,
                                 NULL};

struct cmd_entry *cmd_find_cmd(const char *cmd_name) {
  struct cmd_entry **cmd_ent = NULL;
  struct cmd_entry *cmd_p = NULL;

  if (cmd_name == NULL)
    log_fatal("command name was NULL");

  for (cmd_ent = cmd_table; *cmd_ent != NULL; cmd_ent++) {
    if (strcmp((*cmd_ent)->name, cmd_name) == 0) {
      cmd_p = *cmd_ent;
      break;
    }
  }

  return (cmd_p);
}

char **cmd_copy_argv(int argc, char *const *argv) {
  char **new_argv;
  int i;

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
char *cmd_argv_to_string(int argc, char **argv, int start) {
  size_t len;
  int i;
  char *buf;

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

void cmd_free_argv(int argc, char **argv) {
  int i;

  if (argc == 0)
    return;
  for (i = 0; i < argc; i++)
    xfree(argv[i]);
  xfree(argv);
}

size_t cmd_print(struct cmd *cmd, char *buf, size_t len) {
  size_t off, used;

  off = snprintf(buf, len, "%s ", cmd->entry->name);
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

struct cmd *cmd_parse(int argc, char **argv, const char *file, u_int line,
                      char **cause) {
  const struct cmd_entry *entry;
  struct cmd *cmd;
  struct args *args;

  *cause = NULL;
  if (argc == 0) {
    xasprintf(cause, "no command");
    return (NULL);
  }

  if ((entry = cmd_find_cmd(argv[0])) == NULL) {
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

  cmd = xcalloc(1, sizeof *cmd);
  cmd->entry = entry;
  cmd->args = args;

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
