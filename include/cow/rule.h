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

/*
 * Lifecycle events that rules can attach to.  COW_RULE_NONE is a
 * sentinel; COW_RULE_TYPE_COUNT is the number of real values.
 */
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
	COW_RULE_ON_RESTART,
	COW_RULE_ON_TITLE_CHANGE,
	COW_RULE_ON_OUTPUTS_CHANGED,
	/*
	 * Declarative desk style.  Unlike the other ON_* events which
	 * fire once per lifecycle event, ON_DESK rules express a desk's
	 * persistent style and re-fire whenever the desk's effective
	 * configuration may need to be re-applied:
	 *   - a desk becomes the current desk on an output (on-new-desk)
	 *   - the page within a desk changes (on-new-page)
	 *   - a view maps onto a desk (on-map)
	 * Use with "-t :N" or ":N:" scope to target a specific desk.
	 */
	COW_RULE_ON_DESK,
	COW_RULE_TYPE_COUNT,
};

/*
 * A registered rule.  Each rule binds a parsed command list to one
 * lifecycle event.  Rules are scoped via either is_global=true (fires
 * for every event of this type) or by output_name / desk_nr (fires
 * only when the event's output / desk match).
 *
 * For view-bearing events (on-map, on-move, etc.) match_spec further
 * restricts firing to views matching the selector grammar (%app,
 * /title/, #id).
 */
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

/*
 * Return the file-scope list of registered rules so external callers
 * (e.g. `show rules`) can iterate them.  The list head is owned by
 * rule.c; callers must not modify it.  Returns NULL until
 * cow_rule_init() has been called.
 */
const struct wl_list *cow_rules_head(void);

/*
 * Take ownership of 'rule' (caller must have populated all fields,
 * including a parsed cmdlist).  Appends to the rules list.
 */
void cow_rule_add(struct cow_rule *);

/*
 * Remove and free the first rule with the given name.  Returns true
 * if a rule was removed.
 */
bool cow_rule_remove_by_name(const char *);

/*
 * Fire all rules of 'type' whose scope matches the supplied context.
 * 'view' / 'desk' / 'output' may be NULL when the event has no
 * associated view (on-restart) or no associated output (some
 * iconify paths).
 *
 * Most call sites use the cow_rule_fire() wrapper.  Events that
 * carry structured per-fire information (e.g. on-layer-change's
 * previous + current layer) call cow_rule_fire_json() instead and
 * pass a JSON object string; rule bodies see it via
 * cmd_q->event_json (cmd-exec exposes it as $COW_EVENT to the
 * spawned shell).  Ownership of `event_json` stays with the caller:
 * cow_rule_fire_json() copies it once per fire into each rule
 * body's cmd_q.
 */
void cow_rule_fire(enum cow_rule_type, struct cow_view *, struct cow_desk *,
    struct cow_output *);
void cow_rule_fire_json(enum cow_rule_type, struct cow_view *,
    struct cow_desk *, struct cow_output *, const char *event_json);

#endif
