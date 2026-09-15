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

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include <cJSON.h>

#include "cow/compat.h"
#include "cow/memory.h"
#include "cow/menu.h"
#include "cow/util.h"

#include "config-serialize.h"
#include "state-value.h"

struct config_buf {
	char   *data;
	size_t  len;
	size_t  cap;
};

static void
buf_reserve(struct config_buf *buf, size_t extra)
{
	if (buf->len + extra + 1 <= buf->cap)
		return;
	size_t cap = buf->cap != 0 ? buf->cap : 256;
	while (buf->len + extra + 1 > cap)
		cap *= 2;
	buf->data = xrealloc(buf->data, cap, 1);
	buf->cap = cap;
}

static void
buf_appendn(struct config_buf *buf, const char *text, size_t len)
{
	buf_reserve(buf, len);
	memcpy(buf->data + buf->len, text, len);
	buf->len += len;
	buf->data[buf->len] = '\0';
}

static void
buf_append(struct config_buf *buf, const char *text)
{
	buf_appendn(buf, text, strlen(text));
}

static void
buf_command(struct config_buf *buf, size_t argc, const char *const *argv)
{
	char *line = cow_command_from_argv(argc, argv);
	if (line == NULL)
		return;
	buf_append(buf, line);
	buf_append(buf, "\n");
	free(line);
}

static char *
buf_take(struct config_buf *buf)
{
	if (buf->data == NULL)
		return (xstrdup(""));
	char *result = buf->data;
	buf->data = NULL;
	buf->len = buf->cap = 0;
	return (result);
}

static const struct state_value *
field(const struct state_value *record, const char *name,
    enum state_value_type type)
{
	const struct state_value *value = state_record_get(record, name);
	return (value != NULL && value->type == type ? value : NULL);
}

static const char *
string_field(const struct state_value *record, const char *name)
{
	const struct state_value *value = field(record, name, STATE_V_STRING);
	return (value != NULL ? value->u.s : "");
}

static bool
bool_field(const struct state_value *record, const char *name)
{
	const struct state_value *value = field(record, name, STATE_V_BOOL);
	return (value != NULL && value->u.b);
}

static void
serialize_setting(struct config_buf *buf, const char *key,
    const cJSON *value)
{
	/* These values are represented by the active decoration profile. */
	static const char *decor_only[] = {
		"border.width",
		"border.corner_length",
		"border.style",
		"titlebar.font",
		"title_format",
		NULL,
	};
	for (const char **name = decor_only; *name != NULL; name++) {
		if (strcmp(key, *name) == 0)
			return;
	}

	if (strcmp(key, "desktop_size") == 0 && cJSON_IsArray(value) &&
	    cJSON_GetArraySize(value) == 2) {
		const cJSON *cols = cJSON_GetArrayItem(value, 0);
		const cJSON *rows = cJSON_GetArrayItem(value, 1);
		if (cJSON_IsNumber(cols) && cJSON_IsNumber(rows)) {
			char col_text[32], row_text[32];
			snprintf(col_text, sizeof(col_text), "%d", cols->valueint);
			snprintf(row_text, sizeof(row_text), "%d", rows->valueint);
			const char *argv[] = { "set", "desktop_size", col_text,
			    row_text };
			buf_command(buf, 4, argv);
		}
		return;
	}

	char number[64];
	const char *text;

	if (cJSON_IsString(value))
		text = value->valuestring != NULL ? value->valuestring : "";
	else if (cJSON_IsBool(value))
		text = cJSON_IsTrue(value) ? "true" : "false";
	else if (cJSON_IsNumber(value)) {
		if (value->valuedouble == (double)value->valueint)
			snprintf(number, sizeof(number), "%d", value->valueint);
		else
			snprintf(number, sizeof(number), "%.17g",
			    value->valuedouble);
		text = number;
	} else
		return;

	const char *argv[] = { "set", key, text };
	buf_command(buf, 3, argv);
}

static void
serialize_settings(struct config_buf *buf, const cJSON *settings)
{
	const cJSON *entry;
	cJSON_ArrayForEach(entry, settings)
		serialize_setting(buf, entry->string, entry);
}

char *
cow_config_serialize_settings(const cJSON *settings)
{
	struct config_buf buf = { 0 };
	serialize_settings(&buf, settings);
	return (buf_take(&buf));
}

static void
serialize_rule(struct config_buf *buf, const struct state_value *record)
{
	const char *name = string_field(record, "name");
	const char *type = string_field(record, "type");
	const char *match = string_field(record, "match");
	const char *body = string_field(record, "body");
	const struct state_value *desk = state_record_get(record, "desk");
	char desk_text[64] = "";
	char *target = NULL;

	if (desk != NULL && desk->type == STATE_V_STRING)
		strlcpy(desk_text, desk->u.s, sizeof(desk_text));
	else if (desk != NULL && desk->type == STATE_V_NUMBER && desk->u.n >= 0)
		snprintf(desk_text, sizeof(desk_text), "%d", (int)desk->u.n);

	const char *argv[10];
	size_t argc = 0;
	argv[argc++] = "rule";
	if (bool_field(record, "global")) {
		argv[argc++] = "-g";
	} else {
		const char *output = string_field(record, "output");
		if (desk_text[0] != '\0')
			xasprintf(&target, "%s:%s", output, desk_text);
		else if (output[0] != '\0')
			target = xstrdup(output);
		else
			target = xstrdup(":");
		argv[argc++] = "-t";
		argv[argc++] = target;
	}
	argv[argc++] = "-n";
	argv[argc++] = name;
	argv[argc++] = "-T";
	argv[argc++] = type;
	if (match[0] != '\0') {
		argv[argc++] = "-s";
		argv[argc++] = match;
	}

	char *prefix = cow_command_from_argv(argc, argv);
	if (prefix != NULL) {
		buf_append(buf, prefix);
		if (bool_field(record, "body_is_single")) {
			buf_append(buf, " ");
			buf_append(buf, body);
			buf_append(buf, "\n");
		} else {
			buf_append(buf, " script {\n");
			buf_append(buf, body);
			if (body[0] != '\0' && body[strlen(body) - 1] != '\n')
				buf_append(buf, "\n");
			buf_append(buf, "}\n");
		}
		free(prefix);
	}
	xfree(target);
}

static void
serialize_rules(struct config_buf *buf, const struct state_value *value)
{
	if (value->type == STATE_V_RECORD) {
		serialize_rule(buf, value);
		return;
	}
	if (value->type != STATE_V_LIST)
		return;
	for (size_t i = 0; i < value->u.list.n; i++)
		serialize_rule(buf, value->u.list.items[i]);
}

static void
serialize_style(struct config_buf *buf, const struct state_value *record)
{
	const char *name = string_field(record, "name");
	const char *match = string_field(record, "match");
	const char *body = string_field(record, "body");
	const struct state_value *desk = state_record_get(record, "desk");
	char desk_text[64] = "";
	char *target = NULL;

	if (desk != NULL && desk->type == STATE_V_STRING)
		strlcpy(desk_text, desk->u.s, sizeof(desk_text));
	else if (desk != NULL && desk->type == STATE_V_NUMBER && desk->u.n >= 0)
		snprintf(desk_text, sizeof(desk_text), "%d", (int)desk->u.n);

	const char *argv[8];
	size_t argc = 0;
	argv[argc++] = "style";
	if (bool_field(record, "global")) {
		argv[argc++] = "-g";
	} else {
		const char *output = string_field(record, "output");
		if (desk_text[0] != '\0')
			xasprintf(&target, "%s:%s", output, desk_text);
		else if (output[0] != '\0')
			target = xstrdup(output);
		else
			target = xstrdup(":");
		argv[argc++] = "-t";
		argv[argc++] = target;
	}
	argv[argc++] = "-n";
	argv[argc++] = name;
	if (match[0] != '\0') {
		argv[argc++] = "-s";
		argv[argc++] = match;
	}
	char *prefix = cow_command_from_argv(argc, argv);
	if (prefix != NULL) {
		buf_append(buf, prefix);
		buf_append(buf, " {\n");
		buf_append(buf, body);
		if (body[0] != '\0' && body[strlen(body) - 1] != '\n')
			buf_append(buf, "\n");
		buf_append(buf, "}\n");
		free(prefix);
	}
	xfree(target);
}

static void
serialize_styles(struct config_buf *buf, const struct state_value *value)
{
	if (value->type == STATE_V_RECORD) {
		serialize_style(buf, value);
		return;
	}
	if (value->type != STATE_V_LIST)
		return;
	for (size_t i = 0; i < value->u.list.n; i++)
		serialize_style(buf, value->u.list.items[i]);
}

static void
serialize_decor_profile(struct config_buf *buf,
    const struct state_value *record, const char **active)
{
	const char *name = string_field(record, "name");
	const struct state_value *settings = field(record, "settings",
	    STATE_V_RECORD);
	if (settings == NULL)
		return;
	for (size_t i = 0; i < settings->u.record.n; i++) {
		const struct state_record_entry *entry =
		    &settings->u.record.entries[i];
		if (entry->val->type != STATE_V_STRING)
			continue;
		const char *argv[] = { "decor", "-d", name, entry->key,
		    entry->val->u.s };
		buf_command(buf, 5, argv);
	}
	if (bool_field(record, "active"))
		*active = name;
}

static void
serialize_decor(struct config_buf *buf, const struct state_value *value)
{
	const char *active = NULL;
	if (value->type == STATE_V_RECORD)
		serialize_decor_profile(buf, value, &active);
	else if (value->type == STATE_V_LIST) {
		for (size_t i = 0; i < value->u.list.n; i++)
			serialize_decor_profile(buf, value->u.list.items[i],
			    &active);
	}
	if (active != NULL) {
		const char *argv[] = { "decor", "-a", active };
		buf_command(buf, 3, argv);
	}
}

static void
serialize_menu_style(struct config_buf *buf, const char *name,
	const struct state_value *style, uint32_t overrides)
{
	static const struct {
		const char *snapshot;
		const char *config;
		uint32_t field;
	} fields[] = {
		{ "bg", "bg", COW_MENU_STYLE_BG },
		{ "fg", "fg", COW_MENU_STYLE_FG },
		{ "title_bg", "title.bg", COW_MENU_STYLE_TITLE_BG },
		{ "title_fg", "title.fg", COW_MENU_STYLE_TITLE_FG },
		{ "separator", "separator", COW_MENU_STYLE_SEPARATOR },
		{ "border", "border.colour", COW_MENU_STYLE_BORDER_COLOUR },
		{ "border_width", "border.width", COW_MENU_STYLE_BORDER_WIDTH },
		{ "border_relief", "border.relief", COW_MENU_STYLE_BORDER_RELIEF },
		{ "padding_x", "padding.x", COW_MENU_STYLE_PADDING_X },
		{ "padding_y", "padding.y", COW_MENU_STYLE_PADDING_Y },
		{ "icon_size", "icon.size", COW_MENU_STYLE_ICON_SIZE },
		{ "icon_gap", "icon.gap", COW_MENU_STYLE_ICON_GAP },
		{ "icon_path", "icon.path", COW_MENU_STYLE_ICON_PATH },
		{ "side_width", "side.width", COW_MENU_STYLE_SIDE_WIDTH },
		{ "side_colour", "side.colour", COW_MENU_STYLE_SIDE_COLOUR },
		{ "side_image", "side.image", COW_MENU_STYLE_SIDE_IMAGE },
		{ "font", "font", COW_MENU_STYLE_FONT },
		{ "title_underlines", "title.underlines",
		    COW_MENU_STYLE_TITLE_UNDERLINES },
		{ "automatic_hotkeys", "automatic.hotkeys",
		    COW_MENU_STYLE_AUTOMATIC_HOTKEYS },
		{ "selection_relief", "selection.relief",
		    COW_MENU_STYLE_SELECTION_RELIEF },
	};

	for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
		if (overrides != UINT32_MAX &&
		    (overrides & fields[i].field) == 0)
			continue;
		const struct state_value *value = state_record_get(style,
		    fields[i].snapshot);
		char *text = state_value_to_argv_string(value);
		if (text == NULL)
			continue;
		if (strcmp(fields[i].config, "font") == 0 && text[0] == '\0') {
			xfree(text);
			continue;
		}
		if (strcmp(fields[i].config, "side.image") == 0 &&
		    text[0] == '\0') {
			xfree(text);
			text = xstrdup("none");
		}
		const char *named[] = { "menu-style", "-m", name,
		    fields[i].config, text };
		const char *global[] = { "menu-style", fields[i].config, text };
		buf_command(buf, name != NULL ? 5 : 3,
		    name != NULL ? named : global);
		xfree(text);
	}

	static const struct {
		const char *snapshot;
		const char *config;
		uint32_t field;
	} gradients[] = {
		{ "bg_gradient", "bg.gradient", COW_MENU_STYLE_BG_GRADIENT },
		{ "title_bg_gradient", "title.bg.gradient",
		    COW_MENU_STYLE_TITLE_BG_GRADIENT },
	};
	for (size_t i = 0; i < sizeof(gradients) / sizeof(gradients[0]); i++) {
		if (overrides != UINT32_MAX &&
		    (overrides & gradients[i].field) == 0)
			continue;
		const struct state_value *gradient = field(style,
		    gradients[i].snapshot, STATE_V_RECORD);
		if (gradient == NULL)
			continue;
		const char *mode = string_field(gradient, "mode");
		const char *from = string_field(gradient, "from");
		const char *to = string_field(gradient, "to");
		const char *named_none[] = { "menu-style", "-m", name,
		    gradients[i].config, "none" };
		const char *named_gradient[] = { "menu-style", "-m", name,
		    gradients[i].config,
		    strcmp(mode, "horizontal") == 0 ? "HGradient" :
		    "VGradient", from, to };
		const char *global_none[] = { "menu-style",
		    gradients[i].config, "none" };
		const char *global_gradient[] = { "menu-style",
		    gradients[i].config,
		    strcmp(mode, "horizontal") == 0 ? "HGradient" :
		    "VGradient", from, to };
		if (strcmp(mode, "none") == 0)
			buf_command(buf, name != NULL ? 5 : 3,
			    name != NULL ? named_none : global_none);
		else
			buf_command(buf, name != NULL ? 7 : 5,
			    name != NULL ? named_gradient : global_gradient);
	}
}

static void
serialize_menu(struct config_buf *buf, const struct state_value *record)
{
	const char *name = string_field(record, "name");
	bool typeahead = bool_field(record, "typeahead");
	const struct state_value *style = field(record, "style",
	    STATE_V_RECORD);
	const struct state_value *override_value = state_record_get(record,
	    "style_overrides");
	uint32_t overrides = override_value != NULL &&
	    override_value->type == STATE_V_NUMBER ?
	    (uint32_t)override_value->u.n : UINT32_MAX;
	if (style != NULL)
		serialize_menu_style(buf, name, style, overrides);

	const struct state_value *items = field(record, "items", STATE_V_LIST);
	if (items == NULL)
		return;
	for (size_t i = 0; i < items->u.list.n; i++) {
		const struct state_value *item = items->u.list.items[i];
		const char *type = string_field(item, "type");
		const char *label = string_field(item, "label");
		const char *icon = string_field(item, "icon");
		const char *action = string_field(item, "action");
		char *popup_action = NULL;
		if (bool_field(item, "popup"))
			xasprintf(&popup_action, "%s %s",
			    bool_field(item, "popup_pick_context") ?
			    "popup-pick" : "popup", action);

		const char *argv[7];
		size_t argc = 0;
		argv[argc++] = "menu-add";
		if (typeahead) {
			argv[argc++] = "-T";
			typeahead = false;
		}
		if (icon[0] != '\0') {
			argv[argc++] = "-i";
			argv[argc++] = icon;
		}
		argv[argc++] = name;
		argv[argc++] = strcmp(type, "separator") == 0 ? "-" : label;
		argv[argc++] = strcmp(type, "separator") == 0 ? "-" :
		    strcmp(type, "title") == 0 ? "title" :
		    popup_action != NULL ? popup_action : action;
		buf_command(buf, argc, argv);
		xfree(popup_action);
	}
}

static void
serialize_menus(struct config_buf *buf, const struct state_value *value)
{
	if (value->type == STATE_V_RECORD) {
		serialize_menu(buf, value);
		return;
	}
	if (value->type != STATE_V_LIST)
		return;
	for (size_t i = 0; i < value->u.list.n; i++)
		serialize_menu(buf, value->u.list.items[i]);
}

static void
serialize_binding(struct config_buf *buf, const struct state_value *record)
{
	const char *type = string_field(record, "type");
	const char *trigger = string_field(record, "trigger");
	const char *action = string_field(record, "action");

	if (strcmp(type, "keyboard") == 0) {
		const char *release = string_field(record, "release_action");
		const char *argv[7];
		size_t argc = 0;
		argv[argc++] = "bind";
		if (bool_field(record, "repeatable"))
			argv[argc++] = "-r";
		if (release[0] != '\0') {
			argv[argc++] = "-x";
			argv[argc++] = release;
		}
		argv[argc++] = trigger;
		argv[argc++] = action;
		buf_command(buf, argc, argv);
	} else if (strcmp(type, "mouse") == 0) {
		const char *argv[] = { "mouse", trigger, action };
		buf_command(buf, 3, argv);
	} else if (strcmp(type, "decoration") == 0) {
		const char *region = string_field(record, "region");
		const struct state_value *clicks = state_record_get(record,
		    "clicks");
		const char *argv_single[] = { "bind-decoration", region,
		    action };
		const char *argv_double[] = { "bind-decoration", "-2", region,
		    action };
		buf_command(buf, clicks != NULL && clicks->type == STATE_V_NUMBER &&
		    clicks->u.n == 2 ? 4 : 3,
		    clicks != NULL && clicks->type == STATE_V_NUMBER &&
		    clicks->u.n == 2 ? argv_double : argv_single);
	}
}

static void
serialize_bindings(struct config_buf *buf, const struct state_value *value)
{
	if (value->type != STATE_V_LIST)
		return;
	for (size_t i = 0; i < value->u.list.n; i++)
		serialize_binding(buf, value->u.list.items[i]);
}

static void
serialize_alias(struct config_buf *buf, const struct state_value *record)
{
	const char *argv[] = {
		"alias",
		string_field(record, "name"),
		string_field(record, "command"),
	};
	buf_command(buf, 3, argv);
}

static void
serialize_aliases(struct config_buf *buf, const struct state_value *value)
{
	if (value->type == STATE_V_RECORD) {
		serialize_alias(buf, value);
		return;
	}
	if (value->type != STATE_V_LIST)
		return;
	for (size_t i = 0; i < value->u.list.n; i++)
		serialize_alias(buf, value->u.list.items[i]);
}

static bool
serialize_topic(struct config_buf *buf, const char *topic,
	const struct state_value *value)
{
	if (strcmp(topic, "rule") == 0)
		serialize_rules(buf, value);
	else if (strcmp(topic, "style") == 0)
		serialize_styles(buf, value);
	else if (strcmp(topic, "decor") == 0)
		serialize_decor(buf, value);
	else if (strcmp(topic, "menu") == 0)
		serialize_menus(buf, value);
	else if (strcmp(topic, "binding") == 0)
		serialize_bindings(buf, value);
	else if (strcmp(topic, "alias") == 0)
		serialize_aliases(buf, value);
	else
		return (false);
	return (true);
}

char *
cow_config_serialize_topic(const char *topic, const struct state_value *value)
{
	struct config_buf buf = { 0 };
	if (!serialize_topic(&buf, topic, value))
		return (NULL);
	return (buf_take(&buf));
}
