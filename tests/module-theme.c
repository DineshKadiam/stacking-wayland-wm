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
#include <math.h>
#include <stdio.h>
#include <string.h>

#include <cJSON.h>

#include "cow/module_theme.h"

static bool
near(float actual, float expected)
{
	return (fabsf(actual - expected) < 0.005f);
}

static int
test_complete(void)
{
	const char *json =
	    "{\"background\":\"0x01020380\","
	    "\"active_bg\":\"0x112233\","
	    "\"inactive_bg\":\"0x445566\","
	    "\"active_fg\":\"0x778899\","
	    "\"inactive_fg\":\"0xAABBCC\","
	    "\"font\":\"Sans 14\",\"height\":31}";
	cJSON *root = cJSON_Parse(json);
	struct cow_module_theme theme;
	int failed = 0;

	if (root == NULL || !cow_module_theme_parse(root, &theme)) {
		fprintf(stderr, "complete theme was not parsed\n");
		failed = 1;
		goto out;
	}
	if (!theme.has_background || !theme.has_active_bg ||
	    !theme.has_inactive_bg || !theme.has_active_fg ||
	    !theme.has_inactive_fg || !theme.has_font || !theme.has_height) {
		fprintf(stderr, "complete theme fields were not recorded\n");
		failed = 1;
	}
	if (!near(theme.background[0], 1.0f / 255.0f) ||
	    !near(theme.background[3], 128.0f / 255.0f)) {
		fprintf(stderr, "RGBA theme colour was not preserved\n");
		failed = 1;
	}
	if (strcmp(theme.font, "Sans 14") != 0 ||
	    theme.height != 31) {
		fprintf(stderr, "theme font or height was not parsed\n");
		failed = 1;
	}

out:
	cJSON_Delete(root);
	return (failed);
}

static int
test_partial_and_invalid(void)
{
	cJSON *root = cJSON_Parse(
	    "{\"active_bg\":\"not-a-colour\",\"font\":\"\","
	    "\"height\":0}");
	struct cow_module_theme theme;
	int failed = 0;

	if (root == NULL || !cow_module_theme_parse(root, &theme)) {
		fprintf(stderr, "partial theme was not parsed\n");
		failed = 1;
		goto out;
	}
	if (theme.has_background || theme.has_active_bg || theme.has_font ||
	    theme.has_height) {
		fprintf(stderr, "invalid theme fields were accepted\n");
		failed = 1;
	}

	memset(&theme, 0xff, sizeof(theme));
	if (cow_module_theme_parse(NULL, &theme) || theme.has_background ||
	    theme.has_font || theme.has_height) {
		fprintf(stderr, "missing theme did not produce an empty result\n");
		failed = 1;
	}

out:
	cJSON_Delete(root);
	return (failed);
}

int
main(void)
{
	return (test_complete() || test_partial_and_invalid());
}
