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
#include <stdlib.h>
#include <string.h>

#include "cow/json_util.h"
#include "cow/memory.h"
#include "cow/module_snapshot.h"

static int
json_int(cJSON *object, const char *name)
{
	cJSON *item = cJSON_GetObjectItem(object, name);
	return cJSON_IsNumber(item) ? item->valueint : 0;
}

static void
parse_desks(struct cow_module_output_state *output, cJSON *array)
{
	if (!cJSON_IsArray(array))
		return;
	int count = cJSON_GetArraySize(array);
	if (count <= 0)
		return;
	output->desks = xcalloc((size_t)count, sizeof(*output->desks));
	cJSON *item;
	size_t index = 0;
	cJSON_ArrayForEach(item, array)
	{
		struct cow_module_desk *desk = &output->desks[index++];
		desk->nr = json_int(item, "nr");
		desk->window_count = json_int(item, "windows");
		desk->active = cJSON_IsTrue(cJSON_GetObjectItem(item, "active"));
		desk->collected = cJSON_IsTrue(
		    cJSON_GetObjectItem(item, "collected"));
		cow_json_copy_string(item, "name", desk->name,
		    sizeof(desk->name));
	}
	output->n_desks = index;
}

static bool
parse_outputs(struct cow_module_snapshot *snapshot, cJSON *array)
{
	if (!cJSON_IsArray(array))
		return false;
	int count = cJSON_GetArraySize(array);
	if (count <= 0)
		return true;
	snapshot->outs = xcalloc((size_t)count, sizeof(*snapshot->outs));
	cJSON *item;
	size_t index = 0;
	cJSON_ArrayForEach(item, array)
	{
		struct cow_module_output_state *output = &snapshot->outs[index++];
		cow_json_copy_string(item, "name", output->name,
		    sizeof(output->name));
		output->number = json_int(item, "number");
		output->x = json_int(item, "x");
		output->y = json_int(item, "y");
		output->w = json_int(item, "width");
		output->h = json_int(item, "height");
		output->current = cJSON_IsTrue(cJSON_GetObjectItem(item, "current"));
		output->current_desk = json_int(item, "current_desk");
		output->page_cols = json_int(item, "page_cols");
		output->page_rows = json_int(item, "page_rows");
		output->page_col = json_int(item, "page_col");
		output->page_row = json_int(item, "page_row");
		output->page_vx = json_int(item, "page_vx");
		output->page_vy = json_int(item, "page_vy");
		output->output_w = json_int(item, "output_w");
		output->output_h = json_int(item, "output_h");
		cJSON *usable = cJSON_GetObjectItem(item, "usable");
		output->usable_x = json_int(usable, "x");
		output->usable_y = json_int(usable, "y");
		output->usable_w = json_int(usable, "width");
		output->usable_h = json_int(usable, "height");
		struct cow_module_theme theme;
		(void)cow_module_theme_parse(item, &theme);
		output->has_background = theme.has_background;
		if (theme.has_background)
			memcpy(output->background, theme.background,
			    sizeof(output->background));
		parse_desks(output, cJSON_GetObjectItem(item, "desks"));
	}
	snapshot->n_outs = index;
	return true;
}

static bool
parse_windows(struct cow_module_snapshot *snapshot, cJSON *array)
{
	if (!cJSON_IsArray(array))
		return false;
	int count = cJSON_GetArraySize(array);
	if (count <= 0)
		return true;
	snapshot->wins = xcalloc((size_t)count, sizeof(*snapshot->wins));
	cJSON *item;
	size_t index = 0;
	cJSON_ArrayForEach(item, array)
	{
		struct cow_module_window_state *window = &snapshot->wins[index++];
		cow_json_copy_string(item, "id", window->id, sizeof(window->id));
		cow_json_copy_string(item, "app_id", window->app_id,
		    sizeof(window->app_id));
		cow_json_copy_string(item, "title", window->title,
		    sizeof(window->title));
		cow_json_copy_string(item, "output", window->output,
		    sizeof(window->output));
		cow_json_copy_string(item, "icon", window->icon,
		    sizeof(window->icon));
		window->desk = json_int(item, "desk");
		window->x = json_int(item, "x");
		window->y = json_int(item, "y");
		window->vx = json_int(item, "vx");
		window->vy = json_int(item, "vy");
		window->w = json_int(item, "width");
		window->h = json_int(item, "height");
		window->icon_vx = json_int(item, "icon_vx");
		window->icon_vy = json_int(item, "icon_vy");
		window->icon_w = json_int(item, "icon_width");
		window->icon_h = json_int(item, "icon_height");
		window->focused = cJSON_IsTrue(cJSON_GetObjectItem(item, "focused"));
		window->iconified = cJSON_IsTrue(
		    cJSON_GetObjectItem(item, "iconified"));
		window->container_hidden = cJSON_IsTrue(
		    cJSON_GetObjectItem(item, "container_hidden"));
	}
	snapshot->n_wins = index;
	return true;
}

void
cow_module_snapshot_fini(struct cow_module_snapshot *snapshot)
{
	if (snapshot == NULL)
		return;
	for (size_t i = 0; i < snapshot->n_outs; i++)
		xfree(snapshot->outs[i].desks);
	xfree(snapshot->outs);
	xfree(snapshot->wins);
	*snapshot = (struct cow_module_snapshot){ 0 };
}

bool
cow_module_snapshot_parse(struct cow_module_snapshot *snapshot,
    const char *json)
{
	if (snapshot == NULL || json == NULL)
		return false;
	cJSON *root = cJSON_Parse(json);
	if (root == NULL)
		return false;
	struct cow_module_snapshot next = { 0 };
	bool ok = parse_outputs(&next, cJSON_GetObjectItem(root, "outputs")) &&
	    parse_windows(&next, cJSON_GetObjectItem(root, "windows"));
	if (ok) {
		cow_json_copy_string(root, "desktop_configuration",
		    next.desktop_config, sizeof(next.desktop_config));
		cJSON *theme = cJSON_GetObjectItem(root, "theme");
		(void)cow_module_theme_parse(theme, &next.theme);
		if (cJSON_IsObject(theme))
			(void)cow_module_theme_parse(
			    cJSON_GetObjectItem(theme, "titlebar"),
			    &next.titlebar_theme);
		cow_module_snapshot_fini(snapshot);
		*snapshot = next;
	} else
		cow_module_snapshot_fini(&next);
	cJSON_Delete(root);
	return ok;
}
