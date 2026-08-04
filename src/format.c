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

#include <string.h>

#include "cow/format.h"
#include "cow/memory.h"

bool
cow_format_parse(const char *format, const char *conversions, bool strict,
    cow_format_text_cb text_cb, cow_format_conversion_cb conversion_cb,
    void *data, char **cause)
{
	const char *literal = format;

	if (cause != NULL)
		*cause = NULL;
	for (const char *p = format; *p != '\0'; p++) {
		if (*p != '%')
			continue;
		if (text_cb != NULL && p > literal)
			text_cb(data, literal, (size_t)(p - literal));
		p++;
		if (*p == '\0') {
			if (strict) {
				if (cause != NULL)
					xasprintf(cause,
					    "trailing %% in format string");
				return (false);
			}
			if (text_cb != NULL)
				text_cb(data, "%", 1);
			return (true);
		}
		if (*p == '%') {
			if (text_cb != NULL)
				text_cb(data, "%", 1);
		} else if (strchr(conversions, *p) != NULL) {
			if (conversion_cb != NULL)
				conversion_cb(data, *p);
		} else if (strict) {
			if (cause != NULL)
				xasprintf(cause,
				    "unknown format conversion: %%%c", *p);
			return (false);
		} else if (text_cb != NULL) {
			text_cb(data, p - 1, 2);
		}
		literal = p + 1;
	}
	if (text_cb != NULL && *literal != '\0')
		text_cb(data, literal, strlen(literal));
	return (true);
}
