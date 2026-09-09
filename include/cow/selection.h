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
#ifndef COW_SELECTION_H
#define COW_SELECTION_H

#include <stdbool.h>
#include <stddef.h>

#include <wayland-util.h>

struct cow_view;

#define COW_SELECTION_DEFAULT "default"

struct cow_selection_entry {
	struct wl_list link;
	char	      *name;
};

const char *cow_selection_name(const char *);
bool	    cow_selection_has(const struct cow_view *, const char *);
bool	    cow_selection_add(struct cow_view *, const char *);
bool	    cow_selection_remove(struct cow_view *, const char *);
bool	    cow_selection_toggle(struct cow_view *, const char *);
void	    cow_selection_clear(const char *);
void	    cow_selection_fini_view(struct cow_view *);

size_t cow_selection_collect(const char *, struct cow_view ***);
void   cow_selection_replace(const char *, struct cow_view **, size_t);

#endif /* COW_SELECTION_H */
