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

#include "cow/module_output.h"
#include "cow/output_selector.h"

struct cJSON *
cow_module_output_resolve(struct cJSON *outputs, const char *selector)
{
	if (!cJSON_IsArray(outputs))
		return NULL;

	int count = cJSON_GetArraySize(outputs);
	if (count <= 0)
		return NULL;
	struct cow_output_selector_entry *entries = calloc((size_t)count,
	    sizeof(*entries));
	if (entries == NULL)
		return NULL;

	for (int i = 0; i < count; i++) {
		struct cJSON *output = cJSON_GetArrayItem(outputs, i);
		struct cJSON *name = cJSON_GetObjectItem(output, "name");
		struct cJSON *number = cJSON_GetObjectItem(output, "number");
		entries[i].name = cJSON_IsString(name) ? name->valuestring : NULL;
		entries[i].number = cJSON_IsNumber(number) ?
		    (uint32_t)number->valuedouble : 0;
		entries[i].current = cJSON_IsTrue(
		    cJSON_GetObjectItem(output, "current"));
	}

	int selected = cow_output_selector_resolve(selector, entries,
	    (size_t)count);
	free(entries);
	return selected >= 0 ? cJSON_GetArrayItem(outputs, selected) : NULL;
}
