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

#include "cow/cow_box.h"

#define CHECK(expr)                                                          \
	do {                                                                   \
		if (!(expr)) {                                                  \
			fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, \
			    __LINE__, #expr);                                    \
			abort();                                                   \
		}                                                              \
	} while (0)

static void
test_half_open_intersection(void)
{
	struct cow_box left = { 0, 0, 100, 100 };
	struct cow_box right = { 100, 0, 100, 100 };
	struct cow_box overlap;

	CHECK(cow_box_contains_point(&left, 99, 99));
	CHECK(!cow_box_contains_point(&left, 100, 99));
	cow_box_intersection(&left, &right, &overlap);
	CHECK(cow_box_area(&overlap) == 0);

	right.x = 99;
	cow_box_intersection(&left, &right, &overlap);
	CHECK(overlap.x == 99 && overlap.width == 1);
	CHECK(cow_box_area(&overlap) == 100);
}

static void
test_layout_bounds(void)
{
	struct cow_box bounds = { -1920, 0, 1920, 1080 };
	struct cow_box right = { 0, 0, 2560, 1440 };
	struct cow_box below = { 200, 1600, 1000, 800 };

	cow_box_union(&bounds, &right, &bounds);
	CHECK(bounds.x == -1920 && bounds.y == 0);
	CHECK(bounds.width == 4480 && bounds.height == 1440);

	cow_box_union(&bounds, &below, &bounds);
	CHECK(bounds.x == -1920 && bounds.y == 0);
	CHECK(bounds.width == 4480 && bounds.height == 2400);
}

int
main(void)
{
	test_half_open_intersection();
	test_layout_bounds();
	return 0;
}
