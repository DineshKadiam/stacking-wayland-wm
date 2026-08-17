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

#include "cow/app_icon.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cow/compat.h"
#include "cow/image.h"
#include "cow/memory.h"

struct desktop_icon {
	char *icon;
	char *startup_wm_class;
};

struct icon_match {
	char *path;
	int   score;
};

static bool  supported_image_path(const char *);
static bool  svg_uses_text(const char *);
static bool  renderable_image_path(const char *);
static void  desktop_icon_fini(struct desktop_icon *);
static bool  desktop_icon_read(const char *, struct desktop_icon *);
static char *desktop_icon_find(const char *, const char *);
static char *desktop_icon_from_data_dirs(const char *);
static char *icon_name_find(const char *, int);
static void  icon_theme_search(const char *, const char *, int,
     struct icon_match *);

static bool
supported_image_path(const char *path)
{
	const char *dot = path != NULL ? strrchr(path, '.') : NULL;

	return dot != NULL &&
	    (strcasecmp(dot, ".png") == 0 || strcasecmp(dot, ".svg") == 0 ||
	    strcasecmp(dot, ".xpm") == 0);
}

static bool
svg_uses_text(const char *path)
{
	const char *dot = path != NULL ? strrchr(path, '.') : NULL;
	if (dot == NULL || strcasecmp(dot, ".svg") != 0)
		return false;

	FILE *fp = fopen(path, "r");
	if (fp == NULL)
		return false;
	char *line = NULL;
	size_t capacity = 0;
	bool found = false;
	while (getline(&line, &capacity, fp) != -1) {
		if (strstr(line, "<text") != NULL ||
		    strstr(line, ":text") != NULL) {
			found = true;
			break;
		}
	}
	free(line);
	fclose(fp);
	return found;
}

static bool
renderable_image_path(const char *path)
{
	if (svg_uses_text(path))
		return false;

	uint32_t pixels[16 * 16] = { 0 };
	struct cow_pixbuf pixbuf = {
		.pixels = pixels,
		.stride_px = 16,
		.width = 16,
		.height = 16,
	};

	return cow_image_render_rect(path, &pixbuf, 0, 0, 16, 16);
}

static void
desktop_icon_fini(struct desktop_icon *entry)
{
	xfree(entry->icon);
	xfree(entry->startup_wm_class);
	memset(entry, 0, sizeof(*entry));
}

static char *
trim(char *s)
{
	while (isspace((unsigned char)*s))
		s++;
	char *end = s + strlen(s);
	while (end > s && isspace((unsigned char)end[-1]))
		*--end = '\0';
	return s;
}

static bool
desktop_icon_read(const char *path, struct desktop_icon *entry)
{
	FILE *fp = fopen(path, "r");
	if (fp == NULL)
		return false;

	char  *line		= NULL;
	size_t cap		= 0;
	bool   in_desktop_entry = false;
	while (getline(&line, &cap, fp) != -1) {
		char *s = trim(line);
		if (*s == '\0' || *s == '#')
			continue;
		if (*s == '[') {
			in_desktop_entry = strcmp(s, "[Desktop Entry]") == 0;
			continue;
		}
		if (!in_desktop_entry)
			continue;
		char *eq = strchr(s, '=');
		if (eq == NULL)
			continue;
		*eq++	    = '\0';
		char *key   = trim(s);
		char *value = trim(eq);
		if (strcmp(key, "Icon") == 0 && entry->icon == NULL)
			entry->icon = xstrdup(value);
		else if (strcmp(key, "StartupWMClass") == 0 &&
		    entry->startup_wm_class == NULL)
			entry->startup_wm_class = xstrdup(value);
	}
	free(line);
	fclose(fp);
	return entry->icon != NULL;
}

static bool
desktop_name_matches(const char *name, const char *app_id)
{
	size_t nlen = strlen(name);
	size_t alen = strlen(app_id);

	if (nlen == alen && strncasecmp(name, app_id, alen) == 0)
		return true;
	return nlen == alen + strlen(".desktop") &&
	    strncasecmp(name, app_id, alen) == 0 &&
	    strcasecmp(name + alen, ".desktop") == 0;
}

static char *
desktop_icon_find(const char *applications, const char *app_id)
{
	DIR *dir = opendir(applications);
	if (dir == NULL)
		return NULL;

	char	      *fallback = NULL;
	struct dirent *de;
	while ((de = readdir(dir)) != NULL) {
		size_t len = strlen(de->d_name);
		if (len < strlen(".desktop") ||
		    strcasecmp(de->d_name + len - strlen(".desktop"),
			".desktop") != 0)
			continue;

		char path[PATH_MAX];
		if (snprintf(path, sizeof(path), "%s/%s", applications,
			de->d_name) >= (int)sizeof(path))
			continue;

		struct desktop_icon entry = { 0 };
		if (!desktop_icon_read(path, &entry)) {
			desktop_icon_fini(&entry);
			continue;
		}
		if (desktop_name_matches(de->d_name, app_id)) {
			xfree(fallback);
			fallback   = entry.icon;
			entry.icon = NULL;
			desktop_icon_fini(&entry);
			break;
		}
		if (fallback == NULL && entry.startup_wm_class != NULL &&
		    strcasecmp(entry.startup_wm_class, app_id) == 0) {
			fallback   = entry.icon;
			entry.icon = NULL;
		}
		desktop_icon_fini(&entry);
	}
	closedir(dir);
	return fallback;
}

static char *
desktop_icon_from_data_dirs(const char *app_id)
{
	const char *data_home = getenv("XDG_DATA_HOME");
	char	    home_path[PATH_MAX];
	if (data_home == NULL || *data_home == '\0') {
		const char *home = getenv("HOME");
		if (home != NULL &&
		    snprintf(home_path, sizeof(home_path), "%s/.local/share",
			home) < (int)sizeof(home_path))
			data_home = home_path;
	}

	if (data_home != NULL) {
		char applications[PATH_MAX];
		if (snprintf(applications, sizeof(applications),
			"%s/applications",
			data_home) < (int)sizeof(applications)) {
			char *icon = desktop_icon_find(applications, app_id);
			if (icon != NULL)
				return icon;
		}
	}

	const char *dirs = getenv("XDG_DATA_DIRS");
	if (dirs == NULL || *dirs == '\0')
		dirs = "/usr/local/share:/usr/share";
	char *copy   = xstrdup(dirs);
	char *cursor = copy;
	char *dir;
	while ((dir = strsep(&cursor, ":")) != NULL) {
		if (*dir == '\0')
			continue;
		char applications[PATH_MAX];
		if (snprintf(applications, sizeof(applications),
			"%s/applications", dir) >= (int)sizeof(applications))
			continue;
		char *icon = desktop_icon_find(applications, app_id);
		if (icon != NULL) {
			xfree(copy);
			return icon;
		}
	}
	xfree(copy);
	return NULL;
}

static int
icon_size_score(const char *path, int wanted)
{
	const char *p	  = path;
	int	    score = 100000;
	while ((p = strchr(p, '/')) != NULL) {
		p++;
		int width, height;
		if (sscanf(p, "%dx%d/", &width, &height) == 2 &&
		    width == height) {
			int delta = width - wanted;
			if (delta < 0)
				delta = -delta + 1000;
			if (delta < score)
				score = delta;
		}
	}
	if (strstr(path, "/scalable/") != NULL)
		score = 1;
	return score;
}

static void
icon_theme_walk(const char *dir, const char *name, int wanted, int depth,
    struct icon_match *best)
{
	if (depth > 5)
		return;
	DIR *dp = opendir(dir);
	if (dp == NULL)
		return;

	struct dirent *de;
	while ((de = readdir(dp)) != NULL) {
		if (de->d_name[0] == '.')
			continue;
		char path[PATH_MAX];
		if (snprintf(path, sizeof(path), "%s/%s", dir, de->d_name) >=
		    (int)sizeof(path))
			continue;
		struct stat st;
		if (stat(path, &st) != 0)
			continue;
		if (S_ISDIR(st.st_mode)) {
			icon_theme_walk(path, name, wanted, depth + 1, best);
			continue;
		}
		if (!S_ISREG(st.st_mode) || !supported_image_path(path))
			continue;
		const char *dot = strrchr(de->d_name, '.');
		if (dot == NULL || (size_t)(dot - de->d_name) != strlen(name) ||
		    strncasecmp(de->d_name, name, strlen(name)) != 0)
			continue;
		int score = icon_size_score(path, wanted);
		if ((best->path == NULL || score < best->score) &&
		    renderable_image_path(path)) {
			xstrset(&best->path, path);
			best->score = score;
		}
	}
	closedir(dp);
}

static void
icon_theme_search(const char *data_dir, const char *name, int wanted,
    struct icon_match *best)
{
	char path[PATH_MAX];
	if (snprintf(path, sizeof(path), "%s/icons/hicolor", data_dir) <
	    (int)sizeof(path))
		icon_theme_walk(path, name, wanted, 0, best);
	if (best->path != NULL)
		return;

	const char *extensions[] = { ".png", ".svg", ".xpm" };
	for (size_t i = 0; i < sizeof(extensions) / sizeof(extensions[0]);
	    i++) {
		if (snprintf(path, sizeof(path), "%s/pixmaps/%s%s", data_dir,
			name, extensions[i]) >= (int)sizeof(path))
			continue;
		if (access(path, R_OK) == 0 && renderable_image_path(path)) {
			best->path  = xstrdup(path);
			best->score = 0;
			return;
		}
	}
}

static char *
icon_name_find(const char *name, int wanted)
{
	if (name == NULL || *name == '\0')
		return NULL;
	if (name[0] == '/' && supported_image_path(name) &&
	    access(name, R_OK) == 0 && renderable_image_path(name))
		return xstrdup(name);

	char *base = xstrdup(name);
	char *dot  = strrchr(base, '.');
	if (dot != NULL && supported_image_path(base))
		*dot = '\0';

	struct icon_match best	    = { .score = INT_MAX };
	const char	 *data_home = getenv("XDG_DATA_HOME");
	char		  home_path[PATH_MAX];
	if (data_home == NULL || *data_home == '\0') {
		const char *home = getenv("HOME");
		if (home != NULL &&
		    snprintf(home_path, sizeof(home_path), "%s/.local/share",
			home) < (int)sizeof(home_path))
			data_home = home_path;
	}
	if (data_home != NULL)
		icon_theme_search(data_home, base, wanted, &best);

	const char *dirs = getenv("XDG_DATA_DIRS");
	if (dirs == NULL || *dirs == '\0')
		dirs = "/usr/local/share:/usr/share";
	char *copy   = xstrdup(dirs);
	char *cursor = copy;
	char *dir;
	while (best.path == NULL && (dir = strsep(&cursor, ":")) != NULL)
		if (*dir != '\0')
			icon_theme_search(dir, base, wanted, &best);
	xfree(copy);
	xfree(base);
	return best.path;
}

char *
cow_app_icon_resolve(const char *app_id, int wanted)
{
	if (app_id == NULL || *app_id == '\0')
		return NULL;
	if (wanted <= 0)
		wanted = 64;

	char *name = desktop_icon_from_data_dirs(app_id);
	char *path = icon_name_find(name != NULL ? name : app_id, wanted);
	if (path == NULL && name != NULL) {
		char *colour_name;
		xasprintf(&colour_name, "%s-color", app_id);
		path = icon_name_find(colour_name, wanted);
		xfree(colour_name);
	}
	if (path == NULL && name != NULL)
		path = icon_name_find(app_id, wanted);
	xfree(name);
	return path;
}
