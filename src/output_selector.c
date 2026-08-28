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

#include <limits.h>
#include <string.h>

#include "cow/output_selector.h"

static int
current_index(const struct cow_output_selector_entry *entries, size_t count)
{
	for (size_t i = 0; i < count; i++)
		if (entries[i].current)
			return (int)i;
	return -1;
}

bool
cow_output_selector_is_number(const char *spec)
{
	if (spec == NULL || spec[0] != '@' ||
	    spec[1] < '0' || spec[1] > '9')
		return false;
	for (const char *p = spec + 2; *p != '\0'; p++)
		if (*p < '0' || *p > '9')
			return false;
	return true;
}

static bool
parse_number(const char *spec, uint32_t *number)
{
	if (!cow_output_selector_is_number(spec))
		return false;

	uint32_t value = 0;
	for (const char *p = spec + 1; *p != '\0'; p++) {
		uint32_t digit = (uint32_t)(*p - '0');
		if (value > (UINT32_MAX - digit) / 10)
			return false;
		value = value * 10 + digit;
	}
	*number = value;
	return true;
}

int
cow_output_selector_resolve(const char *spec,
    const struct cow_output_selector_entry *entries, size_t count)
{
	if (entries == NULL || count == 0)
		return -1;
	if (spec == NULL || *spec == '\0' || strcmp(spec, "@current") == 0)
		return current_index(entries, count);

	if (strcmp(spec, "@next") == 0 || strcmp(spec, "@prev") == 0) {
		int current = current_index(entries, count);
		if (current < 0)
			return -1;
		if (strcmp(spec, "@next") == 0)
			return (current + 1) % (int)count;
		return (current + (int)count - 1) % (int)count;
	}

	uint32_t number;
	if (parse_number(spec, &number)) {
		for (size_t i = 0; i < count; i++)
			if (entries[i].number == number)
				return (int)i;
		return -1;
	}

	for (size_t i = 0; i < count; i++)
		if (entries[i].name != NULL &&
		    strcmp(entries[i].name, spec) == 0)
			return (int)i;
	return -1;
}
