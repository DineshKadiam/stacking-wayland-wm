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

#ifndef COW_MENU_CHOICE_H
#define COW_MENU_CHOICE_H

#include <stdbool.h>
#include <stddef.h>

struct cow_output;
struct cow_view;

struct cow_menu_choice {
	char *label;
	char *action;
};

enum cow_menu_choice_result {
	COW_MENU_CHOICE_ERROR = -1,
	COW_MENU_CHOICE_EMPTY,
	COW_MENU_CHOICE_SINGLE,
	COW_MENU_CHOICE_SHOWN,
};

bool cow_menu_choice_init(struct cow_menu_choice *, const char *, size_t,
    const char *const *);
void cow_menu_choices_free(struct cow_menu_choice *, size_t);
enum cow_menu_choice_result cow_menu_choose(const char *, const char *,
    struct cow_menu_choice *, size_t, struct cow_output *, struct cow_view *);

#endif /* COW_MENU_CHOICE_H */
