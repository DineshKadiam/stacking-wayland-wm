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
#include <string.h>

#include "cow/compat.h"
#include "cow/keyboard_layout.h"
#include "cow/log.h"
#include "cow/memory.h"
#include "cow/status.h"
#include "cow/wm.h"

#include "river-xkb-config-v1-client-protocol.h"

static struct cow_keyboard_layout *active_layout;

static void
keyboard_removed(void *data, struct river_xkb_keyboard_v1 *river_keyboard)
{
	struct cow_keyboard_layout *keyboard = data;
	bool was_active = keyboard == active_layout;

	wl_list_remove(&keyboard->link);
	river_xkb_keyboard_v1_destroy(river_keyboard);
	xfree(keyboard->name);
	xfree(keyboard);

	if (was_active) {
		active_layout = NULL;
		if (!wl_list_empty(&cow_wm.keyboard_layouts))
			active_layout = wl_container_of(
			    cow_wm.keyboard_layouts.next, active_layout, link);
		cow_status_request();
	}
}

static void
keyboard_input_device(__unused void *data,
    __unused struct river_xkb_keyboard_v1 *river_keyboard,
    __unused void *device)
{
}

static void
keyboard_layout(void *data,
    __unused struct river_xkb_keyboard_v1 *river_keyboard, uint32_t index,
    const char *name)
{
	struct cow_keyboard_layout *keyboard = data;
	bool changed = !keyboard->has_layout || keyboard->index != index ||
	    (keyboard->name == NULL) != (name == NULL) ||
	    (keyboard->name != NULL && name != NULL &&
	    strcmp(keyboard->name, name) != 0);

	keyboard->index = index;
	keyboard->has_layout = true;
	if (changed) {
		xstrset(&keyboard->name, name);
		active_layout = keyboard;
		cow_status_request();
	}
}

static void
keyboard_capslock_enabled(__unused void *data,
    __unused struct river_xkb_keyboard_v1 *river_keyboard)
{
}

static void
keyboard_capslock_disabled(__unused void *data,
    __unused struct river_xkb_keyboard_v1 *river_keyboard)
{
}

static void
keyboard_numlock_enabled(__unused void *data,
    __unused struct river_xkb_keyboard_v1 *river_keyboard)
{
}

static void
keyboard_numlock_disabled(__unused void *data,
    __unused struct river_xkb_keyboard_v1 *river_keyboard)
{
}

static void
keyboard_done(__unused void *data,
    __unused struct river_xkb_keyboard_v1 *river_keyboard)
{
}

static const struct river_xkb_keyboard_v1_listener keyboard_listener = {
	.removed = keyboard_removed,
	.input_device = keyboard_input_device,
	.layout = keyboard_layout,
	.capslock_enabled = keyboard_capslock_enabled,
	.capslock_disabled = keyboard_capslock_disabled,
	.numlock_enabled = keyboard_numlock_enabled,
	.numlock_disabled = keyboard_numlock_disabled,
	.done = keyboard_done,
};

static void
config_finished(__unused void *data,
    struct river_xkb_config_v1 *xkb_config)
{
	river_xkb_config_v1_destroy(xkb_config);
	if (cow_wm.xkb_config == xkb_config)
		cow_wm.xkb_config = NULL;
}

static void
config_keyboard(__unused void *data,
    __unused struct river_xkb_config_v1 *xkb_config,
    struct river_xkb_keyboard_v1 *river_keyboard)
{
	struct cow_keyboard_layout *keyboard = xcalloc(1, sizeof *keyboard);

	keyboard->river_keyboard = river_keyboard;
	wl_list_insert(cow_wm.keyboard_layouts.prev, &keyboard->link);
	river_xkb_keyboard_v1_add_listener(river_keyboard, &keyboard_listener,
	    keyboard);
}

static const struct river_xkb_config_v1_listener config_listener = {
	.finished = config_finished,
	.xkb_keyboard = config_keyboard,
};

void
cow_keyboard_layout_init(void)
{
	wl_list_init(&cow_wm.keyboard_layouts);
	active_layout = NULL;
}

void
cow_keyboard_layout_bind(struct wl_registry *registry, uint32_t name,
    uint32_t version)
{
	uint32_t bind_version = version < 2 ? version : 2;

	cow_wm.xkb_config = wl_registry_bind(registry, name,
	    &river_xkb_config_v1_interface, bind_version);
	river_xkb_config_v1_add_listener(cow_wm.xkb_config, &config_listener,
	    NULL);
}

void
cow_keyboard_layout_fini(void)
{
	struct cow_keyboard_layout *keyboard, *tmp;
	wl_list_for_each_safe(keyboard, tmp, &cow_wm.keyboard_layouts, link)
	{
		wl_list_remove(&keyboard->link);
		river_xkb_keyboard_v1_destroy(keyboard->river_keyboard);
		xfree(keyboard->name);
		xfree(keyboard);
	}
	active_layout = NULL;

	if (cow_wm.xkb_config != NULL)
		river_xkb_config_v1_stop(cow_wm.xkb_config);
}

void
cow_keyboard_layout_add_status(cJSON *root)
{
	if (active_layout != NULL && active_layout->has_layout) {
		if (active_layout->name != NULL)
			cJSON_AddStringToObject(root, "language",
			    active_layout->name);
		else
			cJSON_AddNullToObject(root, "language");
	} else {
		cJSON_AddNullToObject(root, "language");
	}

	cJSON *layouts = cJSON_AddArrayToObject(root, "keyboard_layouts");
	struct cow_keyboard_layout *keyboard;
	wl_list_for_each(keyboard, &cow_wm.keyboard_layouts, link)
	{
		if (!keyboard->has_layout)
			continue;
		cJSON *layout = cJSON_CreateObject();
		if (keyboard->name != NULL)
			cJSON_AddStringToObject(layout, "name", keyboard->name);
		else
			cJSON_AddNullToObject(layout, "name");
		cJSON_AddBoolToObject(layout, "active",
		    keyboard == active_layout);
		cJSON_AddItemToArray(layouts, layout);
	}
}

const char *
cow_keyboard_layout_active_name(void)
{
	if (active_layout == NULL || !active_layout->has_layout)
		return (NULL);

	return (active_layout->name);
}

bool
cow_keyboard_layout_set_name(const char *name)
{
	bool found = false;
	struct cow_keyboard_layout *keyboard;

	wl_list_for_each(keyboard, &cow_wm.keyboard_layouts, link)
	{
		river_xkb_keyboard_v1_set_layout_by_name(
		    keyboard->river_keyboard, name);
		found = true;
	}
	return found;
}
