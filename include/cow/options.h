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
#if !defined(COW_OPTIONS_H)
#define COW_OPTIONS_H

#include <stddef.h>

#include "cow/view.h"

enum cow_option_scope {
	COW_OPTION_GLOBAL,
	COW_OPTION_WINDOW,
	COW_OPTION_OUTPUT,
	COW_OPTION_MIXED,
};

enum cow_option_match {
	COW_OPTION_EXACT,
	COW_OPTION_PREFIX,
};

enum cow_option_action {
	COW_OPTION_SCALAR_BOOL,
	COW_OPTION_SCALAR_INT,
	COW_OPTION_SCALAR_STRING,
	COW_OPTION_COLOUR,
	COW_OPTION_IMAGE,
	COW_OPTION_BORDER_STYLE,
	COW_OPTION_DESKTOP_CONFIG,
	COW_OPTION_FOCUS,
	COW_OPTION_GEOMETRY_WINDOW,
	COW_OPTION_PLACEMENT,
	COW_OPTION_FONT,
	COW_OPTION_BUTTON,
	COW_OPTION_DESKS_COUNT,
	COW_OPTION_DESKTOP_SIZE,
	COW_OPTION_TITLEBAR_HEIGHT,
	COW_OPTION_TITLEBAR_POSITION,
	COW_OPTION_TITLEBAR_SQUEEZE,
	COW_OPTION_TITLEBAR_SQUEEZE_JUSTIFY,
	COW_OPTION_TITLEBAR_SQUEEZE_INT,
	COW_OPTION_LAYER,
};

enum cow_option_post {
	COW_OPTION_POST_NONE		  = 0,
	COW_OPTION_POST_REFRESH_VISUALS	  = 1 << 0,
	COW_OPTION_POST_PAN_FRAMES	  = 1 << 1,
	COW_OPTION_POST_ZERO_DISABLE_HANDLES = 1 << 2,
};

#define COW_OPTION_NO_OFFSET ((size_t)-1)

struct cow_option_entry {
	const char		 *name;
	enum cow_option_match	  match;
	enum cow_option_scope	  scope;
	enum cow_option_action	  action;
	enum cow_view_cfg_option  id;
	size_t			  config_offset;
	size_t			  view_offset;
	long long		  lo;
	long long		  hi;
	unsigned int		  post;
};

extern const struct cow_option_entry cow_options_table[];

#endif
