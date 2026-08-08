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
#if !defined(COW_ICON_PLACEMENT_H)
#define COW_ICON_PLACEMENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <wayland-client.h>

#include "cow/cow_box.h"

struct cow_config;
struct cow_output;

enum cow_icon_direction {
	COW_ICON_DIR_RIGHT,
	COW_ICON_DIR_LEFT,
	COW_ICON_DIR_DOWN,
	COW_ICON_DIR_UP,
};

enum cow_icon_box_type {
	COW_ICON_BOX_DEFAULT,
	COW_ICON_BOX_FULL,
	COW_ICON_BOX_GEOMETRY,
};

struct cow_icon_box_config {
	enum cow_icon_box_type type;
	int width, height;
	int x, y;
	bool from_right, from_bottom;
};

struct cow_icon_placement {
	struct cow_icon_box_config box;
	enum cow_icon_direction primary, secondary;
	int grid_x, grid_y;
	bool compact;
};

struct cow_icon_rect {
	int x, y, width, height;
};

enum cow_icon_placement_field {
	COW_ICON_PLACEMENT_BOX  = 1u << 0,
	COW_ICON_PLACEMENT_FILL = 1u << 1,
	COW_ICON_PLACEMENT_GRID = 1u << 2,
	COW_ICON_PLACEMENT_COMPACT = 1u << 3,
};

struct cow_icon_placement_override {
	struct wl_list link;
	char *output;
	struct cow_icon_placement placement;
	uint32_t fields;
};

void cow_icon_placement_default(struct cow_icon_placement *);
bool cow_icon_box_parse(const char *, struct cow_icon_box_config *);
bool cow_icon_fill_parse(const char *, const char *,
    enum cow_icon_direction *, enum cow_icon_direction *);
bool cow_icon_box_resolve(const struct cow_icon_box_config *,
    const struct cow_box *, struct cow_box *);
bool cow_icon_placement_find(const struct cow_icon_placement *,
    const struct cow_box *, int, int, const struct cow_icon_rect *, size_t,
    int *, int *);

void cow_icon_placement_config_init(struct cow_config *);
void cow_icon_placement_config_fini(struct cow_config *);
struct cow_icon_placement_override *cow_icon_placement_override_get(
    struct cow_config *, const char *, bool);
void cow_icon_placement_override_remove(struct cow_config *, const char *);
void cow_icon_placement_resolve(const struct cow_config *,
    const struct cow_output *, struct cow_icon_placement *);

#endif
