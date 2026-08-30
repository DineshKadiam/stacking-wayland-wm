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
#include <stdlib.h>
#include <string.h>

#include <cJSON.h>

#include "cow/menu.h"

#include "config-serialize.h"
#include "state-value.h"

static int failures;

static void
record_string(struct state_value *record, const char *key, const char *text)
{
	struct state_value *value = state_value_string(text);
	state_record_set(record, key, value);
	state_value_unref(value);
}

static void
record_bool(struct state_value *record, const char *key, bool enabled)
{
	struct state_value *value = state_value_bool(enabled);
	state_record_set(record, key, value);
	state_value_unref(value);
}

static void
record_number(struct state_value *record, const char *key, double number)
{
	struct state_value *value = state_value_number(number);
	state_record_set(record, key, value);
	state_value_unref(value);
}

static void
expect(const char *name, char *actual, const char *expected)
{
	if (actual == NULL || strcmp(actual, expected) != 0) {
		fprintf(stderr, "%s:\nexpected:\n%sactual:\n%s", name,
		    expected, actual != NULL ? actual : "(null)\n");
		failures++;
	}
	free(actual);
}

static void
test_settings(void)
{
	cJSON *settings = cJSON_CreateObject();
	cJSON_AddNumberToObject(settings, "border.width", 4);
	cJSON_AddStringToObject(settings, "titlebar.font", "Iosevka 12");
	cJSON_AddStringToObject(settings, "output.colour.background",
	    "0x191970FF");
	cJSON_AddStringToObject(settings, "output.image.background.eDP-1",
	    "none");
	cJSON_AddBoolToObject(settings, "raise_on_focus", true);
	expect("settings", cow_config_serialize_settings(settings),
	    "set output.colour.background 0x191970FF\n"
	    "set output.image.background.eDP-1 none\n"
	    "set raise_on_focus true\n");
	cJSON_Delete(settings);
}

static void
test_rules(void)
{
	struct state_value *rules = state_value_list();
	struct state_value *rule = state_value_record();
	record_string(rule, "name", "terminal placement");
	record_string(rule, "type", "on-map");
	record_bool(rule, "global", false);
	record_string(rule, "output", "HDMI-A-1");
	record_number(rule, "desk", 2);
	record_string(rule, "match", "/foot/");
	record_string(rule, "body", "window-move -x 10 ; winops -L 4");
	record_bool(rule, "body_is_single", false);
	state_list_append(rules, rule);
	state_value_unref(rule);
	expect("rules", cow_config_serialize_topic("rule", rules),
	    "rule -t HDMI-A-1:2 -n \"terminal placement\" -T on-map -s "
	    "/foot/ script {\n"
	    "window-move -x 10 ; winops -L 4\n"
	    "}\n");
	state_value_unref(rules);

	rule = state_value_record();
	record_string(rule, "name", "dynamic bindings");
	record_string(rule, "type", "on-outputs-changed");
	record_bool(rule, "global", true);
	record_string(rule, "match", "");
	record_string(rule, "body",
	    "script {\n    bind \"L+#{d.nr}\" desk -d #{d.nr}\n}");
	record_bool(rule, "body_is_single", true);
	expect("script rule", cow_config_serialize_topic("rule", rule),
	    "rule -g -n \"dynamic bindings\" -T on-outputs-changed "
	    "script {\n    bind \"L+#{d.nr}\" desk -d #{d.nr}\n}\n");
	state_value_unref(rule);
}

static void
test_styles(void)
{
	struct state_value *style = state_value_record();
	record_string(style, "name", "terminal style");
	record_bool(style, "global", true);
	record_string(style, "output", "");
	record_number(style, "desk", -1);
	record_string(style, "match", "%foot");
	record_string(style, "body",
	    "\n    decor {\n        border.width 4\n    }\n"
	    "    layer top\n");
	expect("style", cow_config_serialize_topic("style", style),
	    "style -g -n \"terminal style\" -s %foot {\n"
	    "\n    decor {\n        border.width 4\n    }\n"
	    "    layer top\n}\n");
	state_value_unref(style);
}

static void
test_menu(void)
{
	struct state_value *menu = state_value_record();
	struct state_value *style = state_value_record();
	struct state_value *items = state_value_list();
	struct state_value *item = state_value_record();

	record_string(menu, "name", "Root Menu");
	record_number(menu, "style_overrides",
	    COW_MENU_STYLE_BG | COW_MENU_STYLE_PADDING_X);
	record_string(style, "bg", "0x112233ff");
	record_number(style, "padding_x", 8);
	record_string(style, "font", "");
	state_record_set(menu, "style", style);
	state_value_unref(style);

	record_string(item, "type", "normal");
	record_string(item, "label", "Terminal");
	record_string(item, "action", "exec foot");
	record_string(item, "icon", "/tmp/terminal.svg");
	record_bool(item, "popup", false);
	record_bool(item, "popup_pick_context", false);
	state_list_append(items, item);
	state_value_unref(item);
	state_record_set(menu, "items", items);
	state_value_unref(items);

	expect("menu", cow_config_serialize_topic("menu", menu),
	    "menu-style -m \"Root Menu\" bg 0x112233ff\n"
	    "menu-style -m \"Root Menu\" padding.x 8\n"
	    "menu-add -i /tmp/terminal.svg \"Root Menu\" Terminal "
	    "\"exec foot\"\n");
	state_value_unref(menu);
}

int
main(void)
{
	test_settings();
	test_rules();
	test_styles();
	test_menu();
	return (failures != 0);
}
