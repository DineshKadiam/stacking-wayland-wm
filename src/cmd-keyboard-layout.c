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

#include "cow/cmd.h"
#include "cow/keyboard_layout.h"
#include "cow/menu.h"
#include "cow/output.h"
#include "cow/update.h"
#include "cow/util.h"
#include "cow/wm.h"

#define KEYBOARD_LAYOUT_MENU "\037cow-keyboard-layout"

static enum cmd_retval cmd_keyboard_layout_exec(struct cmd *, struct cmd_q *);

struct cmd_entry cmd_keyboard_layout_entry = {
	"keyboard-layout",
	"",
	0,
	1,
	"[NAME]",
	cmd_keyboard_layout_exec,
	0,
};

static enum cmd_retval
cmd_keyboard_layout_exec(struct cmd *self, struct cmd_q *cmdq)
{
	if (cow_wm.xkb_config == NULL) {
		cmdq_error(cmdq, "keyboard-layout: river_xkb_config_v1 unavailable");
		return CMD_RETURN_ERROR;
	}

	if (self->args->argc == 0) {
		struct cow_keyboard_layout_summary *summaries = NULL;
		size_t count = cow_keyboard_layout_summaries(&summaries);
		if (count == 0) {
			cmdq_error(cmdq,
			    "keyboard-layout: no layouts have been reported");
			return CMD_RETURN_ERROR;
		}
		struct cow_menu *menu = cow_menu_generated_begin(
		    KEYBOARD_LAYOUT_MENU, "Keyboard Layout (type to search)",
		    true, true);
		if (menu == NULL) {
			cow_keyboard_layout_summaries_free(summaries, count);
			cmdq_error(cmdq,
			    "keyboard-layout: cannot create layout menu");
			return CMD_RETURN_ERROR;
		}
		for (size_t i = 0; i < count; i++) {
			char *label;
			xasprintf(&label, "%s%s", summaries[i].name,
			    summaries[i].active ? " (active)" : "");
			const char *argv[] = { "keyboard-layout",
			    summaries[i].name };
			char *action = cow_command_from_argv(2, argv);
			cow_menu_item_add(menu, COW_MENU_ITEM_NORMAL, label,
			    action, false, false, NULL);
			xfree(action);
			xfree(label);
		}
		cow_keyboard_layout_summaries_free(summaries, count);
		cow_menu_popup_open_on_output(menu,
		    cmd_ctx_target_output(cmdq), NULL);
		cow_update_request(COW_UPDATE_MANAGE);
		return CMD_RETURN_NORMAL;
	}

	if (!cow_keyboard_layout_set_name(self->args->argv[0])) {
		cmdq_error(cmdq, "keyboard-layout: no keyboards available");
		return CMD_RETURN_ERROR;
	}
	return CMD_RETURN_NORMAL;
}
