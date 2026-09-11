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

#include "cow/compat.h"

#include "cow/memory.h"
#include "cow/menu.h"
#include "cow/menu_choice.h"
#include "cow/update.h"
#include "cow/util.h"

bool
cow_menu_choice_init(struct cow_menu_choice *choice, const char *label,
    size_t argc, const char *const *argv)
{
	if (choice == NULL || label == NULL || argc == 0 || argv == NULL)
		return (false);
	choice->action = cow_command_from_argv(argc, argv);
	if (choice->action == NULL)
		return (false);
	choice->label = xstrdup(label);
	return (true);
}

void
cow_menu_choices_free(struct cow_menu_choice *choices, size_t count)
{
	if (choices == NULL)
		return;
	for (size_t i = 0; i < count; i++) {
		xfree(choices[i].label);
		xfree(choices[i].action);
	}
	xfree(choices);
}

enum cow_menu_choice_result
cow_menu_choose(const char *name, const char *title,
    struct cow_menu_choice *choices, size_t count, struct cow_output *output,
    struct cow_view *context_view)
{
	if (count == 0)
		return (COW_MENU_CHOICE_EMPTY);
	if (count == 1)
		return (COW_MENU_CHOICE_SINGLE);

	struct cow_menu *menu = cow_menu_generated_begin(name, title, true, true);
	if (menu == NULL)
		return (COW_MENU_CHOICE_ERROR);
	for (size_t i = 0; i < count; i++) {
		cow_menu_item_add(menu, COW_MENU_ITEM_NORMAL, choices[i].label,
		    choices[i].action, false, false, NULL);
	}
	cow_menu_popup_open_on_output(menu, output, context_view);
	cow_update_request(COW_UPDATE_MANAGE);
	return (COW_MENU_CHOICE_SHOWN);
}
