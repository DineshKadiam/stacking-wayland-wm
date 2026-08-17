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
#include "cow/placement.h"

#include <stdlib.h>

#include "cow/memory.h"

static int
compare_box_x(const void *pa, const void *pb)
{
	const struct cow_box *a = pa;
	const struct cow_box *b = pb;

	if (a->x != b->x)
		return a->x < b->x ? -1 : 1;
	if (a->width != b->width)
		return a->width < b->width ? -1 : 1;
	return 0;
}

static int
compare_int(const void *pa, const void *pb)
{
	int a = *(const int *)pa;
	int b = *(const int *)pb;

	return (a > b) - (a < b);
}

/*
 * Find the leftmost position at this y coordinate.  Obstacles are sorted by
 * their left edge, so advancing x past an intersecting obstacle cannot skip
 * an earlier usable position.
 */
static bool
find_empty_x(const struct cow_box *obstacles, size_t obstacle_count,
    const struct cow_box *usable, int frame_w, int frame_h, int y, int *result)
{
	int x = usable->x;
	int usable_right = usable->x + usable->width;

	for (size_t i = 0; i < obstacle_count; i++) {
		const struct cow_box *obstacle = &obstacles[i];
		int obstacle_right = obstacle->x + obstacle->width;
		int obstacle_bottom = obstacle->y + obstacle->height;

		if (y + frame_h <= obstacle->y || y >= obstacle_bottom)
			continue;
		if (obstacle_right <= x)
			continue;
		if (obstacle->x >= x + frame_w) {
			*result = x;
			return true;
		}

		x = obstacle_right;
		if (x + frame_w > usable_right)
			return false;
	}

	if (x + frame_w > usable_right)
		return false;
	*result = x;
	return true;
}

bool
cow_placement_find_empty(const struct cow_box *usable, int frame_w,
    int frame_h, const struct cow_box *occupied, size_t occupied_count,
    int gap, int *result_x, int *result_y)
{
	if (usable == NULL || result_x == NULL || result_y == NULL ||
	    frame_w <= 0 || frame_h <= 0 || frame_w > usable->width ||
	    frame_h > usable->height)
		return false;
	if (occupied_count > 0 && occupied == NULL)
		return false;
	if (gap < 0)
		gap = 0;

	struct cow_box *obstacles = NULL;
	if (occupied_count > 0)
		obstacles = xcalloc(occupied_count, sizeof(*obstacles));
	int *rows = xcalloc(occupied_count + 1, sizeof(*rows));

	int max_y = usable->y + usable->height - frame_h;
	size_t row_count = 1;
	rows[0] = usable->y;
	for (size_t i = 0; i < occupied_count; i++) {
		obstacles[i] = occupied[i];
		obstacles[i].x -= gap;
		obstacles[i].y -= gap;
		obstacles[i].width += gap * 2;
		obstacles[i].height += gap * 2;

		int next_y = obstacles[i].y + obstacles[i].height;
		if (next_y > usable->y && next_y <= max_y)
			rows[row_count++] = next_y;
	}

	if (occupied_count > 1)
		qsort(obstacles, occupied_count, sizeof(*obstacles),
		    compare_box_x);
	if (row_count > 1)
		qsort(rows, row_count, sizeof(*rows), compare_int);

	bool found = false;
	for (size_t i = 0; i < row_count; i++) {
		if (i > 0 && rows[i] == rows[i - 1])
			continue;
		if (!find_empty_x(obstacles, occupied_count, usable, frame_w,
		    frame_h, rows[i], result_x))
			continue;

		*result_y = rows[i];
		found = true;
		break;
	}

	xfree(rows);
	xfree(obstacles);
	return found;
}
