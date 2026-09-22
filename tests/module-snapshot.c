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

#include "cow/module_snapshot.h"

static char *
make_snapshot(void)
{
	cJSON *root = cJSON_CreateObject();
	cJSON_AddStringToObject(root, "desktop_configuration", "per-output");
	cJSON *theme = cJSON_AddObjectToObject(root, "theme");
	cJSON_AddStringToObject(theme, "background", "0x102030");
	cJSON *titlebar = cJSON_AddObjectToObject(theme, "titlebar");
	cJSON_AddStringToObject(titlebar, "font", "Adwaita Sans Bold 14");
	cJSON *outputs = cJSON_AddArrayToObject(root, "outputs");
	for (int i = 0; i < 24; i++) {
		cJSON *output = cJSON_CreateObject();
		char name[32];
		snprintf(name, sizeof(name), "OUTPUT-%d", i);
		cJSON_AddStringToObject(output, "name", name);
		cJSON_AddNumberToObject(output, "number", i + 1);
		cJSON_AddNumberToObject(output, "x", i * 1920);
		cJSON_AddNumberToObject(output, "output_w", 1920);
		cJSON_AddNumberToObject(output, "output_h", 1080);
		cJSON_AddBoolToObject(output, "current", i == 23);
		cJSON *desks = cJSON_AddArrayToObject(output, "desks");
		for (int d = 0; d < 40; d++) {
			cJSON *desk = cJSON_CreateObject();
			cJSON_AddNumberToObject(desk, "nr", d);
			cJSON_AddStringToObject(desk, "name", "desk");
			cJSON_AddItemToArray(desks, desk);
		}
		cJSON_AddItemToArray(outputs, output);
	}
	cJSON *windows = cJSON_AddArrayToObject(root, "windows");
	for (int i = 0; i < 600; i++) {
		cJSON *window = cJSON_CreateObject();
		char id[32];
		snprintf(id, sizeof(id), "window-%d", i);
		cJSON_AddStringToObject(window, "id", id);
		cJSON_AddStringToObject(window, "app_id", "test");
		cJSON_AddNumberToObject(window, "width", 800);
		cJSON_AddNumberToObject(window, "height", 600);
		cJSON_AddItemToArray(windows, window);
	}
	char *json = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	return json;
}

int
main(void)
{
	char *json = make_snapshot();
	struct cow_module_snapshot snapshot = { 0 };
	int failed = 0;
	if (json == NULL || !cow_module_snapshot_parse(&snapshot, json)) {
		fprintf(stderr, "complete snapshot was not parsed\n");
		failed = 1;
		goto out;
	}
	if (snapshot.n_outs != 24 || snapshot.outs[23].n_desks != 40 ||
	    snapshot.n_wins != 600 ||
	    strcmp(snapshot.wins[599].id, "window-599") != 0) {
		fprintf(stderr, "snapshot collections were truncated\n");
		failed = 1;
	}
	if (!snapshot.titlebar_theme.has_font ||
	    strcmp(snapshot.titlebar_theme.font, "Adwaita Sans Bold 14") != 0 ||
	    snapshot.theme.has_font) {
		fprintf(stderr, "snapshot titlebar font was not preserved\n");
		failed = 1;
	}
	if (cow_module_snapshot_parse(&snapshot, "not json") ||
	    snapshot.n_outs != 24 || snapshot.n_wins != 600) {
		fprintf(stderr, "failed parse replaced the previous snapshot\n");
		failed = 1;
	}

out:
	cow_module_snapshot_fini(&snapshot);
	free(json);
	return failed;
}
