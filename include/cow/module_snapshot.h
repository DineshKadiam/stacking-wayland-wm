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
#if !defined(COW_MODULE_SNAPSHOT_H)
#define COW_MODULE_SNAPSHOT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cow/module_theme.h"

struct cow_module_desk {
	int  nr;
	int  window_count;
	bool active;
	bool collected;
	char name[128];
};

struct cow_module_page_background {
	int desk, col, row;
	bool has_colour;
	float colour[4];
	char *image;
};

struct cow_module_output_state {
	char name[128];
	int  number;
	int  x, y, w, h;
	int  usable_x, usable_y, usable_w, usable_h;
	int  current;
	int  current_desk;
	int  page_cols, page_rows;
	int  page_col, page_row;
	int  page_vx, page_vy;
	int  output_w, output_h;
	bool has_background;
	float background[4];
	char *background_image;
	struct cow_module_desk *desks;
	size_t n_desks;
	struct cow_module_page_background *page_backgrounds;
	size_t n_page_backgrounds;
};

struct cow_module_window_state {
	char id[128];
	char container_window_id[128];
	char app_id[128];
	char title[256];
	char output[128];
	char icon[1024];
	int  desk;
	int  x, y;
	int  vx, vy, w, h;
	int  icon_vx, icon_vy, icon_w, icon_h;
	int  focused;
	int  iconified;
	bool container_hidden;
	uint8_t layer;
	uint64_t raise_seq;
};

struct cow_module_snapshot {
	struct cow_module_output_state *outs;
	size_t n_outs;
	struct cow_module_window_state *wins;
	size_t n_wins;
	char desktop_config[32];
	char user_output[128];
	struct cow_module_theme theme;
	struct cow_module_theme titlebar_theme;
};

bool cow_module_snapshot_parse(struct cow_module_snapshot *, const char *);
void cow_module_snapshot_fini(struct cow_module_snapshot *);

#endif
