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
#include <unistd.h>

#include "cow/cmd.h"
#include "cow/colour.h"
#include "cow/compat.h"
#include "cow/config.h"
#include "cow/image.h"
#include "cow/ipc.h"
#include "cow/log.h"
#include "cow/memory.h"
#include "cow/menu.h"
#include "cow/seat.h"
#include "cow/util.h"
#include "cow/update.h"
#include "cow/wm.h"

static char *
menu_image_validate(char *path, const char *what)
{
	char *cause = NULL;
	if (!cow_image_validate(path, &cause)) {
		cow_log_warn("%s ignored: %s", what, cause);
		xfree(cause);
		xfree(path);
		return (NULL);
	}
	return (path);
}

static char *
menu_icon_path_resolve(const char *paths, struct cmd_q *cmdq)
{
	char *base = NULL;
	if (cmdq->cmd != NULL && cmdq->cmd->file != NULL)
		base = cow_path_dirname(cmdq->cmd->file);
	char *copy = xstrdup(paths);
	char *resolved = xstrdup("");
	char *save = NULL;
	for (char *part = strtok_r(copy, ":", &save); part != NULL;
	    part = strtok_r(NULL, ":", &save)) {
		char *entry = cow_path_resolve(base, part);
		if (entry == NULL) {
			xfree(resolved);
			resolved = NULL;
			break;
		}
		char *next;
		xasprintf(&next, "%s%s%s", resolved, resolved[0] ? ":" : "",
		    entry);
		xfree(entry);
		xfree(resolved);
		resolved = next;
	}
	xfree(copy);
	xfree(base);
	return (resolved);
}

static char *
menu_image_resolve(const char *name, const struct cow_menu *menu,
    const char *what)
{
	if (strchr(name, '/') != NULL) {
		char *path = cow_path_resolve(NULL, name);
		if (path == NULL) {
			cow_log_warn("%s ignored: cannot expand path: %s", what,
			    name);
			return (NULL);
		}
		return (menu_image_validate(path, what));
	}

	struct cow_menu_style style;
	cow_menu_style_resolve(menu, &style);
	const char *search = style.icon_path;
	if (search == NULL || search[0] == '\0') {
		cow_log_warn("%s ignored: icon.path is empty (%s)", what, name);
		return (NULL);
	}
	char *copy = xstrdup(search);
	char *save = NULL;
	for (char *dir = strtok_r(copy, ":", &save); dir != NULL;
	    dir = strtok_r(NULL, ":", &save)) {
		char *path;
		xasprintf(&path, "%s/%s", dir, name);
		cow_log_debug("menu-add: looking for icon: %s", path);
		if (access(path, R_OK) != 0) {
			xfree(path);
			continue;
		}
		xfree(copy);
		return (menu_image_validate(path, what));
	}
	xfree(copy);
	cow_log_warn("%s not found: %s (icon.path: %s)", what, name, search);
	return (NULL);
}

static enum cmd_retval
cmd_menu_add_exec(struct cmd *self, struct cmd_q *cmdq)
{
	struct args *args = self->args;
	if (args->argc < 3) {
		cmdq_error(cmdq,
		    "menu-add: usage: [-M] [-T] [-i ICON] NAME LABEL ACTION "
		    "[args]");
		return (CMD_RETURN_ERROR);
	}
	const char *name  = args->argv[0];
	const char *label = args->argv[1];

	char *action = cmd_argv_to_string(args->argc, args->argv, 2);

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
	} else if (strcmp(action, "accept") == 0) {
		type = COW_MENU_ITEM_ACCEPT;
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
	const char *icon_arg = args_get(args, 'i');
	if (icon_arg != NULL && type != COW_MENU_ITEM_NORMAL) {
		cmdq_error(cmdq,
		    "menu-add: icons may only be set on selectable items");
		xfree(action);
		return (CMD_RETURN_ERROR);
	}
	struct cow_menu *m = cow_menu_find(name);
	char *icon = icon_arg != NULL ?
	    menu_image_resolve(icon_arg, m, "menu-add: icon") :
	    NULL;
	if (m == NULL)
		m = cow_menu_create(name);
	if (m == NULL) {
		cmdq_error(cmdq, "menu-add: cannot create menu \"%s\"", name);
		xfree(icon);
		xfree(action);
		return (CMD_RETURN_ERROR);
	}
	if (args_has(args, 'T'))
		cow_menu_enable_typeahead(m);
	if (args_has(args, 'M'))
		cow_menu_enable_multi_select(m);

	cow_menu_item_add(m, type,
	    type == COW_MENU_ITEM_SEPARATOR ? NULL : label, act, is_popup,
	    popup_pick_context, icon);
	xfree(icon);
	xfree(action);
	return (CMD_RETURN_NORMAL);
}

struct cmd_entry cmd_menu_add_entry = {
	"menu-add",
	"+MTi:",
	3,
	-1,
	"menu-add [-M] [-T] [-i ICON] NAME LABEL ACTION",
	cmd_menu_add_exec,
	CMD_CONFIG,
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
	CMD_CONFIG,
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
	CMD_CONFIG,
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
	cow_update_request(COW_UPDATE_MANAGE);
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
	if (strcmp(key, "border.colour") == 0)
		return (COW_MENU_STYLE_BORDER_COLOUR);
	if (strcmp(key, "bg.gradient") == 0)
		return (COW_MENU_STYLE_BG_GRADIENT);
	if (strcmp(key, "title.bg.gradient") == 0)
		return (COW_MENU_STYLE_TITLE_BG_GRADIENT);
	if (strcmp(key, "border.width") == 0)
		return (COW_MENU_STYLE_BORDER_WIDTH);
	if (strcmp(key, "border.relief") == 0)
		return (COW_MENU_STYLE_BORDER_RELIEF);
	if (strcmp(key, "padding.x") == 0)
		return (COW_MENU_STYLE_PADDING_X);
	if (strcmp(key, "padding.y") == 0)
		return (COW_MENU_STYLE_PADDING_Y);
	if (strcmp(key, "icon.size") == 0)
		return (COW_MENU_STYLE_ICON_SIZE);
	if (strcmp(key, "icon.gap") == 0)
		return (COW_MENU_STYLE_ICON_GAP);
	if (strcmp(key, "icon.path") == 0)
		return (COW_MENU_STYLE_ICON_PATH);
	if (strcmp(key, "side.colour") == 0)
		return (COW_MENU_STYLE_SIDE_COLOUR);
	if (strcmp(key, "side.image") == 0)
		return (COW_MENU_STYLE_SIDE_IMAGE);
	if (strcmp(key, "side.width") == 0)
		return (COW_MENU_STYLE_SIDE_WIDTH);
	if (strcmp(key, "font") == 0)
		return (COW_MENU_STYLE_FONT);
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
	    field == COW_MENU_STYLE_PADDING_Y ||
	    field == COW_MENU_STYLE_ICON_SIZE ||
	    field == COW_MENU_STYLE_ICON_GAP ||
	    field == COW_MENU_STYLE_SIDE_WIDTH ||
	    field == COW_MENU_STYLE_SIDE_COLOUR ||
	    field == COW_MENU_STYLE_SIDE_IMAGE ||
	    field == COW_MENU_STYLE_FONT);
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
		if (field == COW_MENU_STYLE_ICON_PATH) {
			xfree(menu->style.icon_path);
			menu->style.icon_path = NULL;
		}
		if (field == COW_MENU_STYLE_SIDE_IMAGE) {
			xfree(menu->style.side_image);
			menu->style.side_image = NULL;
		}
		if (field == COW_MENU_STYLE_FONT) {
			xfree(menu->style.font);
			menu->style.font = NULL;
		}
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

	if (strcmp(key, "icon.path") == 0) {
		char *path = menu_icon_path_resolve(val, cmdq);
		if (path == NULL || path[0] == '\0') {
			cmdq_error(cmdq,
			    "menu-style: icon.path: cannot resolve path");
			xfree(path);
			ret = CMD_RETURN_ERROR;
			goto out;
		}
		if (menu_name == NULL) {
			xfree(cow_config->menu_style.icon_path);
			cow_config->menu_style.icon_path = path;
		} else {
			if (menu == NULL)
				menu = cow_menu_create(menu_name);
			if (menu == NULL) {
				cmdq_error(cmdq,
				    "menu-style: cannot create menu \"%s\"",
				    menu_name);
				xfree(path);
				ret = CMD_RETURN_ERROR;
				goto out;
			}
			xfree(menu->style.icon_path);
			menu->style.icon_path = path;
			menu->style_overrides |= field;
		}
		if (!cow_ipc.startup_mode)
			cow_menu_style_refresh(menu, false);
		xfree(val);
		return (CMD_RETURN_NORMAL);
	}
	if (strcmp(key, "side.image") == 0) {
		char *path = NULL;
		if (strcmp(val, "none") != 0) {
			path = menu_image_resolve(val, menu,
			    "menu-style: side.image");
			if (path == NULL) {
				cmdq_error(cmdq,
				    "menu-style: side.image: cannot resolve image");
				xfree(val);
				return (CMD_RETURN_ERROR);
			}
		}
		if (menu_name == NULL) {
			xfree(cow_config->menu_style.side_image);
			cow_config->menu_style.side_image = path;
		} else {
			if (menu == NULL)
				menu = cow_menu_create(menu_name);
			if (menu == NULL) {
				cmdq_error(cmdq,
				    "menu-style: cannot create menu \"%s\"",
				    menu_name);
				xfree(path);
				xfree(val);
				return (CMD_RETURN_ERROR);
			}
			xfree(menu->style.side_image);
			menu->style.side_image = path;
			menu->style_overrides |= field;
		}
		if (!cow_ipc.startup_mode)
			cow_menu_style_refresh(menu, true);
		xfree(val);
		return (CMD_RETURN_NORMAL);
	}
	if (strcmp(key, "font") == 0) {
		if (val[0] == '\0') {
			cmdq_error(cmdq, "menu-style: font cannot be empty");
			ret = CMD_RETURN_ERROR;
			goto out;
		}
		if (menu_name == NULL) {
			xstrset(&cow_config->menu_style.font, val);
		} else {
			if (menu == NULL)
				menu = cow_menu_create(menu_name);
			if (menu == NULL) {
				cmdq_error(cmdq,
				    "menu-style: cannot create menu \"%s\"",
				    menu_name);
				ret = CMD_RETURN_ERROR;
				goto out;
			}
			xstrset(&menu->style.font, val);
			menu->style_overrides |= field;
		}
		if (!cow_ipc.startup_mode)
			cow_menu_style_refresh(menu, true);
		xfree(val);
		return (CMD_RETURN_NORMAL);
	}

	struct {
		const char *name;
		float	   *ptr;
	} colours[] = {
		{ "bg", st->bg },
		{ "fg", st->fg },
		{ "title.bg", st->title_bg },
		{ "title.fg", st->title_fg },
		{ "separator", st->separator },
		{ "border.colour", st->border },
		{ "side.colour", st->side_colour },
		{ NULL, NULL },
	};
	for (int i = 0; colours[i].name != NULL; i++) {
		if (strcmp(key, colours[i].name) == 0) {
			if (strcmp(key, "side.colour") == 0 &&
			    strcmp(val, "none") == 0)
				memset(colours[i].ptr, 0,
				    sizeof(st->side_colour));
			else
				ret = menu_style_set_colour(key, val,
				    colours[i].ptr, cmdq);
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
		{ "icon.size", &st->icon_size, 0, 256 },
		{ "icon.gap", &st->icon_gap, 0, 100 },
		{ "side.width", &st->side_width, 1, 256 },
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
		} else if (strcmp(val, "sunken") == 0) {
			st->selection_relief = COW_MENU_SELECTION_RELIEF_SUNKEN;
		} else {
			cmdq_error(cmdq,
			    "menu-style: %s: expected none|raised|sunken", key);
			ret = CMD_RETURN_ERROR;
		}
		goto out;
	}

	if (strcmp(key, "border.relief") == 0) {
		if (strcmp(val, "raised") == 0) {
			st->border_relief = COW_MENU_RELIEF_RAISED;
		} else if (strcmp(val, "flat") == 0) {
			st->border_relief = COW_MENU_RELIEF_FLAT;
		} else if (strcmp(val, "sunken") == 0) {
			st->border_relief = COW_MENU_RELIEF_SUNKEN;
		} else {
			cmdq_error(cmdq,
			    "menu-style: border.relief: %s: "
			    "expected raised|flat|sunken", val);
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
	CMD_CONFIG,
};
