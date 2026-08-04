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
#if !defined(COW_FORMAT_H)
#define COW_FORMAT_H

#include <stdbool.h>
#include <stddef.h>

typedef void (*cow_format_text_cb)(void *, const char *, size_t);
typedef void (*cow_format_conversion_cb)(void *, char);

/*
 * Parse percent conversions in format.  Text and recognised conversions are
 * delivered to their callbacks; %% is delivered as literal text.  In strict
 * mode an unknown conversion or trailing % returns false and sets cause.
 * Otherwise it is preserved as literal text.
 */
bool cow_format_parse(const char *, const char *, bool, cow_format_text_cb,
    cow_format_conversion_cb, void *, char **);

#endif
