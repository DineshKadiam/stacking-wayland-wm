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

#include <cJSON.h>
#include <stdio.h>
#include <string.h>

#include "cow/alias.h"
#include "cow/cmd.h"
#include "cow/cmd-filter.h"
#include "cow/config.h"
#include "cow/container.h"
#include "cow/diagnostics.h"
#include "cow/ipc.h"
#include "cow/keyboard_layout.h"
#include "cow/menu.h"
#include "cow/output.h"
#include "cow/rule.h"
#include "cow/style.h"
#include "cow/wm.h"

#include "decor-profile.h"
#include "config-serialize.h"
#include "state-snapshot.h"
#include "state-value.h"

#include "river-window-management-v1-client-protocol.h"

static enum cmd_retval cmd_show_exec(struct cmd *, struct cmd_q *);
static enum cmd_retval show_collection(enum state_source, const char *,
    const char *, struct cmd_q *);
static enum cmd_retval show_record(const char *, struct state_value *,
    const char *, struct cmd_q *);
static enum cmd_retval do_show_config(const char *, const char *,
    struct cmd_q *);
static enum cmd_retval show_defaults(const char *, struct cmd_q *);
static cJSON	      *show_config_value(const char *, struct cmd_q *);
static cJSON	      *show_config_object(struct cmd_q *);
static void	       json_add_snapshot(cJSON *, const char *,
    struct state_value *);
static void	       json_add_config_command(const char *, void *);

static const char *show_config_keys[] = {
	"border.width",
	"step",
	"border.corner_length",
	"reserved.top",
	"reserved.bottom",
	"reserved.left",
	"reserved.right",
	"output.colour.background",
	"output.image.background",
	"snap_distance",
	"border.style",
	"desktop-configuration",
	"desktop_size",
	"focus",
	"geometry_window",
	"clamp_mode",
	"placement",
	"page_visibility",
	"page_scroll_animate",
	"page_scroll_ms",
	"edge_scroll",
	"edge_scroll_delay_ms",
	"edge_scroll_modifiers",
	"titlebar.font",
	"title_format",
	"cursor.theme",
	"cursor.size",
	"cursor.hide_timeout",
	"repeat_delay_ms",
	"repeat_interval_ms",
	"menu.repeat_keys",
	"raise_on_focus",
	"keyboard_layout.per_window",
	"preserve_desk_on_output_loss",
	"max_layer",
	"default_layer",
	"desks.count",
	NULL,
};

struct cmd_entry cmd_show_entry = {
	"show",
	"DaF:f:t:",
	1,
	2,
	"show [-D] [-f json|config] [-t target] TOPIC [name] | "
	"show -a [-f json|config] [-F expr] TOPIC",
	cmd_show_exec,
	0,
};

static bool
state_topic(const char *topic, enum state_source *source)
{
	if (strcmp(topic, "window") == 0)
		*source = STATE_SRC_WINDOWS;
	else if (strcmp(topic, "container") == 0)
		*source = STATE_SRC_CONTAINERS;
	else if (strcmp(topic, "pane") == 0)
		*source = STATE_SRC_PANES;
	else if (strcmp(topic, "output") == 0)
		*source = STATE_SRC_OUTPUTS;
	else if (strcmp(topic, "desk") == 0)
		*source = STATE_SRC_DESKS;
	else if (strcmp(topic, "rule") == 0)
		*source = STATE_SRC_RULES;
	else if (strcmp(topic, "style") == 0)
		*source = STATE_SRC_STYLES;
	else if (strcmp(topic, "decor") == 0)
		*source = STATE_SRC_DECOR;
	else if (strcmp(topic, "menu") == 0)
		*source = STATE_SRC_MENUS;
	else if (strcmp(topic, "binding") == 0)
		*source = STATE_SRC_BINDINGS;
	else if (strcmp(topic, "command") == 0)
		*source = STATE_SRC_COMMANDS;
	else if (strcmp(topic, "selection") == 0)
		*source = STATE_SRC_SELECTIONS;
	else if (strcmp(topic, "alias") == 0)
		*source = STATE_SRC_ALIASES;
	else
		return (false);
	return (true);
}

static enum cmd_retval
cmd_show_exec(struct cmd *self, struct cmd_q *cmdq)
{
	struct args *args = self->args;
	const char  *topic = args->argv[0];
	const char  *filter_expr = args_get(args, 'F');
	const char  *format = args_get(args, 'f');
	bool	     all = args_has(args, 'a');
	bool	     has_target = args_has(args, 't');
	enum state_source source;

	if (format == NULL)
		format = "json";
	if (strcmp(format, "json") != 0 && strcmp(format, "config") != 0) {
		cmdq_error(cmdq, "show: unknown format: %s", format);
		return (CMD_RETURN_ERROR);
	}

	if (args_has(args, 'D')) {
		if (strcmp(topic, "config") != 0 || args->argc != 1 || all ||
		    has_target || filter_expr != NULL) {
			cmdq_error(cmdq,
			    "show: -D requires config without a key or selectors");
			return CMD_RETURN_ERROR;
		}
		return show_defaults(format, cmdq);
	}
	if (all) {
		if (!state_topic(topic, &source)) {
			cmdq_error(cmdq, "show: -a is not valid with %s", topic);
			return (CMD_RETURN_ERROR);
		}
		if (has_target) {
			cmdq_error(cmdq, "show: -t is not valid with -a");
			return (CMD_RETURN_ERROR);
		}
		if (args->argc != 1) {
			cmdq_error(cmdq, "show: -a %s takes no name", topic);
			return (CMD_RETURN_ERROR);
		}
		return (show_collection(source, filter_expr, format, cmdq));
	}

	if (filter_expr != NULL) {
		cmdq_error(cmdq, "show: -F requires -a");
		return (CMD_RETURN_ERROR);
	}

	if (strcmp(topic, "decor") == 0 && args->argc == 2) {
		if (filter_expr != NULL || has_target) {
			cmdq_error(cmdq,
			    "show: -F and -t are not valid with named decor");
			return (CMD_RETURN_ERROR);
		}
		struct state_value *profile =
		    cow_decor_snapshot_named(args->argv[1]);
		if (profile == NULL) {
			cmdq_error(cmdq, "show: no decor profile named '%s'",
			    args->argv[1]);
			return (CMD_RETURN_ERROR);
		}
		return (show_record(topic, profile, format, cmdq));
	}
	if (strcmp(topic, "decor") == 0) {
		cmdq_error(cmdq, "show: decor requires a name or -a");
		return (CMD_RETURN_ERROR);
	}

	if (strcmp(topic, "container") == 0 || strcmp(topic, "pane") == 0) {
		if (args->argc != 1)
			goto no_name;
		struct cow_view *view = cmd_require_target_window(cmdq, "show");
		if (view == NULL)
			return CMD_RETURN_ERROR;
		struct cow_container *c = cow_view_container(view);
		if (c == NULL) {
			cmdq_error(cmdq,
			    "show: target window is not in a container");
			return CMD_RETURN_ERROR;
		}
		struct state_value *record = strcmp(topic, "container") == 0 ?
		    state_snapshot_container(c) :
		    state_snapshot_pane(c, cow_view_pane(view));
		return show_record(topic, record, format, cmdq);
	}

	if (strcmp(topic, "window") == 0) {
		if (args->argc != 1)
			goto no_name;
		struct cow_view *view = cmd_require_target_window(cmdq, "show");
		if (view == NULL)
			return (CMD_RETURN_ERROR);
		return (show_record(topic, state_snapshot_view(view), format,
		    cmdq));
	}

	if (strcmp(topic, "output") == 0) {
		if (args->argc != 1)
			goto no_name;
		struct cow_output *output = cmd_ctx_target_output(cmdq);
		if (output == NULL) {
			cmdq_error(cmdq, "show: no target output");
			return (CMD_RETURN_ERROR);
		}
		return (show_record(topic, state_snapshot_output(output), format,
		    cmdq));
	}

	if (strcmp(topic, "desk") == 0) {
		if (args->argc != 1)
			goto no_name;
		struct cow_desk *desk = cmd_ctx_target_desk(cmdq);
		if (desk == NULL) {
			cmdq_error(cmdq, "show: no target desk");
			return (CMD_RETURN_ERROR);
		}
		return (show_record(topic, state_snapshot_desk(desk), format,
		    cmdq));
	}

	if (strcmp(topic, "rule") == 0) {
		if (has_target) {
			cmdq_error(cmdq, "show: -t is not valid with rule");
			return (CMD_RETURN_ERROR);
		}
		if (args->argc != 2) {
			cmdq_error(cmdq, "show: rule requires a name");
			return (CMD_RETURN_ERROR);
		}
		struct cow_rule *rule = cow_rule_find_by_name(args->argv[1]);
		if (rule == NULL) {
			cmdq_error(cmdq, "show: no rule named '%s'",
			    args->argv[1]);
			return (CMD_RETURN_ERROR);
		}
		return (show_record(topic, state_snapshot_rule(rule), format,
		    cmdq));
	}

	if (strcmp(topic, "style") == 0) {
		if (has_target) {
			cmdq_error(cmdq, "show: -t is not valid with style");
			return (CMD_RETURN_ERROR);
		}
		if (args->argc != 2) {
			cmdq_error(cmdq, "show: style requires a name");
			return (CMD_RETURN_ERROR);
		}
		struct cow_style *style = cow_style_find_by_name(args->argv[1]);
		if (style == NULL) {
			cmdq_error(cmdq, "show: no style named '%s'",
			    args->argv[1]);
			return (CMD_RETURN_ERROR);
		}
		return (show_record(topic, state_snapshot_style(style), format,
		    cmdq));
	}

	if (strcmp(topic, "menu") == 0) {
		if (has_target) {
			cmdq_error(cmdq, "show: -t is not valid with menu");
			return (CMD_RETURN_ERROR);
		}
		if (args->argc != 2) {
			cmdq_error(cmdq, "show: menu requires a name");
			return (CMD_RETURN_ERROR);
		}
		struct cow_menu *menu = cow_menu_find(args->argv[1]);
		if (menu == NULL) {
			cmdq_error(cmdq, "show: no menu named '%s'",
			    args->argv[1]);
			return (CMD_RETURN_ERROR);
		}
		return (show_record(topic, state_snapshot_menu(menu), format,
		    cmdq));
	}

	if (strcmp(topic, "binding") == 0) {
		cmdq_error(cmdq, "show: binding requires -a");
		return (CMD_RETURN_ERROR);
	}

	if (strcmp(topic, "selection") == 0) {
		cmdq_error(cmdq, "show: selection requires -a");
		return (CMD_RETURN_ERROR);
	}

	if (strcmp(topic, "alias") == 0) {
		if (has_target) {
			cmdq_error(cmdq, "show: -t is not valid with alias");
			return (CMD_RETURN_ERROR);
		}
		if (args->argc != 2) {
			cmdq_error(cmdq, "show: alias requires a name or -a");
			return (CMD_RETURN_ERROR);
		}
		struct cow_alias *alias = cow_alias_find(args->argv[1]);
		if (alias == NULL) {
			cmdq_error(cmdq, "show: no alias named '%s'",
			    args->argv[1]);
			return (CMD_RETURN_ERROR);
		}
		return (show_record(topic, state_snapshot_alias(alias), format,
		    cmdq));
	}

	if (strcmp(topic, "command") == 0) {
		if (has_target) {
			cmdq_error(cmdq, "show: -t is not valid with command");
			return (CMD_RETURN_ERROR);
		}
		if (args->argc != 2) {
			cmdq_error(cmdq, "show: command requires a name");
			return (CMD_RETURN_ERROR);
		}
		struct cmd_entry *entry = cmd_find_cmd(args->argv[1]);
		if (entry == NULL) {
			cmdq_error(cmdq, "show: no command named '%s'",
			    args->argv[1]);
			return (CMD_RETURN_ERROR);
		}
		struct state_value *record = state_value_record();
		struct cow_alias *alias = entry == &cmd_alias_invoke_entry ?
		    cow_alias_find(args->argv[1]) : NULL;
		struct state_value *name = state_value_string(alias != NULL ?
		    alias->name : entry->name);
		struct state_value *usage = state_value_string(alias != NULL ?
		    alias->command : entry->usage);
		state_record_set(record, "name", name);
		state_record_set(record, "usage", usage);
		if (alias != NULL) {
			struct state_value *is_alias = state_value_bool(true);
			state_record_set(record, "alias", is_alias);
			state_value_unref(is_alias);
			if (alias->parameters != NULL) {
				struct state_value *parameters =
				    state_value_string(alias->parameters);
				state_record_set(record, "parameters", parameters);
				state_value_unref(parameters);
			}
		}
		state_value_unref(name);
		state_value_unref(usage);
		return (show_record(topic, record, format, cmdq));
	}

	if (strcmp(topic, "keyboard-layout") == 0) {
		if (has_target) {
			cmdq_error(cmdq,
			    "show: -t is not valid with keyboard-layout");
			return (CMD_RETURN_ERROR);
		}
		if (args->argc != 1)
			goto no_name;
		if (strcmp(format, "config") == 0) {
			cmdq_error(cmdq,
			    "show: keyboard-layout has no config representation");
			return (CMD_RETURN_ERROR);
		}
		cow_ipc_respond_data(cmdq->client_fd,
		    cow_keyboard_layout_active_json());
		return (CMD_RETURN_NORMAL);
	}

	if (strcmp(topic, "diagnostics") == 0) {
		if (has_target) {
			cmdq_error(cmdq,
			    "show: -t is not valid with diagnostics");
			return (CMD_RETURN_ERROR);
		}
		if (args->argc != 1)
			goto no_name;
		if (strcmp(format, "config") == 0) {
			cmdq_error(cmdq,
			    "show: diagnostics has no config representation");
			return (CMD_RETURN_ERROR);
		}
		cow_ipc_respond_data(cmdq->client_fd,
		    cow_diagnostics_json());
		return (CMD_RETURN_NORMAL);
	}

	if (strcmp(topic, "config") == 0) {
		return (do_show_config(args->argc == 2 ? args->argv[1] : NULL,
		    format, cmdq));
	}

	cmdq_error(cmdq, "show: unknown topic: %s", topic);
	return (CMD_RETURN_ERROR);

no_name:
	cmdq_error(cmdq, "show: %s takes no name", topic);
	return (CMD_RETURN_ERROR);
}

static enum cmd_retval
show_collection(enum state_source source, const char *filter_expr,
    const char *format, struct cmd_q *cmdq)
{
	struct cmd_filter filter;
	if (!cmd_filter_parse(&filter, cmdq, "show", "-F", filter_expr))
		return (CMD_RETURN_ERROR);

	struct state_value *all = state_snapshot_collection(source);
	struct state_value *result = state_value_list();
	for (size_t i = 0; i < all->u.list.n; i++) {
		struct state_value *record = all->u.list.items[i];
		bool matches = false;
		if (!cmd_filter_record(&filter, record, &matches)) {
			state_value_unref(result);
			state_value_unref(all);
			cmd_filter_finish(&filter, cmdq);
			cmd_filter_free(&filter);
			return (CMD_RETURN_ERROR);
		}
		if (matches)
			state_list_append(result, record);
	}
	state_value_unref(all);

	if (!cmd_filter_finish(&filter, cmdq)) {
		state_value_unref(result);
		cmd_filter_free(&filter);
		return (CMD_RETURN_ERROR);
	}
	cmd_filter_free(&filter);
	const char *topic = source == STATE_SRC_WINDOWS ? "window" :
	    source == STATE_SRC_CONTAINERS		? "container" :
	    source == STATE_SRC_PANES			? "pane" :
	    source == STATE_SRC_OUTPUTS			? "output" :
	    source == STATE_SRC_DESKS			? "desk" :
	    source == STATE_SRC_RULES			? "rule" :
	    source == STATE_SRC_STYLES			? "style" :
	    source == STATE_SRC_DECOR			? "decor" :
	    source == STATE_SRC_MENUS			? "menu" :
	    source == STATE_SRC_BINDINGS		? "binding" :
	    source == STATE_SRC_SELECTIONS		? "selection" :
	    source == STATE_SRC_ALIASES			? "alias" :
							  "command";
	return (show_record(topic, result, format, cmdq));
}

static enum cmd_retval
show_record(const char *topic, struct state_value *record, const char *format,
    struct cmd_q *cmdq)
{
	if (strcmp(format, "config") == 0) {
		char *text = cow_config_serialize_topic(topic, record);
		state_value_unref(record);
		if (text == NULL) {
			cmdq_error(cmdq,
			    "show: %s has no config representation", topic);
			return (CMD_RETURN_ERROR);
		}
		cow_ipc_respond_formatted(cmdq->client_fd, "config", text);
		xfree(text);
		return (CMD_RETURN_NORMAL);
	}
	cJSON *json = state_value_to_json(record);
	state_value_unref(record);
	if (json == NULL) {
		cmdq_error(cmdq, "show: cannot create JSON response");
		return (CMD_RETURN_ERROR);
	}
	cow_ipc_respond_data(cmdq->client_fd, json);
	return (CMD_RETURN_NORMAL);
}

static enum cmd_retval
show_defaults(const char *format, struct cmd_q *cmdq)
{
	const char *path = cow_config_defaults_path();
	FILE	   *file = fopen(path, "r");
	if (file == NULL) {
		cmdq_error(cmdq, "show: cannot read defaults: %s", path);
		return CMD_RETURN_ERROR;
	}
	char   *text = xstrdup(""), *line = NULL;
	size_t	size = 0, length = 0;
	ssize_t n;
	while ((n = getline(&line, &size, file)) >= 0) {
		text = xrealloc(text, length + n + 1, 1);
		memcpy(text + length, line, n + 1);
		length += n;
	}
	xfree(line);
	bool failed = ferror(file);
	fclose(file);
	if (failed) {
		xfree(text);
		cmdq_error(cmdq, "show: cannot read defaults: %s", path);
		return CMD_RETURN_ERROR;
	}
	if (strcmp(format, "config") == 0)
		cow_ipc_respond_formatted(cmdq->client_fd, "config", text);
	else {
		cJSON *result = cJSON_CreateObject();
		cJSON_AddStringToObject(result, "path", path);
		cJSON_AddBoolToObject(result, "enabled",
		    !cow_ipc.skip_defaults);
		cJSON_AddStringToObject(result, "config", text);
		cow_ipc_respond_data(cmdq->client_fd, result);
	}
	xfree(text);
	return CMD_RETURN_NORMAL;
}

static enum cmd_retval
do_show_config(const char *ckey, const char *format, struct cmd_q *cmdq)
{
	if (ckey != NULL) {
		cJSON *val = show_config_value(ckey, cmdq);
		if (val == NULL) {
			cmdq_error(cmdq, "show: unknown config key: %s", ckey);
			return (CMD_RETURN_ERROR);
		}
		if (strcmp(format, "config") == 0) {
			cJSON *setting = cJSON_CreateObject();
			cJSON_AddItemToObject(setting, ckey, val);
			char *text = cow_config_serialize_settings(setting);
			cJSON_Delete(setting);
			cow_ipc_respond_formatted(cmdq->client_fd, "config", text);
			xfree(text);
		} else
			cow_ipc_respond_data(cmdq->client_fd, val);
		return (CMD_RETURN_NORMAL);
	}

	cJSON *config = show_config_object(cmdq);
	if (strcmp(format, "config") == 0) {
		char *text = cow_config_commands_string(cow_config);
		cJSON_Delete(config);
		cow_ipc_respond_formatted(cmdq->client_fd, "config", text);
		xfree(text);
	} else {
		cJSON *all = cJSON_CreateObject();
		cJSON *commands = cJSON_AddArrayToObject(all, "commands");
		cow_config_visit_commands(cow_config, json_add_config_command,
		    commands);
		cJSON_AddItemToObject(all, "settings", config);
		json_add_snapshot(all, "decor",
		    state_snapshot_collection(STATE_SRC_DECOR));
		json_add_snapshot(all, "menu_style",
		    state_snapshot_menu_style());
		json_add_snapshot(all, "menus",
		    state_snapshot_collection(STATE_SRC_MENUS));
		json_add_snapshot(all, "bindings",
		    state_snapshot_collection(STATE_SRC_BINDINGS));
		json_add_snapshot(all, "rules",
		    state_snapshot_collection(STATE_SRC_RULES));
		json_add_snapshot(all, "styles",
		    state_snapshot_collection(STATE_SRC_STYLES));
		json_add_snapshot(all, "aliases",
		    state_snapshot_collection(STATE_SRC_ALIASES));
		cow_ipc_respond_data(cmdq->client_fd, all);
	}
	return (CMD_RETURN_NORMAL);
}

static void
json_add_config_command(const char *command, void *data)
{
	cJSON_AddItemToArray(data, cJSON_CreateString(command));
}

static void
json_add_snapshot(cJSON *object, const char *name, struct state_value *snapshot)
{
	cJSON *json = state_value_to_json(snapshot);
	state_value_unref(snapshot);
	cJSON_AddItemToObject(object, name, json);
}

static cJSON *
show_config_object(struct cmd_q *cmdq)
{
	cJSON *config = cJSON_CreateObject();
	for (const char **key = show_config_keys; *key != NULL; key++)
		cJSON_AddItemToObject(config, *key,
		    show_config_value(*key, cmdq));
	return (config);
}

static cJSON *
show_config_value(const char *ckey, struct cmd_q *cmdq)
{
	cJSON *val = NULL;

	if (strcmp(ckey, "border.width") == 0)
		val = cJSON_CreateNumber(cow_config->border);
	else if (strcmp(ckey, "step") == 0)
		val = cJSON_CreateNumber(cow_config->step);
	else if (strcmp(ckey, "border.corner_length") == 0)
		val = cJSON_CreateNumber(cow_config->corner_length);
	else if (strcmp(ckey, "reserved.top") == 0)
		val = cJSON_CreateNumber(cow_config->reserved.top);
	else if (strcmp(ckey, "reserved.bottom") == 0)
		val = cJSON_CreateNumber(cow_config->reserved.bottom);
	else if (strcmp(ckey, "reserved.left") == 0)
		val = cJSON_CreateNumber(cow_config->reserved.left);
	else if (strcmp(ckey, "reserved.right") == 0)
		val = cJSON_CreateNumber(cow_config->reserved.right);
	else if (strcmp(ckey, "output.colour.background") == 0) {
		if (cow_config->has_output_bg_colour) {
			char colour[16];
			cow_colour_format(colour, sizeof(colour),
			    cow_config->output_bg_colour);
			val = cJSON_CreateString(colour);
		} else
			val = cJSON_CreateNull();
	} else if (strcmp(ckey, "output.image.background") == 0) {
		if (cow_config->has_output_bg_image)
			val = cJSON_CreateString(cow_config->output_bg_image != NULL ?
			    cow_config->output_bg_image : "none");
		else
			val = cJSON_CreateNull();
	} else if (strncmp(ckey, "output.colour.background.", 25) == 0) {
		float colour[4];
		if (cow_config_get_output_background(cow_config, ckey + 25,
			colour)) {
			char text[16];
			cow_colour_format(text, sizeof(text), colour);
			val = cJSON_CreateString(text);
		}
	} else if (strncmp(ckey, "output.image.background.", 24) == 0) {
		const char *image;
		if (cow_config_get_output_background_image(cow_config, ckey + 24,
			&image))
			val = cJSON_CreateString(image != NULL ? image : "none");
	}
	else if (strcmp(ckey, "snap_distance") == 0)
		val = cJSON_CreateNumber(cow_config->snap_distance);
	else if (strcmp(ckey, "border.style") == 0)
		val = cJSON_CreateString(
		    cow_config->border_style == COW_BORDER_STYLE_FVWM ? "fvwm" :
			cow_config->border_style == COW_BORDER_STYLE_MWM ?
									"mwm" :
									"none");
	else if (strcmp(ckey, "desktop-configuration") == 0)
		val = cJSON_CreateString(cow_config->desktop_config ==
		    COW_DESKTOP_PER_OUTPUT ? "per-output" :
		    cow_config->desktop_config == COW_DESKTOP_SHARED ? "shared" :
		    "global");
	else if (strcmp(ckey, "desktop_size") == 0) {
		val = cJSON_CreateArray();
		cJSON_AddItemToArray(val, cJSON_CreateNumber(cow_config->page_cols));
		cJSON_AddItemToArray(val, cJSON_CreateNumber(cow_config->page_rows));
	} else if (strcmp(ckey, "focus") == 0)
		val = cJSON_CreateString(
		    cow_config->focus_mode == COW_FOCUS_MODE_SLOPPY ? "sloppy" :
								      "click");
	else if (strcmp(ckey, "geometry_window") == 0) {
		const char *mode = "none";
		if (cow_config->geometry_window == COW_GEOMETRY_WINDOW_BOTH)
			mode = "both";
		else if (cow_config->geometry_window ==
		    COW_GEOMETRY_WINDOW_MOVE)
			mode = "move";
		else if (cow_config->geometry_window ==
		    COW_GEOMETRY_WINDOW_RESIZE)
			mode = "resize";
		val = cJSON_CreateString(mode);
	} else if (strcmp(ckey, "clamp_mode") == 0) {
		const char *mode = "output";
		if (cow_config->clamp_mode == COW_CLAMP_NONE)
			mode = "none";
		else if (cow_config->clamp_mode == COW_CLAMP_TO_ALL_OUTPUTS)
			mode = "all_outputs";
		val = cJSON_CreateString(mode);
	} else if (strcmp(ckey, "page_visibility") == 0)
		val = cJSON_CreateString(cow_config->page_visibility ==
		    COW_PAGE_VISIBILITY_OVERLAP ? "overlap" : "origin");
	else if (strcmp(ckey, "placement") == 0)
		val = cJSON_CreateString(cow_config->placement ==
		    COW_PLACEMENT_UNDERMOUSE ? "undermouse" :
		    cow_config->placement == COW_PLACEMENT_CENTRE ? "centre" :
		    cow_config->placement == COW_PLACEMENT_EMPTY ? "empty" :
		    "cascade");
	else if (strcmp(ckey, "page_scroll_animate") == 0)
		val = cJSON_CreateBool(cow_config->page_scroll_animate);
	else if (strcmp(ckey, "page_scroll_ms") == 0)
		val = cJSON_CreateNumber(cow_config->page_scroll_ms);
	else if (strcmp(ckey, "edge_scroll") == 0)
		val = cJSON_CreateBool(cow_config->edge_scroll);
	else if (strcmp(ckey, "edge_scroll_delay_ms") == 0)
		val = cJSON_CreateNumber(cow_config->edge_scroll_delay_ms);
	else if (strcmp(ckey, "edge_scroll_modifiers") == 0) {
		char modifiers[16];
		size_t n = 0;
		uint32_t mask = cow_config->edge_scroll_modifiers;
		if ((mask & RIVER_SEAT_V1_MODIFIERS_MOD4) != 0)
			modifiers[n++] = 'L';
		if ((mask & RIVER_SEAT_V1_MODIFIERS_SHIFT) != 0)
			modifiers[n++] = 'S';
		if ((mask & RIVER_SEAT_V1_MODIFIERS_MOD1) != 0)
			modifiers[n++] = 'A';
		if ((mask & RIVER_SEAT_V1_MODIFIERS_CTRL) != 0)
			modifiers[n++] = 'C';
		if ((mask & RIVER_SEAT_V1_MODIFIERS_MOD5) != 0)
			modifiers[n++] = '5';
		if (n == 0)
			modifiers[n++] = '0';
		modifiers[n] = '\0';
		val = cJSON_CreateString(modifiers);
	} else if (strcmp(ckey, "titlebar.font") == 0)
		val = cJSON_CreateString(
		    cow_config->font_name ? cow_config->font_name : "");
	else if (strcmp(ckey, "title_format") == 0)
		val = cJSON_CreateString(
		    cow_config->title_format ? cow_config->title_format : "");
	else if (strcmp(ckey, "cursor.theme") == 0)
		val = cJSON_CreateString(cow_config->cursor_theme != NULL ?
		    cow_config->cursor_theme : "");
	else if (strcmp(ckey, "cursor.size") == 0)
		val = cJSON_CreateNumber(cow_config->cursor_size);
	else if (strcmp(ckey, "cursor.hide_timeout") == 0)
		val = cJSON_CreateNumber(cow_config->cursor_hide_timeout);
	else if (strcmp(ckey, "repeat_delay_ms") == 0)
		val = cJSON_CreateNumber(cow_config->repeat_delay_ms);
	else if (strcmp(ckey, "repeat_interval_ms") == 0)
		val = cJSON_CreateNumber(cow_config->repeat_interval_ms);
	else if (strcmp(ckey, "menu.repeat_keys") == 0)
		val = cJSON_CreateString(cow_config->menu_repeat_keys != NULL ?
		    cow_config->menu_repeat_keys : "");
	else if (strcmp(ckey, "raise_on_focus") == 0)
		val = cJSON_CreateBool(cow_config->raise_on_focus);
	else if (strcmp(ckey, "keyboard_layout.per_window") == 0)
		val = cJSON_CreateBool(
		    cow_config->keyboard_layout_per_window);
	else if (strcmp(ckey, "preserve_desk_on_output_loss") == 0)
		val = cJSON_CreateBool(cow_config->preserve_desk_on_output_loss);
	else if (strcmp(ckey, "default_layer") == 0)
		val = cJSON_CreateNumber(cow_config->default_layer);
	else if (strcmp(ckey, "max_layer") == 0)
		val = cJSON_CreateNumber(cow_config->max_layer);
	else if (strcmp(ckey, "desks.count") == 0) {
		struct cow_output *out = cmd_ctx_target_output(cmdq);
		if (out == NULL && !wl_list_empty(&cow_wm.outputs))
			out = wl_container_of(cow_wm.outputs.next, out, wm_link);
		val = cJSON_CreateNumber(out != NULL ? cow_desk_count(out) :
		    cow_config->desk_count);
	}
	return (val);
}
