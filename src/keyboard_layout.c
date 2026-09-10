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
#include "cow/update.h"
#include "cow/view.h"
#include "cow/wm.h"

#include "river-xkb-config-v1-client-protocol.h"

static struct cow_keyboard_layout *active_layout;
static struct cow_view *layout_focus_view;

struct observed_layout {
	struct wl_list link;
	char          *name;
};

static struct wl_list observed_layouts;

static void
keyboard_layout_observe(const char *name)
{
	if (name == NULL || name[0] == '\0')
		return;
	struct observed_layout *layout;
	wl_list_for_each(layout, &observed_layouts, link)
	{
		if (strcmp(layout->name, name) == 0)
			return;
	}
	layout = xcalloc(1, sizeof(*layout));
	layout->name = xstrdup(name);
	wl_list_insert(observed_layouts.prev, &layout->link);
}

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
		layout_focus_view = NULL;
		cow_update_request(COW_UPDATE_MANAGE);
	}
	cow_update_request(COW_UPDATE_STATUS);
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
		keyboard_layout_observe(name);
		xstrset(&keyboard->name, name);
		active_layout = keyboard;
		if (cow_config != NULL &&
		    cow_config->keyboard_layout_per_window &&
		    cow_wm.focused_view != NULL)
			xstrset(&cow_wm.focused_view->keyboard_layout, name);
		cow_update_request(COW_UPDATE_STATUS);
	}
}

static void
keyboard_set_lock(bool *lock, bool *has_lock, bool enabled)
{
	if (*has_lock && *lock == enabled)
		return;

	*lock = enabled;
	*has_lock = true;
	cow_update_request(COW_UPDATE_STATUS);
}

static void
keyboard_capslock_enabled(void *data,
    __unused struct river_xkb_keyboard_v1 *river_keyboard)
{
	struct cow_keyboard_layout *keyboard = data;
	keyboard_set_lock(&keyboard->caps_lock, &keyboard->has_caps_lock,
	    true);
}

static void
keyboard_capslock_disabled(void *data,
    __unused struct river_xkb_keyboard_v1 *river_keyboard)
{
	struct cow_keyboard_layout *keyboard = data;
	keyboard_set_lock(&keyboard->caps_lock, &keyboard->has_caps_lock,
	    false);
}

static void
keyboard_numlock_enabled(void *data,
    __unused struct river_xkb_keyboard_v1 *river_keyboard)
{
	struct cow_keyboard_layout *keyboard = data;
	keyboard_set_lock(&keyboard->num_lock, &keyboard->has_num_lock,
	    true);
}

static void
keyboard_numlock_disabled(void *data,
    __unused struct river_xkb_keyboard_v1 *river_keyboard)
{
	struct cow_keyboard_layout *keyboard = data;
	keyboard_set_lock(&keyboard->num_lock, &keyboard->has_num_lock,
	    false);
}

static void
keyboard_scrolllock_enabled(void *data,
    __unused struct river_xkb_keyboard_v1 *river_keyboard)
{
	struct cow_keyboard_layout *keyboard = data;
	keyboard_set_lock(&keyboard->scroll_lock, &keyboard->has_scroll_lock,
	    true);
}

static void
keyboard_scrolllock_disabled(void *data,
    __unused struct river_xkb_keyboard_v1 *river_keyboard)
{
	struct cow_keyboard_layout *keyboard = data;
	keyboard_set_lock(&keyboard->scroll_lock, &keyboard->has_scroll_lock,
	    false);
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
	.scrolllock_enabled = keyboard_scrolllock_enabled,
	.scrolllock_disabled = keyboard_scrolllock_disabled,
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
	wl_list_init(&observed_layouts);
	active_layout = NULL;
	layout_focus_view = NULL;
}

void
cow_keyboard_layout_bind(struct wl_registry *registry, uint32_t name,
    uint32_t version)
{
	uint32_t bind_version = version < 3 ? version : 3;

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
	layout_focus_view = NULL;
	struct observed_layout *layout, *layout_tmp;
	wl_list_for_each_safe(layout, layout_tmp, &observed_layouts, link)
	{
		wl_list_remove(&layout->link);
		xfree(layout->name);
		xfree(layout);
	}

	if (cow_wm.xkb_config != NULL)
		river_xkb_config_v1_stop(cow_wm.xkb_config);
}

void
cow_keyboard_layout_add_status(cJSON *root)
{
	cJSON_AddItemToObject(root, "language",
	    cow_keyboard_layout_active_json());
}

cJSON *
cow_keyboard_layout_active_json(void)
{
	if (active_layout != NULL && active_layout->has_layout) {
		cJSON *language = cJSON_CreateObject();
		if (active_layout->name != NULL)
			cJSON_AddStringToObject(language, "name",
			    active_layout->name);
		else
			cJSON_AddNullToObject(language, "name");
		if (active_layout->has_caps_lock)
			cJSON_AddBoolToObject(language, "caps_lock",
			    active_layout->caps_lock);
		else
			cJSON_AddNullToObject(language, "caps_lock");
		if (active_layout->has_num_lock)
			cJSON_AddBoolToObject(language, "num_lock",
			    active_layout->num_lock);
		else
			cJSON_AddNullToObject(language, "num_lock");
		if (active_layout->has_scroll_lock)
			cJSON_AddBoolToObject(language, "scroll_lock",
			    active_layout->scroll_lock);
		else
			cJSON_AddNullToObject(language, "scroll_lock");
		return language;
	}
	return cJSON_CreateNull();
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

void
cow_keyboard_layout_reconcile_focus(void)
{
	struct cow_view *view = cow_wm.focused_view;

	if (cow_config == NULL || !cow_config->keyboard_layout_per_window) {
		layout_focus_view = NULL;
		return;
	}
	if (view == layout_focus_view)
		return;

	layout_focus_view = view;
	if (view == NULL || active_layout == NULL ||
	    !active_layout->has_layout || active_layout->name == NULL)
		return;

	if (view->keyboard_layout == NULL) {
		view->keyboard_layout = xstrdup(active_layout->name);
		return;
	}
	if (strcmp(view->keyboard_layout, active_layout->name) == 0)
		return;

	cow_log_debug("restore keyboard layout (id=%s layout=%s)",
	    view->identifier != NULL ? view->identifier : "unknown",
	    view->keyboard_layout);
	cow_keyboard_layout_set_name(view->keyboard_layout);
}

void
cow_keyboard_layout_forget_view(struct cow_view *view)
{
	if (layout_focus_view == view)
		layout_focus_view = NULL;
}

static int
keyboard_layout_summary_cmp(const void *a, const void *b)
{
	const struct cow_keyboard_layout_summary *sa = a;
	const struct cow_keyboard_layout_summary *sb = b;

	return strcmp(sa->name, sb->name);
}

size_t
cow_keyboard_layout_summaries(struct cow_keyboard_layout_summary **summaries)
{
	size_t count = 0;
	struct observed_layout *layout;

	*summaries = NULL;
	wl_list_for_each(layout, &observed_layouts, link) count++;
	if (count == 0)
		return 0;
	*summaries = xcalloc(count, sizeof(**summaries));
	size_t i = 0;
	wl_list_for_each(layout, &observed_layouts, link)
	{
		(*summaries)[i].name = xstrdup(layout->name);
		(*summaries)[i].active = active_layout != NULL &&
		    active_layout->name != NULL &&
		    strcmp(active_layout->name, layout->name) == 0;
		i++;
	}
	if (count > 1)
		qsort(*summaries, count, sizeof(**summaries),
		    keyboard_layout_summary_cmp);
	return count;
}

void
cow_keyboard_layout_summaries_free(
    struct cow_keyboard_layout_summary *summaries, size_t count)
{
	for (size_t i = 0; i < count; i++)
		xfree(summaries[i].name);
	xfree(summaries);
}
