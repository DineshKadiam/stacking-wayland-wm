/*
 * Command infrastructure, derived from tmux.
 *
 * Copyright (c) 2008 Nicholas Marriott <nicm@users.sourceforge.net>
 * Copyright (c) 2026 Thomas Adam <thomas@xteddy.org>
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

#if !defined(COW_CMD_H)
#define COW_CMD_H

#include <sys/types.h>

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "cow/compat.h"
#include "cow/array.h"

struct args_entry {
	u_char		     flag;
	char		    *value;
	RB_ENTRY(args_entry) entry;
};
RB_HEAD(args_tree, args_entry);

struct args {
	struct args_tree tree;
	int		 argc;
	char	       **argv;
};

struct cow_output;
struct cow_desk;
struct cow_view;
struct cmd_q;

/*
 * Cardinal direction used by focus, move, resize, page, etc.
 * COW_DIR_NONE signals an unrecognised input.
 */
enum cow_direction {
	COW_DIR_NONE = -1,
	COW_DIR_UP,
	COW_DIR_DOWN,
	COW_DIR_LEFT,
	COW_DIR_RIGHT,
};

/*
 * Parse a direction token ("up"/"down"/"left"/"right").
 * Returns COW_DIR_NONE on any other input (including NULL).
 */
enum cow_direction cow_parse_direction(const char *s);

/*
 * Parse a boolean from a config/command value.
 *   true/yes/on/1  -> true
 *   false/no/off/0 -> false
 * Returns true on success; on any other input *out is untouched
 * and the return is false.  NULL input also returns false.
 */
bool cmd_parse_bool(const char *s, bool *out);

/*
 * Resolve the target window from cmdq or emit a canonical error.
 * cmd_name is embedded in the error message.  Returns NULL (and
 * has already reported via cmdq_error) if there is no usable
 * target, which the caller should translate to CMD_RETURN_ERROR.
 */
struct cow_view *cmd_require_target_window(struct cmd_q *cmdq,
    const char						*cmd_name);

/*
 * Resolved command context.  Produced by parsing a specifier string
 * of the form "[output]:[desk].[window]".
 *
 * NULL fields mean "use current/default".
 */
struct cmd_ctx {
	struct cow_output *output;
	struct cow_desk	  *desk;
	struct cow_view	  *window;
};

/*
 * Parse a target/source specifier string into a cmd_ctx.
 *
 * Specifier format:  [output]:[desk].[window]
 *
 * Output selectors:
 *   <name>        by output name (e.g. "DP-1")
 *   @next/@prev   relative to current output
 *
 * Desk selectors:
 *   <number>      by desk number (0-9)
 *   @next/@prev   relative to current desk

 *
 * Window selectors:
 *   %<app_id>     by app_id (e.g. "%XTerm")
 *   /<substring>/ by title substring
 *   #<id>         by river window identifier
 *   @focused      the currently focused window
 *   @next/@prev   cycle relative to focused
 *
 * Returns true on success.  On failure, sets *cause and returns false.
 */
bool cmd_ctx_parse(const char *spec, struct cmd_ctx *ctx, char **cause);

/* Forward declarations. */
struct cmd;
struct cmd_q;

/* Command return values. */
enum cmd_retval {
	CMD_RETURN_ERROR  = -1,
	CMD_RETURN_NORMAL = 0,
};

/* Command definition. */
struct cmd_entry {
	const char *name;

	const char *args_template;
	int	    args_lower;
	int	    args_upper;

	const char *usage;

	enum cmd_retval (*exec)(struct cmd *, struct cmd_q *);
};

/* A parsed command instance. */
struct cmd {
	const struct cmd_entry *entry;
	struct args	       *args;

	char *file;
	u_int line;

	TAILQ_ENTRY(cmd) qentry;
};

/* A list of commands (semicolon-separated chain). */
struct cmd_list {
	int references;
	TAILQ_HEAD(, cmd) list;
};

/* Command queue entry. */
struct cmd_q_item {
	struct cmd_list	       *cmdlist;
	TAILQ_ENTRY(cmd_q_item) qentry;
};
TAILQ_HEAD(cmd_q_items, cmd_q_item);

/* Command queue. */
struct cmd_q {
	int references;
	int dead;

	struct cmd_q_items queue;
	struct cmd_q_item *item;
	struct cmd	  *cmd;

	time_t time;
	u_int  number;

	void (*emptyfn)(struct cmd_q *);
	void *data;

	/* IPC client fd for response routing (-1 = config file mode). */
	int client_fd;

	/* Resolved target/source context from -t/-s flags.
	 * Populated by cmdq_continue before calling cmd->exec. */
	struct cmd_ctx target;
	struct cmd_ctx source;
};

/* Error cause list. */
ARRAY_DECL(causelist, char *);
extern struct causelist cfg_causes;

/*
 * Convenience: get the target window from a cmd_q, falling back
 * to the focused view if no -t was specified.
 */
struct cow_view *cmd_ctx_target_window(struct cmd_q *cmdq);

/*
 * Convenience: get the target output, falling back to current.
 */
struct cow_output *cmd_ctx_target_output(struct cmd_q *cmdq);

/*
 * Convenience: get the target desk, falling back to current.
 */
struct cow_desk *cmd_ctx_target_desk(struct cmd_q *cmdq);

extern struct cmd_entry *cmd_table[];

/*
 * Run any exec commands that were deferred during config loading.
 * Called from main() after the WM is connected.
 */
void cow_exec_run_deferred(void);

/*
 * Run any rule commands that were deferred during config loading.
 * Called after WM is connected and outputs are available.
 */
void cow_rule_run_deferred(void);

/* Built-in commands. */
extern struct cmd_entry cmd_set_entry;
extern struct cmd_entry cmd_bind_entry;
extern struct cmd_entry cmd_mouse_entry;
extern struct cmd_entry cmd_rule_entry;
extern struct cmd_entry cmd_exec_entry;
extern struct cmd_entry cmd_show_entry;
extern struct cmd_entry cmd_quit_entry;
extern struct cmd_entry cmd_list_commands_entry;
extern struct cmd_entry cmd_winops_entry;
extern struct cmd_entry cmd_window_expand_entry;
extern struct cmd_entry cmd_window_maximize_entry;
extern struct cmd_entry cmd_move_entry;
extern struct cmd_entry cmd_resize_entry;
extern struct cmd_entry cmd_focus_entry;
extern struct cmd_entry cmd_desk_entry;
extern struct cmd_entry cmd_page_entry;
extern struct cmd_entry cmd_window_shade_entry;
extern struct cmd_entry cmd_container_entry;

int args_cmp(struct args_entry *, struct args_entry *);
RB_PROTOTYPE(args_tree, args_entry, entry, args_cmp);
struct args *args_create(int, ...);
struct args *args_parse(const char *, int, char **);
void	     args_free(struct args *);
size_t	     args_print(struct args *, char *, size_t);
int	     args_has(struct args *, u_char);
void	     args_set(struct args *, u_char, const char *);
char	    *args_get(struct args *, u_char);
long long args_strtonum(struct args *, u_char, long long, long long, char **);

struct cmd_entry *cmd_find_cmd(const char *);
char		**cmd_copy_argv(int, char *const *);
void		  cmd_free_argv(int, char **);
char		 *cmd_argv_to_string(int, char **, int);
size_t		  cmd_print(struct cmd *, char *, size_t);
struct cmd	 *cmd_parse(int, char **, const char *, u_int, char **);

struct cmd_list *cmd_list_parse(int, char **, const char *, u_int, char **);
void		 cmd_list_free(struct cmd_list *);
size_t		 cmd_list_print(struct cmd_list *, char *, size_t);

int cmd_string_parse(const char *, struct cmd_list **, const char *, u_int,
    char **);

struct cmd_q	*cmdq_new(void);
int		 cmdq_free(struct cmd_q *);
void printflike2 cmdq_error(struct cmd_q *, const char *, ...);
void		 cmdq_run(struct cmd_q *, struct cmd_list *);
void		 cmdq_append(struct cmd_q *, struct cmd_list *);
int		 cmdq_continue(struct cmd_q *);
void		 cmdq_flush(struct cmd_q *);

/* Memory wrappers -- declared in cow/memory.h */
#include "cow/memory.h"

#endif /* COW_CMD_H */
