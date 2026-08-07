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
struct cJSON;

/* Shared direction token. */
enum cow_direction {
	COW_DIR_NONE = -1,
	COW_DIR_UP,
	COW_DIR_DOWN,
	COW_DIR_LEFT,
	COW_DIR_RIGHT,
};

enum cow_direction cow_parse_direction(const char *);

/* Parse true/false, yes/no, on/off, or 1/0. */
bool cmd_parse_bool(const char *, bool *);

/* Resolve the target window or emit a canonical error. */
struct cow_view *cmd_require_target_window(struct cmd_q *, const char *);

/* Resolved command context; NULL fields mean current/default. */
struct cmd_ctx {
	struct cow_output *output;
	struct cow_desk	  *desk;
	struct cow_view	  *window;
};

/* Parse [output]:[desk].[window] into a cmd_ctx. */
bool cmd_ctx_parse(const char *, struct cmd_ctx *, char **);

/* Forward declarations. */
struct cmd;
struct cmd_q;

/* Command return values. */
enum cmd_retval {
	CMD_RETURN_ERROR  = -1,
	CMD_RETURN_NORMAL = 0,
};

/* Command parses -t/-s itself. */
#define CMD_OWN_TS (1u << 0)

/* Command definition. */
struct cmd_entry {
	const char *name;

	const char *args_template;
	int	    args_lower;
	int	    args_upper;

	const char *usage;

	enum cmd_retval (*exec)(struct cmd *, struct cmd_q *);

	unsigned int flags;
};

/* A parsed command instance. */
struct cmd {
	const struct cmd_entry *entry;
	struct args	       *args;

	char *file;
	u_int line;

	/* Per-command default target. */
	struct cmd_ctx default_target;

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

	/* Resolved -t/-s contexts. */
	struct cmd_ctx target;
	struct cmd_ctx source;

	/* Queue-level default target. */
	struct cmd_ctx default_target;

	/* Per-event JSON payload, exposed to exec as COW_EVENT. */
	char *event_json;

	int in_rule;
};

/* Error cause list. */
ARRAY_DECL(causelist, char *);
extern struct causelist cfg_causes;

struct cow_view *cmd_ctx_target_window(struct cmd_q *);
struct cow_output *cmd_ctx_target_output(struct cmd_q *);
struct cow_desk *cmd_ctx_target_desk(struct cmd_q *);

/* Resolve from -s instead of -t. */
struct cow_view *cmd_require_source_window(struct cmd_q *, const char *);

struct cow_output *cmd_require_target_output(struct cmd_q *, const char *);
struct cow_desk *cmd_require_target_desk(struct cmd_q *, const char *);

/* Parse an integer and emit the standard command error on failure. */
bool cmd_parse_int(struct cmd_q *, const char *, const char *, const char *,
    long long, long long, long long *);

enum cmd_length_unit {
	CMD_LENGTH_PIXELS,
	CMD_LENGTH_VW,
	CMD_LENGTH_VH,
};

struct cmd_length {
	double		     value;
	enum cmd_length_unit unit;
};

/* Parse an integer pixel length or a decimal value suffixed vw/vh. */
bool cmd_parse_length(struct cmd_q *, const char *, const char *, const char *,
    long long, long long, struct cmd_length *);

/* Resolve a parsed length against an output's usable area. */
bool cmd_length_resolve(struct cmd_q *, const char *, const char *,
    const struct cmd_length *, const struct cow_output *, int *);

/* Lookup a flag and parse its integer value if present. */
bool cmd_args_int(struct cmd_q *, const char *, struct args *, u_char,
    long long, long long, long long *);

/* Resolve a direction flag. */
enum cow_direction cmd_args_direction(struct cmd_q *, const char *,
    struct args *, u_char, bool);

/* Mark a view as needing visual and manage refresh. */
void cmd_view_dirty(struct cow_view *);

/* Send an IPC JSON response; takes ownership of the object. */
void cmdq_respond_data(struct cmd_q *, struct cJSON *);

/* Send an IPC error response. */
void cmdq_respond_error(struct cmd_q *, const char *);

extern struct cmd_entry *cmd_table[];

/* Run exec commands deferred during config loading. */
void cow_exec_run_deferred(void);

/* Built-in commands. */
extern struct cmd_entry cmd_menu_add_entry;
extern struct cmd_entry cmd_menu_clear_entry;
extern struct cmd_entry cmd_menu_destroy_entry;
extern struct cmd_entry cmd_menu_show_entry;
extern struct cmd_entry cmd_menu_style_entry;
extern struct cmd_entry cmd_set_entry;
extern struct cmd_entry cmd_decor_entry;
extern struct cmd_entry cmd_bind_entry;
extern struct cmd_entry cmd_bind_decoration_entry;
extern struct cmd_entry cmd_mouse_entry;
extern struct cmd_entry cmd_nop_entry;
extern struct cmd_entry cmd_rule_entry;
extern struct cmd_entry cmd_draw_exec_entry;
extern struct cmd_entry cmd_exec_entry;
extern struct cmd_entry cmd_show_entry;
extern struct cmd_entry cmd_quit_entry;
extern struct cmd_entry cmd_list_commands_entry;
extern struct cmd_entry cmd_winops_entry;
extern struct cmd_entry cmd_window_expand_entry;
extern struct cmd_entry cmd_window_maximize_entry;
extern struct cmd_entry cmd_window_tile_entry;
extern struct cmd_entry cmd_window_list_entry;
extern struct cmd_entry cmd_move_entry;
extern struct cmd_entry cmd_resize_entry;
extern struct cmd_entry cmd_focus_entry;
extern struct cmd_entry cmd_icon_reset_entry;
extern struct cmd_entry cmd_include_entry;
extern struct cmd_entry cmd_desk_entry;
extern struct cmd_entry cmd_desk_name_entry;
extern struct cmd_entry cmd_scroll_entry;
extern struct cmd_entry cmd_script_entry;
extern struct cmd_entry cmd_window_shade_entry;
extern struct cmd_entry cmd_container_entry;
extern struct cmd_entry cmd_pick_window_entry;

bool cmd_set_is_decor_key(const char *);
char *cmd_set_normalize_decor_value(const char *, const char *,
    struct cmd_q *);
void cow_decor_reset_profiles(void);
void cow_decor_apply_runtime_active(struct cow_view *);
void cmd_set_reset_decor_target(struct cmd_q *);
enum cmd_retval cmd_set_apply_decor(const char *, const char *,
    struct cmd_q *);

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
char		 *cmd_argv_to_command_string(int, char **, int);
char		 *cmd_argv_to_action_string(int, char **, int);
char		 *cmd_argv_to_command_string_or_body(int, char **, int);
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
void		 cmdq_set_default_target(struct cmd_q *, struct cow_view *,
		struct cow_desk *, struct cow_output *);
/* Attach/replace the queue event JSON; takes ownership. */
void cmdq_set_event_json(struct cmd_q *, char *json);

/* Memory wrappers -- declared in cow/memory.h */
#include "cow/memory.h"

#endif /* COW_CMD_H */
