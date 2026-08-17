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

#include "cow/placement.h"

#define CHECK(expr)                                                          \
	do {                                                                   \
		if (!(expr)) {                                                  \
			fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, \
			    __LINE__, #expr);                                    \
			abort();                                                   \
		}                                                              \
	} while (0)

static void
test_empty_and_exact_fit(void)
{
	struct cow_box usable = { 10, 20, 100, 80 };
	int x, y;

	CHECK(cow_placement_find_empty(&usable, 100, 80, NULL, 0, 5,
	    &x, &y));
	CHECK(x == 10 && y == 20);
	CHECK(!cow_placement_find_empty(&usable, 101, 80, NULL, 0, 5,
	    &x, &y));
}

static void
test_reading_order(void)
{
	struct cow_box usable = { 0, 0, 300, 200 };
	struct cow_box occupied[] = {
		{ 105, 0, 100, 80 },
		{ 0, 0, 100, 80 },
	};
	int x, y;

	CHECK(cow_placement_find_empty(&usable, 90, 70, occupied, 2, 5,
	    &x, &y));
	CHECK(x == 210 && y == 0);

	CHECK(cow_placement_find_empty(&usable, 100, 70, occupied, 2, 5,
	    &x, &y));
	CHECK(x == 0 && y == 85);
}

static void
test_gap_and_bottom_boundary(void)
{
	struct cow_box usable = { 0, 0, 100, 100 };
	struct cow_box occupied = { 0, 0, 100, 60 };
	int x, y;

	CHECK(cow_placement_find_empty(&usable, 100, 35, &occupied, 1, 5,
	    &x, &y));
	CHECK(x == 0 && y == 65);
	CHECK(!cow_placement_find_empty(&usable, 100, 36, &occupied, 1, 5,
	    &x, &y));
	CHECK(cow_placement_find_empty(&usable, 100, 40, &occupied, 1, 0,
	    &x, &y));
	CHECK(x == 0 && y == 60);
}

static void
test_gap_is_enforced_before_obstacle(void)
{
	struct cow_box usable = { 0, 0, 200, 100 };
	struct cow_box occupied = { 52, 0, 50, 100 };
	int x, y;

	CHECK(cow_placement_find_empty(&usable, 50, 100, &occupied, 1, 0,
	    &x, &y));
	CHECK(x == 0 && y == 0);
	CHECK(cow_placement_find_empty(&usable, 50, 100, &occupied, 1, 5,
	    &x, &y));
	CHECK(x == 107 && y == 0);
}

int
main(void)
{
	test_empty_and_exact_fit();
	test_reading_order();
	test_gap_and_bottom_boundary();
	test_gap_is_enforced_before_obstacle();
	return 0;
}
