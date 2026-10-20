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

#include "cow/icon_lookup.h"
#include "cow/memory.h"
#include "cow/util.h"
#include "appmenu-launch.h"
#include "appmenu-entry.h"
#include "appmenu-output.h"

struct icon_cache {
	cJSON *paths;
	int size;
};

static const char *
string(const cJSON *object, const char *key)
{
	const cJSON *value = cJSON_GetObjectItemCaseSensitive(object, key);

	return cJSON_IsString(value) ? value->valuestring : "";
}

static void
command(size_t argc, const char *const *argv)
{
	char *line = cow_command_from_argv(argc, argv);

	if (!line)
		log_fatal("cannot quote menu command");
	puts(line);
	free(line);
}

static char *
launch_action(const cJSON *app, const char *terminal)
{
	char *file = cow_shell_quote(string(app, "file"));
	char *term = cow_shell_quote(terminal), *shell, *action;
	const char *argv[2];

	if (!file || !term)
		log_fatal("cannot quote application launcher");
	xasprintf(&shell, "cow-appmenu --launch %s --terminal %s", file, term);
	argv[0] = "exec";
	argv[1] = shell;
	action = cow_command_from_argv(2, argv);
	if (!action)
		log_fatal("cannot quote launch command");
	free(file);
	free(term);
	free(shell);
	return action;
}

static void
menu_start(const char *name, const char *label)
{
	const char *title[] = {"menu-add", "-T", "--", name, label, "title"};
	const char *clear[] = {"menu-clear", name};

	/* Ensure the menu exists before clearing it; repeated loads replace it. */
	command(6, title);
	command(2, clear);
	command(6, title);
}

static const char *
icon_path(struct icon_cache *cache, const char *name)
{
	cJSON *cached;
	char *path;

	if (!*name)
		return "";
	cached = cJSON_GetObjectItemCaseSensitive(cache->paths, name);
	if (cached)
		return cached->valuestring;
	path = cow_icon_name_resolve(name, cache->size, NULL);
	cached = cJSON_AddStringToObject(cache->paths, name, path ? path : "");
	free(path);
	return cached->valuestring;
}

static void
menu_item(struct icon_cache *cache, const char *name, const char *label,
    const char *action, const char *icon_name, bool disabled)
{
	const char *icon = icon_path(cache, icon_name), *argv[8];
	size_t n = 0;

	argv[n++] = "menu-add";
	if (disabled)
		argv[n++] = "-d";
	if (*icon) {
		argv[n++] = "-i";
		argv[n++] = icon;
	}
	argv[n++] = "--";
	argv[n++] = name;
	argv[n++] = label;
	argv[n++] = action;
	command(n, argv);
}

static void
application(const cJSON *app, const char *name, const char *terminal,
    struct icon_cache *cache, const char *label)
{
	cJSON *args = appmenu_arguments(app);
	bool available = args && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(app, "available"));
	char *action = available ? launch_action(app, terminal) : xstrdup("nop");

	if (!args)
		fprintf(stderr, "cow-appmenu: disabled unsupported launcher: %s\n",
		    string(app, "file"));
	else if (!available)
		fprintf(stderr, "cow-appmenu: disabled unavailable launcher: %s\n",
		    string(app, "file"));
	menu_item(cache, name, *label ? label : string(app, "Name"), action,
	    string(app, "Icon"), !available);
	cJSON_Delete(args);
	free(action);
}

static const cJSON *
find_menu(const cJSON *menu, const char *id)
{
	const cJSON *child, *found;

	if (!strcmp(string(menu, "id"), id))
		return menu;
	cJSON_ArrayForEach(child, cJSON_GetObjectItemCaseSensitive(menu, "menus")) {
		if ((found = find_menu(child, id)))
			return found;
	}
	return NULL;
}

static void
render(const cJSON *menu, const cJSON *catalogue, const char *base,
    const char *name, const char *terminal, const char *all,
    struct icon_cache *cache)
{
	const cJSON *entry, *object;

	/* Define children first so every popup refers to an existing menu. */
	cJSON_ArrayForEach(object, cJSON_GetObjectItemCaseSensitive(menu, "menus")) {
		char *child;

		xasprintf(&child, "%s/%s", base, string(object, "id"));
		render(object, catalogue, base, child, terminal, NULL, cache);
		free(child);
	}
	menu_start(name, *string(menu, "name") ? string(menu, "name") : name);
	if (!cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(menu, "entries")))
		menu_item(cache, name, "No applications match this menu", "nop", "", true);
	if (all) {
		char *action;

		xasprintf(&action, "popup %s", all);
		const char *argv[] = {"menu-add", "--", name, "All applications", action};
		command(5, argv);
		free(action);
	}
	cJSON_ArrayForEach(entry, cJSON_GetObjectItemCaseSensitive(menu, "entries")) {
		const char *type = string(entry, "type");

		if (!strcmp(type, "separator")) {
			const char *argv[] = {"menu-add", "--", name, "-", "-"};

			command(5, argv);
		} else if (!strcmp(type, "header")) {
			menu_item(cache, name, string(entry, "name"), "title", "", false);
		} else if (!strcmp(type, "application")) {
			cJSON_ArrayForEach(object, catalogue) {
				if (!strcmp(string(object, "id"), string(entry, "id")) &&
				    !strcmp(string(object, "file"), string(entry, "file"))) {
					application(object, name, terminal, cache, string(entry, "name"));
					break;
				}
			}
		} else if (!strcmp(type, "menu")) {
			object = find_menu(menu, string(entry, "id"));
			if (object) {
				char *child, *action;

				xasprintf(&child, "%s/%s", base, string(object, "id"));
				/* popup's suffix is a menu name, not a command argv. */
				xasprintf(&action, "popup %s", child);
				menu_item(cache, name, *string(entry, "name") ?
				    string(entry, "name") : string(object, "name"),
				    action, string(object, "icon"), false);
				free(child);
				free(action);
			}
		}
	}
}

static int
referenced(const cJSON *menu, const cJSON *app)
{
	const cJSON *item;

	cJSON_ArrayForEach(item, cJSON_GetObjectItemCaseSensitive(menu, "entries")) {
		if (!strcmp(string(item, "type"), "application") &&
		    !strcmp(string(item, "id"), string(app, "id")) &&
		    !strcmp(string(item, "file"), string(app, "file")))
			return 1;
		if (!strcmp(string(item, "type"), "menu")) {
			const cJSON *child = find_menu(menu, string(item, "id"));

			if (child && child != menu && referenced(child, app))
				return 1;
		}
	}
	return 0;
}

static int
compare_apps(const void *a, const void *b)
{
	const cJSON *left = *(const cJSON *const *)a;
	const cJSON *right = *(const cJSON *const *)b;
	int result = strcoll(string(left, "Name"), string(right, "Name"));

	return result ? result : strcmp(string(left, "file"), string(right, "file"));
}

int
appmenu_output(const cJSON *menu, const cJSON *catalogue,
    const char *name, const char *terminal, int icon_size)
{
	struct icon_cache cache = {.paths = cJSON_CreateObject(), .size = icon_size};
	const cJSON *app;
	const cJSON **apps = xcalloc(cJSON_GetArraySize(catalogue) + 1, sizeof(*apps));
	size_t count = 0;
	char *all;

	cJSON_ArrayForEach(app, catalogue) {
		if (referenced(menu, app))
			apps[count++] = app;
	}
	qsort(apps, count, sizeof(*apps), compare_apps);
	xasprintf(&all, "%s/all", name);
	puts("# Generated by cow-appmenu; regenerate rather than edit.");
	menu_start(all, "All applications");
	if (!count)
		menu_item(&cache, all, "No applications match this menu", "nop", "", true);
	for (size_t i = 0; i < count; i++)
		application(apps[i], all, terminal, &cache, "");
	render(menu, catalogue, name, name, terminal, count ? all : NULL, &cache);
	free(all);
	free(apps);
	cJSON_Delete(cache.paths);
	return ferror(stdout) || fflush(stdout) == EOF;
}
