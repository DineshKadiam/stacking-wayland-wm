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
#if !defined(COW_KEYBOARD_LAYOUT_H)
#define COW_KEYBOARD_LAYOUT_H

#include <stdbool.h>
#include <stdint.h>
#include <wayland-util.h>

struct cJSON;
struct cow_view;
struct wl_registry;
struct river_xkb_keyboard_v1;

struct cow_keyboard_layout {
	struct wl_list		 link;
	struct river_xkb_keyboard_v1 *river_keyboard;
	uint32_t		 index;
	char			*name;
	bool			 has_layout;
	bool			 caps_lock;
	bool			 has_caps_lock;
	bool			 num_lock;
	bool			 has_num_lock;
	bool			 scroll_lock;
	bool			 has_scroll_lock;
};

void cow_keyboard_layout_init(void);
void cow_keyboard_layout_bind(struct wl_registry *, uint32_t, uint32_t);
void cow_keyboard_layout_fini(void);
void cow_keyboard_layout_add_status(struct cJSON *);
struct cJSON *cow_keyboard_layout_active_json(void);
bool cow_keyboard_layout_set_name(const char *);
void cow_keyboard_layout_reconcile_focus(void);
void cow_keyboard_layout_forget_view(struct cow_view *);

#endif
