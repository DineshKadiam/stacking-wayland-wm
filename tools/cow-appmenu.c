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
#include <locale.h>

#include "cow/memory.h"
#include "appmenu-entry.h"
#include "appmenu-menu.h"

int
main(int argc, char **argv)
{
	const char *menu_path = NULL, *source = "explicit";
	const char *fallback = COW_DATADIR "/applications.menu";
	cJSON *result, *dirs, *applications;
	char *json, *discovered = NULL;
	int failed = 1, want_menu = 0, have_fallback = 0;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--menu") && !want_menu) {
			want_menu = 1;
			if (i + 1 < argc && strncmp(argv[i + 1], "--", 2))
				menu_path = argv[++i];
		} else if (!strcmp(argv[i], "--fallback") && !have_fallback &&
		    i + 1 < argc) {
			have_fallback = 1;
			fallback = argv[++i];
		} else {
			goto usage;
		}
	}
	if (have_fallback && (!want_menu || menu_path))
		goto usage;
	if (want_menu && !menu_path) {
		int found = appmenu_find_menu(&discovered);

		if (found < 0)
			return 1;
		menu_path = found ? discovered : fallback;
		source = found ? "discovered" : "fallback";
	}
	appmenu_locale_init();
	setlocale(LC_COLLATE, "");
	dirs = appmenu_data_dirs("applications");
	applications = appmenu_catalogue(dirs);
	cJSON_Delete(dirs);
	result = cJSON_CreateObject();
	cJSON_AddNumberToObject(result, "version", 2);
	cJSON_AddItemToObject(result, "applications", applications);
	if (menu_path) {
		cJSON *menu = appmenu_resolve(menu_path, applications);

		if (!menu)
			goto out;
		cJSON_AddStringToObject(result, "menu_file", menu_path);
		cJSON_AddStringToObject(result, "menu_source", source);
		cJSON_AddItemToObject(result, "menu", menu);
	}
	json = cJSON_Print(result);
	if (!json)
		log_fatal("cannot encode application catalogue");
	failed = puts(json) == EOF || fflush(stdout) == EOF;
	free(json);
out:
	cJSON_Delete(result);
	appmenu_locale_finish();
	free(discovered);
	return failed ? 1 : 0;
usage:
	{
		int help = argc == 2 && !strcmp(argv[1], "--help");

		fprintf(help ? stdout : stderr,
		    "usage: cow-appmenu [--menu [FILE]] [--fallback FILE]\n"
		    "Write the visible XDG application catalogue as JSON.\n");
		return help ? 0 : 1;
	}
}
