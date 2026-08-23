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
#include "cow/module_theme.h"

#include <limits.h>
#include <string.h>

#include "cow/colour.h"
#include "cow/compat.h"

static bool
parse_colour(const cJSON *theme, const char *key, float out[4])
{
	cJSON *value = cJSON_GetObjectItem(theme, key);

	return (cJSON_IsString(value) && value->valuestring != NULL &&
	    cow_colour_parse(value->valuestring, out));
}

bool
cow_module_theme_parse(const cJSON *theme, struct cow_module_theme *out)
{
	if (out == NULL)
		return (false);
	memset(out, 0, sizeof(*out));
	if (!cJSON_IsObject(theme))
		return (false);

	out->has_background = parse_colour(theme, "background",
	    out->background);
	out->has_active_bg = parse_colour(theme, "active_bg", out->active_bg);
	out->has_inactive_bg = parse_colour(theme, "inactive_bg",
	    out->inactive_bg);
	out->has_active_fg = parse_colour(theme, "active_fg", out->active_fg);
	out->has_inactive_fg = parse_colour(theme, "inactive_fg",
	    out->inactive_fg);

	cJSON *font = cJSON_GetObjectItem(theme, "font");
	if (cJSON_IsString(font) && font->valuestring != NULL &&
	    font->valuestring[0] != '\0') {
		strlcpy(out->font, font->valuestring, sizeof(out->font));
		out->has_font = true;
	}

	cJSON *height = cJSON_GetObjectItem(theme, "height");
	if (cJSON_IsNumber(height) && height->valuedouble >= 1.0 &&
	    height->valuedouble <= INT_MAX &&
	    height->valuedouble == (double)height->valueint) {
		out->height = height->valueint;
		out->has_height = true;
	}

	return (true);
}
