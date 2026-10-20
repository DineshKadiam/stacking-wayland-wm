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

#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cow/icon_lookup.h"
#include "cow/memory.h"

struct icon_match {
	char *path;
	int score;
	bool (*accept)(const char *);
};

static bool
supported_image_path(const char *path)
{
	const char *dot = path != NULL ? strrchr(path, '.') : NULL;

	return dot != NULL &&
	    (strcasecmp(dot, ".png") == 0 || strcasecmp(dot, ".svg") == 0 ||
	    strcasecmp(dot, ".xpm") == 0);
}

static bool
usable(const char *path, bool (*accept)(const char *))
{
	struct stat st;

	return stat(path, &st) == 0 && S_ISREG(st.st_mode) &&
	    access(path, R_OK) == 0 && (!accept || accept(path));
}

static int
icon_size_score(const char *path, int wanted)
{
	const char *p = path;
	int score = 100000;

	while ((p = strchr(p, '/')) != NULL) {
		int width, height;

		p++;
		if (sscanf(p, "%dx%d/", &width, &height) == 2 && width == height) {
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
		    usable(path, best->accept)) {
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
	for (size_t i = 0; i < sizeof(extensions) / sizeof(extensions[0]); i++) {
		if (snprintf(path, sizeof(path), "%s/pixmaps/%s%s", data_dir,
			name, extensions[i]) >= (int)sizeof(path))
			continue;
		if (usable(path, best->accept)) {
			best->path = xstrdup(path);
			best->score = 0;
			return;
		}
	}
}

char *
cow_icon_name_resolve(const char *name, int wanted, bool (*accept)(const char *))
{
	if (name == NULL || *name == '\0')
		return NULL;
	if (name[0] == '/' && supported_image_path(name) && usable(name, accept))
		return xstrdup(name);
	if (strchr(name, '/'))
		return NULL;
	if (wanted <= 0)
		wanted = 64;

	char *base = xstrdup(name);
	char *dot = strrchr(base, '.');
	if (dot != NULL && supported_image_path(base))
		*dot = '\0';

	struct icon_match best = {.score = INT_MAX, .accept = accept};
	const char *data_home = getenv("XDG_DATA_HOME");
	char home_path[PATH_MAX];
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
	char *copy = xstrdup(dirs);
	char *cursor = copy;
	char *dir;
	while (best.path == NULL && (dir = strsep(&cursor, ":")) != NULL)
		if (*dir != '\0')
			icon_theme_search(dir, base, wanted, &best);
	free(copy);
	free(base);
	return best.path;
}
