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
#if !defined(COW_STYLE_H)
#define COW_STYLE_H

#include <stdbool.h>
#include <stdint.h>

#include <wayland-client.h>
#include <cJSON.h>

#include "cow/policy.h"

struct cmd_q;
struct cow_output;
struct cow_view;

enum cow_style_key {
	COW_STYLE_DECOR_PROFILE,
	COW_STYLE_DECOR_ENTRY,
	COW_STYLE_LAYER,
	COW_STYLE_CIRCULATE_SKIP,
	COW_STYLE_SNAP_TARGET,
	COW_STYLE_STICKY_DESK,
	COW_STYLE_STICKY_PAGE,
	COW_STYLE_PLACEMENT_OUTPUT,
	COW_STYLE_PLACEMENT_DESK,
	COW_STYLE_PLACEMENT_PAGE,
	COW_STYLE_PLACEMENT_X,
	COW_STYLE_PLACEMENT_Y,
	COW_STYLE_PLACEMENT_WIDTH,
	COW_STYLE_PLACEMENT_HEIGHT,
	COW_STYLE_PLACEMENT_ANCHOR,
	COW_STYLE_STATE_MAXIMIZED,
	COW_STYLE_STATE_MAXIMIZED_IGNORE_RESERVED,
	COW_STYLE_STATE_FULLSCREEN,
	COW_STYLE_STATE_SHADED,
	COW_STYLE_STATE_ICONIFIED,
};

struct cow_style_entry {
	enum cow_style_key key;
	char              *name;
	char              *value;
	struct wl_list      link;
};

struct cow_style {
	struct cow_policy policy;
	char             *body;
	char		  *file;
	bool		   runtime;
	struct wl_list     entries;
	struct wl_list     link;
};

void cow_style_init(void);
void cow_style_fini(void);
struct cow_style *cow_style_find_by_name(const char *);
const struct wl_list *cow_styles_head(void);
void cow_style_add(struct cow_style *);
void cow_style_destroy(struct cow_style *);
bool cow_style_remove_by_name(const char *);
bool cow_style_parse_body(struct cow_style *, const char *, struct cmd_q *);
void cow_style_apply_for_event(struct cow_view *, bool);
void cow_style_apply_pending_initial(struct cow_view *);
void cow_style_cancel_pending_initial(struct cow_view *);
void cow_style_reflow_anchors(struct cow_output *);
void cow_style_apply_all_live(void);
bool cow_style_is_applying(void);
cJSON *cow_style_save_runtime(void);
void   cow_style_restore_runtime(const cJSON *);

#endif /* COW_STYLE_H */
