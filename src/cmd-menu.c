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

/* Menu commands. */

#include <stdlib.h>
#include <string.h>

#include "cow/cmd.h"
#include "cow/compat.h"
#include "cow/memory.h"
#include "cow/menu.h"
#include "cow/seat.h"
#include "cow/wm.h"

static enum cmd_retval
cmd_menu_add_exec(struct cmd *self, struct cmd_q *cmdq)
{
	struct args *args = self->args;
	if (args->argc < 3) {
		cmdq_error(cmdq, "menu-add: usage: NAME LABEL ACTION [args]");
		return (CMD_RETURN_ERROR);
	}
	const char *name  = args->argv[0];
	const char *label = args->argv[1];

	char *action = cmd_argv_to_string(args->argc, args->argv, 2);

	struct cow_menu *m = cow_menu_find(name);
	if (m == NULL)
		m = cow_menu_create(name);
	if (m == NULL) {
		cmdq_error(cmdq, "menu-add: cannot create menu \"%s\"", name);
		xfree(action);
		return (CMD_RETURN_ERROR);
	}

	enum cow_menu_item_type type	 = COW_MENU_ITEM_NORMAL;
	bool			is_popup = false;
	bool			popup_pick_context = false;
	const char	       *act	 = action;

	if (strcmp(label, "-") == 0 && strcmp(action, "-") == 0) {
		type = COW_MENU_ITEM_SEPARATOR;
		act  = NULL;
	} else if (strcmp(action, "title") == 0) {
		type = COW_MENU_ITEM_TITLE;
		act  = NULL;
	} else if (strncmp(action, "popup ", 6) == 0) {
		is_popup = true;
		act	 = action + 6;
		while (*act == ' ')
			act++;
	} else if (strncmp(action, "popup-pick ", 11) == 0) {
		is_popup	   = true;
		popup_pick_context = true;
		act		   = action + 11;
		while (*act == ' ')
			act++;
	}

	cow_menu_item_add(m, type,
	    type == COW_MENU_ITEM_SEPARATOR ? NULL : label, act, is_popup,
	    popup_pick_context);
	xfree(action);
	return (CMD_RETURN_NORMAL);
}

struct cmd_entry cmd_menu_add_entry = {
	"menu-add",
	"",
	3,
	-1,
	"menu-add NAME LABEL ACTION",
	cmd_menu_add_exec,
	0,
};

static enum cmd_retval
cmd_menu_clear_exec(struct cmd *self, struct cmd_q *cmdq)
{
	struct args *args = self->args;
	if (args->argc < 1) {
		cmdq_error(cmdq, "menu-clear: usage: NAME");
		return (CMD_RETURN_ERROR);
	}
	struct cow_menu *m = cow_menu_find(args->argv[0]);
	if (m == NULL) {
		cmdq_error(cmdq, "menu-clear: no such menu: %s", args->argv[0]);
		return (CMD_RETURN_ERROR);
	}
	cow_menu_clear(m);
	return (CMD_RETURN_NORMAL);
}

struct cmd_entry cmd_menu_clear_entry = {
	"menu-clear",
	"",
	1,
	1,
	"menu-clear NAME",
	cmd_menu_clear_exec,
	0,
};

static enum cmd_retval
cmd_menu_destroy_exec(struct cmd *self, struct cmd_q *cmdq)
{
	struct args *args = self->args;
	if (args->argc < 1) {
		cmdq_error(cmdq, "menu-destroy: usage: NAME");
		return (CMD_RETURN_ERROR);
	}
	struct cow_menu *m = cow_menu_find(args->argv[0]);
	if (m == NULL) {
		cmdq_error(cmdq, "menu-destroy: no such menu: %s",
		    args->argv[0]);
		return (CMD_RETURN_ERROR);
	}
	cow_menu_destroy(m);
	return (CMD_RETURN_NORMAL);
}

struct cmd_entry cmd_menu_destroy_entry = {
	"menu-destroy",
	"",
	1,
	1,
	"menu-destroy NAME",
	cmd_menu_destroy_exec,
	0,
};

static enum cmd_retval
cmd_menu_show_exec(struct cmd *self, struct cmd_q *cmdq)
{
	struct args *args = self->args;
	int	     x	  = INT32_MIN;
	int	     y	  = INT32_MIN;
	const char  *name = NULL;

	for (int i = 0; i < args->argc; i++) {
		const char *a = args->argv[i];
		if (strcmp(a, "-x") == 0 && i + 1 < args->argc) {
			x = atoi(args->argv[++i]);
		} else if (strcmp(a, "-y") == 0 && i + 1 < args->argc) {
			y = atoi(args->argv[++i]);
		} else if (name == NULL) {
			name = a;
		}
	}
	if (name == NULL) {
		cmdq_error(cmdq, "menu-show: usage: NAME [-x N] [-y N]");
		return (CMD_RETURN_ERROR);
	}

	struct cow_menu *m = cow_menu_find(name);
	if (m == NULL) {
		cmdq_error(cmdq, "menu-show: no such menu: %s", name);
		return (CMD_RETURN_ERROR);
	}

	bool centre_title_on_pointer = (x == INT32_MIN && y == INT32_MIN);

	if (x == INT32_MIN || y == INT32_MIN) {
		int px, py;
		if (cow_seat_get_pointer(&px, &py)) {
			if (x == INT32_MIN)
				x = px;
			if (y == INT32_MIN)
				y = py;
		} else if (cow_wm.has_raw_pointer) {
			if (x == INT32_MIN)
				x = cow_wm.raw_pointer_x;
			if (y == INT32_MIN)
				y = cow_wm.raw_pointer_y;
		} else {
			if (x == INT32_MIN)
				x = 0;
			if (y == INT32_MIN)
				y = 0;
		}
	}

	cow_menu_popup_open(m, x, y, cmdq->target.window, NULL,
	    centre_title_on_pointer);
	cow_wm.needs_manage_dirty = true;
	return (CMD_RETURN_NORMAL);
}

struct cmd_entry cmd_menu_show_entry = {
	"menu-show",
	"",
	1,
	-1,
	"menu-show NAME [-x N] [-y N]",
	cmd_menu_show_exec,
	0,
};

static enum cmd_retval
cmd_menu_key_exec(struct cmd *self, struct cmd_q *cmdq)
{
	struct args *args = self->args;
	if (args->argc < 1) {
		cmdq_error(cmdq,
		    "menu-key: usage: up|down|left|right|select|close");
		return (CMD_RETURN_ERROR);
	}
	cow_menu_navigate(args->argv[0]);
	cow_wm.needs_manage_dirty = true;
	return (CMD_RETURN_NORMAL);
}

struct cmd_entry cmd_menu_key_entry = {
	"menu-key",
	"",
	1,
	1,
	"menu-key DIRECTION",
	cmd_menu_key_exec,
	0,
};
