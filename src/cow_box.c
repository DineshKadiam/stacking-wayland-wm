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
#include "cow/cow_box.h"

bool
cow_box_contains_point(const struct cow_box *box, double px, double py)
{
	return px >= box->x && px < box->x + box->width && py >= box->y &&
	    py < box->y + box->height;
}

void
cow_box_intersection(const struct cow_box *a, const struct cow_box *b,
    struct cow_box *dest)
{
	int x1	= a->x > b->x ? a->x : b->x;
	int y1	= a->y > b->y ? a->y : b->y;
	int x2	= (a->x + a->width) < (b->x + b->width) ? (a->x + a->width) :
							  (b->x + b->width);
	int y2	= (a->y + a->height) < (b->y + b->height) ? (a->y + a->height) :
							    (b->y + b->height);
	dest->x = x1;
	dest->y = y1;
	dest->width  = x2 > x1 ? x2 - x1 : 0;
	dest->height = y2 > y1 ? y2 - y1 : 0;
}

void
cow_box_json_add(cJSON *obj, const struct cow_box *box)
{
	cJSON_AddNumberToObject(obj, "x", box->x);
	cJSON_AddNumberToObject(obj, "y", box->y);
	cJSON_AddNumberToObject(obj, "width", box->width);
	cJSON_AddNumberToObject(obj, "height", box->height);
}
