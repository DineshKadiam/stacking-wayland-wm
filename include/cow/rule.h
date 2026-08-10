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
#if !defined(COW_RULE_H)
#define COW_RULE_H

#include <stdbool.h>

#include <wayland-client.h>

struct cmd_list;
struct cow_view;
struct cow_desk;
struct cow_output;

/* Lifecycle events for rules. */
enum cow_rule_type {
	COW_RULE_NONE = 0,
	COW_RULE_ON_MAP,
	COW_RULE_ON_MOVE,
	COW_RULE_ON_RESIZE,
	COW_RULE_ON_SHADE,
	COW_RULE_ON_UNSHADE,
	COW_RULE_ON_ICONIFY,
	COW_RULE_ON_DEICONIFY,
	COW_RULE_ON_LAYER_CHANGE,
	COW_RULE_ON_NEW_DESK,
	COW_RULE_ON_NEW_PAGE,
	COW_RULE_ON_START,
	COW_RULE_ON_RESTART,
	COW_RULE_ON_TITLE_CHANGE,
	COW_RULE_ON_OUTPUTS_CHANGED,
	COW_RULE_ON_DESKS_CHANGED,
	/* Declarative desk style; re-applied as desk/page/view state changes. */
	COW_RULE_ON_DESK,
	COW_RULE_TYPE_COUNT,
};

/* A registered rule and its scope. */
struct cow_rule {
	char		  *name;
	enum cow_rule_type type;

	bool  is_global;
	char *output_name; /* NULL = any */
	int   desk_nr;	   /* -1 = any */
	char *desk_name;   /* NULL = use desk_nr or any */
	char *match_spec;  /* NULL = any view */

	struct cmd_list *cmdlist;
	struct wl_list	 link; /* file-scope list in src/rule.c */
};

void cow_rule_init(void);
void cow_rule_fini(void);

bool	    cow_rule_type_from_string(const char *, enum cow_rule_type *);
const char *cow_rule_type_name(enum cow_rule_type);

struct cow_rule *cow_rule_find_by_name(const char *);

/* Borrow the registered-rule list. */
const struct wl_list *cow_rules_head(void);

/* Take ownership of 'rule'. */
void cow_rule_add(struct cow_rule *);

/* Remove and free the first rule with this name. */
bool cow_rule_remove_by_name(const char *);

/*
 * Fire all rules of 'type' whose scope matches the supplied context.
 * Context pointers may be NULL.  event_json is copied into each
 * matching rule queue and exposed to exec as $COW_EVENT.
 */
void cow_rule_fire(enum cow_rule_type, struct cow_view *, struct cow_desk *,
    struct cow_output *);
void cow_rule_fire_json(enum cow_rule_type, struct cow_view *,
    struct cow_desk *, struct cow_output *, const char *event_json);

#endif
