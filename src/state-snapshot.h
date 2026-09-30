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

#if !defined(COW_STATE_SNAPSHOT_H)
#define COW_STATE_SNAPSHOT_H

#include "state-value.h"

struct cow_view;
struct cow_output;
struct cow_desk;
struct cow_rule;
struct cow_style;
struct cow_menu;
struct cow_alias;
struct cow_container;
struct cow_container_pane;

enum state_source {
	STATE_SRC_WINDOWS,
	STATE_SRC_CONTAINERS,
	STATE_SRC_PANES,
	STATE_SRC_DESKS,
	STATE_SRC_OUTPUTS,
	STATE_SRC_RULES,
	STATE_SRC_STYLES,
	STATE_SRC_DECOR,
	STATE_SRC_MENUS,
	STATE_SRC_BINDINGS,
	STATE_SRC_COMMANDS,
	STATE_SRC_SELECTIONS,
	STATE_SRC_ALIASES,
};

/*
 * Snapshot the named collection from live wm state.  Returned value
 * is a freshly-owned STATE_V_LIST of records; caller state_value_unref's
 * when done.
 */
struct state_value *state_snapshot_collection(enum state_source);
struct state_value *state_snapshot_view(const struct cow_view *);
struct state_value *state_snapshot_container(const struct cow_container *);
struct state_value *state_snapshot_pane(const struct cow_container *,
    const struct cow_container_pane *);
struct state_value *state_snapshot_output(const struct cow_output *);
struct state_value *state_snapshot_desk(const struct cow_desk *);
struct state_value *state_snapshot_rule(const struct cow_rule *);
struct state_value *state_snapshot_style(const struct cow_style *);
struct state_value *state_snapshot_menu(const struct cow_menu *);
struct state_value *state_snapshot_alias(const struct cow_alias *);
struct state_value *state_snapshot_menu_style(void);

#endif /* COW_STATE_SNAPSHOT_H */
