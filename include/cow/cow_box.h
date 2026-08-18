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
#if !defined(COW_BOX_H)
#define COW_BOX_H

#include <stdbool.h>
#include <stdint.h>

#include <cJSON.h>

/*
 * Simple box struct replacing wlr_box.
 */
struct cow_box {
	int x, y;
	int width, height;
};

bool cow_box_contains_point(const struct cow_box *, double, double);
void cow_box_intersection(const struct cow_box *, const struct cow_box *, struct cow_box *);
void cow_box_union(const struct cow_box *, const struct cow_box *,
    struct cow_box *);
int64_t cow_box_area(const struct cow_box *);

/*
 * Add {x, y, width, height} number fields for 'box' to 'obj'.
 */
void cow_box_json_add(cJSON *, const struct cow_box *);

#endif
