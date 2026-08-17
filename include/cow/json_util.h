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
#if !defined(COW_JSON_UTIL_H)
#define COW_JSON_UTIL_H

#include <stdbool.h>
#include <stdint.h>

#include <cJSON.h>

/*
 * Tiny accessors that combine "object has key" + "key has expected type"
 * + "fetch" + "default if absent" into a single call.  Each returns true
 * if a value of the right type was found and assigned to *out, false
 * otherwise; *out is left untouched on the false path so the caller can
 * pre-seed it with a default.
 */
bool cow_json_get_int(const cJSON *, const char *, long long *);
bool cow_json_get_double(const cJSON *, const char *, double *);
bool cow_json_get_bool(const cJSON *, const char *, bool *);
bool cow_json_get_string(const cJSON *, const char *, const char **);
bool cow_json_copy_string(const cJSON *, const char *, char *, size_t);

#endif /* COW_JSON_UTIL_H */
