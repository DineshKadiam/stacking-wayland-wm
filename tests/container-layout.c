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
#include <stdio.h>
#include <stdlib.h>

#include "cow/container_layout.h"

#define CHECK(expr)                                                        \
	do {                                                               \
		if (!(expr)) {                                             \
			fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, \
			    #expr);                                        \
			abort();                                           \
		}                                                          \
	} while (0)

int
main(void)
{
	struct cow_container_layout a = { .min_width = 40, .min_height = 30 };
	struct cow_container_layout b = { .min_width = 60, .min_height = 20 };
	struct cow_container_layout c = { .min_width = 25, .min_height = 25 };
	struct cow_container_layout right = {
		.axis  = COW_CONTAINER_VERTICAL,
		.share = 5000,
		.child = { &b, &c }
	};
	struct cow_container_layout root = {
		.axis  = COW_CONTAINER_HORIZONTAL,
		.share = 5000,
		.child = { &a, &right }
	};
	struct cow_box box = { .x = -200,
		.y		  = 50,
		.width		  = 205,
		.height		  = 104 };
	int	       w, h;

	/* Clockwise turns move the right column below A, with C before B.
	 * Unequal shares and node identities survive a full rotation.
	 */
	root.share  = 3000;
	right.share = 6000;
	cow_container_layout_rotate(&root, 1);
	CHECK(root.axis == COW_CONTAINER_VERTICAL && root.child[0] == &a);
	CHECK(right.axis == COW_CONTAINER_HORIZONTAL);
	CHECK(right.child[0] == &c && right.share == 4000);
	CHECK(root.share == 3000);
	cow_container_layout_rotate(&root, 3);
	CHECK(root.axis == COW_CONTAINER_HORIZONTAL && root.child[0] == &a);
	CHECK(right.axis == COW_CONTAINER_VERTICAL);
	CHECK(right.child[0] == &b && right.share == 6000);
	cow_container_layout_rotate(&root, 2);
	CHECK(root.child[0] == &right && root.share == 7000);
	CHECK(right.child[0] == &c && right.share == 4000);
	cow_container_layout_rotate(&root, 2);
	CHECK(root.child[0] == &a && root.share == 3000);
	CHECK(right.child[0] == &b && right.share == 6000);
	root.share = right.share = 5000;

	CHECK(cow_container_layout_minimum(&root, 4, &w, &h));
	CHECK(w == 104 && h == 49);
	CHECK(cow_container_layout_arrange(&root, box, 4));
	CHECK(a.box.width == 100 && right.box.width == 101);
	CHECK(a.box.x + a.box.width == root.divider.x);
	CHECK(root.divider.x + root.divider.width == right.box.x);
	CHECK(right.box.x + right.box.width == box.x + box.width);
	CHECK(b.box.height == 50 && c.box.height == 50);
	CHECK(c.box.y + c.box.height == box.y + box.height);

	/* Clamp to client minima without discarding the requested proportion.
	 */
	root.share = 1000;
	CHECK(cow_container_layout_arrange(&root, box, 4));
	CHECK(a.box.width == 40 && root.share == 1000);
	box.width = 1004;
	CHECK(cow_container_layout_arrange(&root, box, 4));
	CHECK(a.box.width == 100);

	/* Failure must leave the last complete layout intact. */
	box.width = 103;
	CHECK(!cow_container_layout_arrange(&root, box, 4));
	CHECK(a.box.width == 100 && root.box.width == 1004);
	box.width  = 104;
	box.height = 49;
	CHECK(cow_container_layout_arrange(&root, box, 4));
	CHECK(a.box.width == 40 && right.box.width == 60);
	CHECK(b.box.height == 20 && c.box.height == 25);

	/* Exhaust odd/even sizes, split axes, zero gaps, and negative origins.
	 */
	for (int axis = COW_CONTAINER_HORIZONTAL;
	    axis <= COW_CONTAINER_VERTICAL; axis++) {
		root.axis = axis;
		for (int gap = 0; gap <= 5; gap++) {
			CHECK(cow_container_layout_minimum(&root, gap, &w, &h));
			for (int extra = 0; extra < 100; extra++) {
				box.width  = w + extra;
				box.height = h + extra;
				CHECK(cow_container_layout_arrange(&root, box,
				    gap));
				CHECK(a.box.width >= a.min_width);
				CHECK(a.box.height >= a.min_height);
				CHECK(
				    c.box.x + c.box.width == box.x + box.width);
				CHECK(c.box.y + c.box.height ==
				    box.y + box.height);
			}
		}
	}
	/* Hidden panes give up their allocation without changing split shares.
	 */
	a.hidden       = true;
	unsigned share = root.share;
	CHECK(cow_container_layout_arrange(&root, box, 4));
	CHECK(a.box.width == 0 && a.box.height == 0);
	CHECK(right.box.width == box.width && right.box.height == box.height);
	CHECK(root.share == share);
	a.hidden = false;
	CHECK(cow_container_layout_arrange(&root, box, 4));
	CHECK(a.box.width >= a.min_width && root.share == share);
	root.share = 0;
	CHECK(!cow_container_layout_arrange(&root, box, 0));
	root.share = 5000;
	box.x	   = INT_MAX;
	CHECK(!cow_container_layout_arrange(&root, box, 0));
	a.min_width = INT_MAX;
	root.axis   = COW_CONTAINER_HORIZONTAL;
	CHECK(!cow_container_layout_minimum(&root, 4, &w, &h));
	return 0;
}
