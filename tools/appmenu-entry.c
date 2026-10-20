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

#define _POSIX_C_SOURCE 200809L
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cow/memory.h"
#include "cJSON.h"
#include "appmenu-entry.h"

static char *locales[4];

static const char *
value(cJSON *entry, const char *key)
{
	const cJSON *item = cJSON_GetObjectItemCaseSensitive(entry, key);

	return cJSON_IsString(item) ? item->valuestring : "";
}

static const char *
env_default(const char *key, const char *fallback)
{
	const char *s = getenv(key);

	return s && *s ? s : fallback;
}

void
appmenu_locale_init(void)
{
	char *s, *modifier, *territory, *encoding;

	s = xstrdup(env_default("LC_ALL",
	    env_default("LC_MESSAGES", env_default("LANG", "C"))));
	modifier = strchr(s, '@');
	if (modifier)
		*modifier++ = '\0';
	encoding = strchr(s, '.');
	if (encoding)
		*encoding = '\0';
	if (modifier)
		xasprintf(&locales[0], "%s@%s", s, modifier);
	locales[1] = xstrdup(s);
	territory = strchr(s, '_');
	if (territory)
		*territory = '\0';
	if (modifier)
		xasprintf(&locales[2], "%s@%s", s, modifier);
	locales[3] = s;
}

void
appmenu_locale_finish(void)
{
	for (size_t i = 0; i < 4; i++) {
		free(locales[i]);
		locales[i] = NULL;
	}
}

static const char *
localized(cJSON *entry, const char *key)
{
	char *name;
	const char *s;

	for (size_t i = 0; i < 4; i++) {
		if (!locales[i])
			continue;
		xasprintf(&name, "%s[%s]", key, locales[i]);
		s = value(entry, name);
		free(name);
		if (*s)
			return s;
	}
	return value(entry, key);
}

char *
appmenu_unescape(const char *s)
{
	char *result = xstrdup(s), *out = result;

	for (; *s; s++) {
		if (*s == '\\' && s[1]) {
			s++;
			switch (*s) {
			case 's': *out++ = ' '; break;
			case 'n': *out++ = '\n'; break;
			case 't': *out++ = '\t'; break;
			case 'r': *out++ = '\r'; break;
			default: *out++ = *s; break;
			}
		} else {
			*out++ = *s;
		}
	}
	*out = '\0';
	return result;
}

static cJSON *
read_entry(const char *path)
{
	FILE *fp = fopen(path, "r");
	char *line = NULL, *eq, *end;
	size_t size = 0;
	int in_entry = 0;
	cJSON *entry;

	if (!fp)
		return NULL;
	entry = cJSON_CreateObject();
	while (getline(&line, &size, fp) != -1) {
		line[strcspn(line, "\r\n")] = '\0';
		if (line[0] == '[') {
			in_entry = strcmp(line, "[Desktop Entry]") == 0;
			continue;
		}
		if (!in_entry || line[0] == '#' || !(eq = strchr(line, '=')))
			continue;
		*eq++ = '\0';
		end = line + strlen(line);
		while (end > line && (end[-1] == ' ' || end[-1] == '\t'))
			*--end = '\0';
		while (*eq == ' ' || *eq == '\t')
			eq++;
		/* Keep list escapes and Exec syntax intact for their consumers. */
		cJSON_DeleteItemFromObjectCaseSensitive(entry, line);
		cJSON_AddStringToObject(entry, line, eq);
	}
	if (ferror(fp)) {
		fprintf(stderr, "cow-appmenu: reading %s failed\n", path);
		cJSON_Delete(entry);
		entry = NULL;
	}
	free(line);
	fclose(fp);
	return entry;
}

static int
desktop_matches(const char *list)
{
	char *desktops = xstrdup(env_default("XDG_CURRENT_DESKTOP", "CoW"));
	char *save, *desktop;
	int found = 0;

	for (desktop = strtok_r(desktops, ":", &save); desktop;
	    desktop = strtok_r(NULL, ":", &save)) {
		size_t len = strlen(desktop);
		const char *p = list;

		while (*p) {
			size_t n = strcspn(p, ";");

			if (n == len && strncmp(p, desktop, n) == 0)
				found = 1;
			p += n;
			if (*p)
				p++;
		}
	}
	free(desktops);
	return found;
}

static int
executable(const char *path)
{
	struct stat st;

	return stat(path, &st) == 0 && S_ISREG(st.st_mode) &&
	    access(path, X_OK) == 0;
}

static int
visible(cJSON *entry)
{
	return strcmp(value(entry, "Hidden"), "true") != 0 &&
	    strcmp(value(entry, "NoDisplay"), "true") != 0 &&
	    (!*value(entry, "OnlyShowIn") ||
	    desktop_matches(value(entry, "OnlyShowIn"))) &&
	    !desktop_matches(value(entry, "NotShowIn"));
}

static void
add_label(cJSON *object, const char *key, cJSON *entry, const char *field)
{
	char *text = appmenu_unescape(localized(entry, field));

	cJSON_AddStringToObject(object, key, text);
	free(text);
}

cJSON *
appmenu_directory(const char *path)
{
	cJSON *entry = read_entry(path), *metadata = NULL;

	if (!entry)
		return NULL;
	if (strcmp(value(entry, "Type"), "Directory") == 0) {
		metadata = cJSON_CreateObject();
		add_label(metadata, "name", entry, "Name");
		add_label(metadata, "icon", entry, "Icon");
		add_label(metadata, "comment", entry, "Comment");
		cJSON_AddBoolToObject(metadata, "visible", visible(entry));
	}
	cJSON_Delete(entry);
	return metadata;
}

int
appmenu_program_available(const char *command)
{
	char *paths, *dir, *save, *path;
	int found = 0;

	if (!*command)
		found = 0;
	else if (strchr(command, '/'))
		found = command[0] == '/' && executable(command);
	else {
		paths = xstrdup(env_default("PATH", "/usr/bin:/bin"));
		for (dir = strtok_r(paths, ":", &save); dir;
		    dir = strtok_r(NULL, ":", &save)) {
			xasprintf(&path, "%s/%s", dir, command);
			found = executable(path);
			free(path);
			if (found)
				break;
		}
		free(paths);
	}
	return found;
}

static int
try_exec(const char *raw)
{
	char *command = appmenu_unescape(raw);
	int found = !*command || appmenu_program_available(command);
	free(command);
	return found;
}

static void
add_application(cJSON *applications, cJSON *seen, const char *path,
    const char *id)
{
	cJSON *entry, *app;
	char *text;
	static const char *fields[] = {"Name", "GenericName", "Comment", "Icon"};

	if (cJSON_GetObjectItemCaseSensitive(seen, id))
		return;
	/* A higher-priority entry masks lower copies, including Hidden entries. */
	cJSON_AddBoolToObject(seen, id, 1);
	entry = read_entry(path);
	if (!entry)
		return;
	if (strcmp(value(entry, "Type"), "Application") != 0 ||
	    !*localized(entry, "Name") ||
	    !visible(entry) ||
	    !try_exec(value(entry, "TryExec")) ||
	    (!*value(entry, "Exec") &&
	    strcmp(value(entry, "DBusActivatable"), "true") != 0))
		goto out;
	app = cJSON_CreateObject();
	cJSON_AddStringToObject(app, "id", id);
	cJSON_AddStringToObject(app, "file", path);
	for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
		add_label(app, fields[i], entry, fields[i]);
	}
	cJSON_AddStringToObject(app, "Exec", value(entry, "Exec"));
	cJSON_AddStringToObject(app, "Categories", value(entry, "Categories"));
	cJSON_AddStringToObject(app, "Keywords", localized(entry, "Keywords"));
	text = appmenu_unescape(value(entry, "Path"));
	cJSON_AddStringToObject(app, "Path", text);
	free(text);
	cJSON_AddBoolToObject(app, "Terminal",
	    strcmp(value(entry, "Terminal"), "true") == 0);
	cJSON_AddBoolToObject(app, "DBusActivatable",
	    strcmp(value(entry, "DBusActivatable"), "true") == 0);
	cJSON_AddItemToArray(applications, app);
out:
	cJSON_Delete(entry);
}

cJSON *
appmenu_application(const char *path)
{
	cJSON *apps = cJSON_CreateArray(), *seen = cJSON_CreateObject(), *app;
	const char *name = strrchr(path, '/');

	add_application(apps, seen, path, name ? name + 1 : path);
	app = cJSON_DetachItemFromArray(apps, 0);
	cJSON_Delete(apps);
	cJSON_Delete(seen);
	return app;
}

static void
scan(cJSON *applications, cJSON *seen, const char *root,
    const char *relative, unsigned depth)
{
	struct dirent **entries;
	struct stat st;
	char *directory, *path, *child, *id;
	int count;

	if (depth > 64)
		return;
	xasprintf(&directory, "%s%s%s", root, *relative ? "/" : "", relative);
	count = scandir(directory, &entries, NULL, alphasort);
	if (count < 0) {
		if (errno != ENOENT)
			fprintf(stderr, "cow-appmenu: %s: %s\n",
			    directory, strerror(errno));
		free(directory);
		return;
	}
	for (int i = 0; i < count; i++) {
		const char *name = entries[i]->d_name;
		size_t len = strlen(name);

		if (name[0] == '.')
			goto next;
		xasprintf(&path, "%s/%s", directory, name);
		xasprintf(&child, "%s%s%s", relative, *relative ? "/" : "", name);
		if (lstat(path, &st) == 0) {
			/* Do not follow directory symlinks: they can form cycles. */
			if (S_ISDIR(st.st_mode))
				scan(applications, seen, root, child, depth + 1);
			else if (len > 8 && strcmp(name + len - 8, ".desktop") == 0 &&
			    stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
				id = xstrdup(child);
				for (char *p = id; *p; p++) {
					if (*p == '/')
						*p = '-';
				}
				add_application(applications, seen, path, id);
				free(id);
			}
		}
		free(path);
		free(child);
next:
		free(entries[i]);
	}
	free(entries);
	free(directory);
}

static cJSON *
xdg_dirs(const char *home_key, const char *home_suffix, const char *dirs_key,
    const char *defaults, const char *suffix)
{
	char *home, *dirs, *dir, *save, *path;
	const char *data_home;
	cJSON *result = cJSON_CreateArray();

	xasprintf(&home, "%s/%s", env_default("HOME", ""), home_suffix);
	data_home = env_default(home_key, home);
	if (data_home[0] != '/')
		data_home = home;
	xasprintf(&path, "%s/%s", data_home, suffix);
	if (path[0] == '/')
		cJSON_AddItemToArray(result, cJSON_CreateString(path));
	free(path);
	free(home);
	dirs = xstrdup(env_default(dirs_key, defaults));
	for (dir = strtok_r(dirs, ":", &save); dir;
	    dir = strtok_r(NULL, ":", &save)) {
		if (dir[0] != '/')
			continue;
		xasprintf(&path, "%s/%s", dir, suffix);
		cJSON_AddItemToArray(result, cJSON_CreateString(path));
		free(path);
	}
	free(dirs);
	return result;
}

cJSON *
appmenu_data_dirs(const char *suffix)
{
	return xdg_dirs("XDG_DATA_HOME", ".local/share", "XDG_DATA_DIRS",
	    "/usr/local/share:/usr/share", suffix);
}

cJSON *
appmenu_config_dirs(const char *suffix)
{
	return xdg_dirs("XDG_CONFIG_HOME", ".config", "XDG_CONFIG_DIRS",
	    "/etc/xdg", suffix);
}

int
appmenu_find_file(const cJSON *dirs, const char *name, int start, char **path)
{
	struct stat st;

	*path = NULL;
	for (int i = start; i < cJSON_GetArraySize(dirs); i++) {
		const cJSON *dir = cJSON_GetArrayItem(dirs, i);

		xasprintf(path, "%s/%s", dir->valuestring, name);
		if (stat(*path, &st) == 0)
			return 1;
		if (errno != ENOENT && errno != ENOTDIR) {
			fprintf(stderr, "cow-appmenu: %s: %s\n", *path, strerror(errno));
			free(*path);
			*path = NULL;
			return -1;
		}
		free(*path);
		*path = NULL;
	}
	return 0;
}

int
appmenu_find_menu(char **path)
{
	const char *prefix = env_default("XDG_MENU_PREFIX", "");
	cJSON *dirs;
	char *name;
	int found;

	*path = NULL;
	if (strchr(prefix, '/')) {
		fprintf(stderr, "cow-appmenu: XDG_MENU_PREFIX must not contain '/'\n");
		return -1;
	}
	dirs = appmenu_config_dirs("menus");
	xasprintf(&name, "%sapplications.menu", prefix);
	found = appmenu_find_file(dirs, name, 0, path);
	free(name);
	cJSON_Delete(dirs);
	return found;
}

cJSON *
appmenu_catalogue(const cJSON *dirs)
{
	cJSON *applications = cJSON_CreateArray();
	cJSON *seen = cJSON_CreateObject();
	const cJSON *dir;

	cJSON_ArrayForEach(dir, dirs)
		scan(applications, seen, dir->valuestring, "", 0);
	cJSON_Delete(seen);
	return applications;
}
