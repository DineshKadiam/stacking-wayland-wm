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
#include "cow/colour.h"
#include "cow/compat.h"
#include "cow/config.h"
#include "cow/ipc.h"
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

static uint32_t
menu_style_field(const char *key)
{
	if (strcmp(key, "bg") == 0)
		return (COW_MENU_STYLE_BG);
	if (strcmp(key, "fg") == 0)
		return (COW_MENU_STYLE_FG);
	if (strcmp(key, "title.bg") == 0)
		return (COW_MENU_STYLE_TITLE_BG);
	if (strcmp(key, "title.fg") == 0)
		return (COW_MENU_STYLE_TITLE_FG);
	if (strcmp(key, "separator") == 0)
		return (COW_MENU_STYLE_SEPARATOR);
	if (strcmp(key, "bg.gradient") == 0)
		return (COW_MENU_STYLE_BG_GRADIENT);
	if (strcmp(key, "title.bg.gradient") == 0)
		return (COW_MENU_STYLE_TITLE_BG_GRADIENT);
	if (strcmp(key, "border.width") == 0)
		return (COW_MENU_STYLE_BORDER_WIDTH);
	if (strcmp(key, "padding.x") == 0)
		return (COW_MENU_STYLE_PADDING_X);
	if (strcmp(key, "padding.y") == 0)
		return (COW_MENU_STYLE_PADDING_Y);
	if (strcmp(key, "title.underlines") == 0)
		return (COW_MENU_STYLE_TITLE_UNDERLINES);
	if (strcmp(key, "automatic.hotkeys") == 0)
		return (COW_MENU_STYLE_AUTOMATIC_HOTKEYS);
	if (strcmp(key, "selection.relief") == 0)
		return (COW_MENU_STYLE_SELECTION_RELIEF);
	return (0);
}

static bool
menu_style_field_changes_geometry(uint32_t field)
{
	return (field == COW_MENU_STYLE_BORDER_WIDTH ||
	    field == COW_MENU_STYLE_PADDING_X ||
	    field == COW_MENU_STYLE_PADDING_Y);
}

static enum cmd_retval
menu_style_set_colour(const char *key, const char *val, float *dst,
    struct cmd_q *cmdq)
{
	if (!cow_colour_parse(val, dst)) {
		cmdq_error(cmdq, "menu-style: %s: invalid colour value", key);
		return (CMD_RETURN_ERROR);
	}
	return (CMD_RETURN_NORMAL);
}

static enum cmd_retval
menu_style_set_gradient(const char *key, const char *val,
    struct cow_gradient *dst, struct cmd_q *cmdq)
{
	if (!cow_gradient_parse(dst, val)) {
		cmdq_error(cmdq,
		    "menu-style: %s: expected none|HGradient colour colour|"
		    "VGradient colour colour",
		    key);
		return (CMD_RETURN_ERROR);
	}
	return (CMD_RETURN_NORMAL);
}

static enum cmd_retval
menu_style_set_int(const char *key, const char *val, int *dst, long long lo,
    long long hi, struct cmd_q *cmdq)
{
	const char *errstr;
	long long   v = strtonum(val, lo, hi, &errstr);

	if (errstr != NULL) {
		cmdq_error(cmdq, "menu-style: %s: bad value: %s", key, val);
		return (CMD_RETURN_ERROR);
	}
	*dst = (int)v;
	return (CMD_RETURN_NORMAL);
}

static enum cmd_retval
cmd_menu_style_exec(struct cmd *self, struct cmd_q *cmdq)
{
	struct args *args = self->args;
	const char  *menu_name = args_get(args, 'm');
	bool	     reset = args_has(args, 'r');

	if (reset && menu_name == NULL) {
		cmdq_error(cmdq, "menu-style: -r requires -m NAME");
		return (CMD_RETURN_ERROR);
	}
	if ((reset && args->argc != 1) || (!reset && args->argc < 2)) {
		cmdq_error(cmdq, "usage: %s", self->entry->usage);
		return (CMD_RETURN_ERROR);
	}
	if (cow_config == NULL)
		return (CMD_RETURN_ERROR);

	const char *key   = args->argv[0];
	uint32_t    field = menu_style_field(key);
	if (field == 0) {
		cmdq_error(cmdq, "menu-style: unknown key: %s", key);
		return (CMD_RETURN_ERROR);
	}

	struct cow_menu *menu = NULL;
	if (menu_name != NULL)
		menu = cow_menu_find(menu_name);
	if (reset) {
		if (menu == NULL)
			menu = cow_menu_create(menu_name);
		if (menu == NULL) {
			cmdq_error(cmdq, "menu-style: cannot create menu \"%s\"",
			    menu_name);
			return (CMD_RETURN_ERROR);
		}
		menu->style_overrides &= ~field;
		if (!cow_ipc.startup_mode)
			cow_menu_style_refresh(menu,
			    menu_style_field_changes_geometry(field));
		return (CMD_RETURN_NORMAL);
	}

	char *val = cmd_argv_to_string(args->argc, args->argv, 1);
	struct cow_menu_style named_style = { 0 };
	if (menu != NULL)
		named_style = menu->style;
	struct cow_menu_style *st = menu_name != NULL ? &named_style :
						      &cow_config->menu_style;
	enum cmd_retval ret = CMD_RETURN_NORMAL;

	struct {
		const char *name;
		float	   *ptr;
	} colours[] = {
		{ "bg", st->bg },
		{ "fg", st->fg },
		{ "title.bg", st->title_bg },
		{ "title.fg", st->title_fg },
		{ "separator", st->separator },
		{ NULL, NULL },
	};
	for (int i = 0; colours[i].name != NULL; i++) {
		if (strcmp(key, colours[i].name) == 0) {
			ret = menu_style_set_colour(key, val, colours[i].ptr,
			    cmdq);
			goto out;
		}
	}

	struct {
		const char	    *name;
		struct cow_gradient *ptr;
	} gradients[] = {
		{ "bg.gradient", &st->bg_gradient },
		{ "title.bg.gradient", &st->title_bg_gradient },
		{ NULL, NULL },
	};
	for (int i = 0; gradients[i].name != NULL; i++) {
		if (strcmp(key, gradients[i].name) == 0) {
			ret = menu_style_set_gradient(key, val,
			    gradients[i].ptr, cmdq);
			goto out;
		}
	}

	struct {
		const char *name;
		int	   *ptr;
		long long  lo;
		long long  hi;
	} ints[] = {
		{ "border.width", &st->border_width, 0, 20 },
		{ "padding.x", &st->padding_x, 0, 100 },
		{ "padding.y", &st->padding_y, 0, 100 },
		{ "title.underlines", &st->title_underlines, 0, 2 },
		{ NULL, NULL, 0, 0 },
	};
	for (int i = 0; ints[i].name != NULL; i++) {
		if (strcmp(key, ints[i].name) == 0) {
			ret = menu_style_set_int(key, val, ints[i].ptr,
			    ints[i].lo, ints[i].hi, cmdq);
			goto out;
		}
	}

	if (strcmp(key, "automatic.hotkeys") == 0) {
		bool enabled;
		if (!cmd_parse_bool(val, &enabled)) {
			cmdq_error(cmdq,
			    "menu-style: %s: expected boolean", key);
			ret = CMD_RETURN_ERROR;
			goto out;
		}
		st->automatic_hotkeys = enabled;
		goto out;
	}

	if (strcmp(key, "selection.relief") == 0) {
		if (strcmp(val, "none") == 0) {
			st->selection_relief = COW_MENU_SELECTION_RELIEF_NONE;
		} else if (strcmp(val, "raised") == 0) {
			st->selection_relief = COW_MENU_SELECTION_RELIEF_RAISED;
		} else {
			cmdq_error(cmdq,
			    "menu-style: %s: expected none|raised", key);
			ret = CMD_RETURN_ERROR;
		}
		goto out;
	}

out:
	if (ret == CMD_RETURN_NORMAL) {
		if (menu_name != NULL && menu == NULL)
			menu = cow_menu_create(menu_name);
		if (menu_name != NULL && menu == NULL) {
			cmdq_error(cmdq, "menu-style: cannot create menu \"%s\"",
			    menu_name);
			ret = CMD_RETURN_ERROR;
		} else if (menu != NULL) {
			menu->style = named_style;
			menu->style_overrides |= field;
		}
		if (ret == CMD_RETURN_NORMAL && !cow_ipc.startup_mode)
			cow_menu_style_refresh(menu,
			    menu_style_field_changes_geometry(field));
	}
	xfree(val);
	return (ret);
}

struct cmd_entry cmd_menu_style_entry = {
	"menu-style",
	"+m:r",
	1,
	-1,
	"menu-style [-m NAME] KEY VALUE | menu-style -m NAME -r KEY",
	cmd_menu_style_exec,
	0,
};
