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
#include <stdint.h>

#include "cow/container_layout.h"

static int
maximum(int a, int b)
{
	return a > b ? a : b;
}

void
cow_container_layout_rotate(struct cow_container_layout *node, unsigned turns)
{
	if (node->axis == COW_CONTAINER_LEAF)
		return;
	turns %= 4;
	for (unsigned i = 0; i < turns; i++) {
		if (node->axis == COW_CONTAINER_VERTICAL) {
			struct cow_container_layout *first = node->child[0];
			node->child[0]			   = node->child[1];
			node->child[1]			   = first;
			node->share = 10000 - node->share;
			node->axis  = COW_CONTAINER_HORIZONTAL;
		} else {
			node->axis = COW_CONTAINER_VERTICAL;
		}
	}
	cow_container_layout_rotate(node->child[0], turns);
	cow_container_layout_rotate(node->child[1], turns);
}

bool
cow_container_layout_hidden(const struct cow_container_layout *node)
{
	return node->axis == COW_CONTAINER_LEAF ? node->hidden :
	    cow_container_layout_hidden(node->child[0]) &&
	    cow_container_layout_hidden(node->child[1]);
}

bool
cow_container_layout_minimum(struct cow_container_layout *node, int gap,
    int *width, int *height)
{
	int	aw, ah, bw, bh;
	int64_t w, h;

	if (node == NULL || gap < 0)
		return false;
	if (node->axis == COW_CONTAINER_LEAF) {
		if (node->hidden) {
			*width = *height = 0;
			return true;
		}
		if (node->min_width < 1 || node->min_height < 1)
			return false;
		*width	= node->min_width;
		*height = node->min_height;
		return true;
	}
	if ((node->axis != COW_CONTAINER_HORIZONTAL &&
		node->axis != COW_CONTAINER_VERTICAL) ||
	    node->share < 1 || node->share > 9999 ||
	    !cow_container_layout_minimum(node->child[0], gap, &aw, &ah) ||
	    !cow_container_layout_minimum(node->child[1], gap, &bw, &bh))
		return false;
	if (cow_container_layout_hidden(node->child[0]) ||
	    cow_container_layout_hidden(node->child[1])) {
		*width = node->min_width = maximum(aw, bw);
		*height = node->min_height = maximum(ah, bh);
		node->collapsed = cow_container_layout_hidden(node->child[0]) ?
		    node->child[1]->collapsed : node->child[0]->collapsed;
		return true;
	}
	if (node->axis == COW_CONTAINER_HORIZONTAL) {
		w = (int64_t)aw + bw + gap;
		h = maximum(ah, bh);
	} else {
		w = maximum(aw, bw);
		h = (int64_t)ah + bh + gap;
	}
	if (w > INT_MAX || h > INT_MAX)
		return false;
	node->min_width	 = (int)w;
	node->min_height = (int)h;
	node->collapsed	 = node->child[0]->collapsed ==
		 node->child[1]->collapsed ?
	     node->child[0]->collapsed :
	     COW_CONTAINER_LEAF;
	*width		 = (int)w;
	*height		 = (int)h;
	return true;
}

static void
arrange(struct cow_container_layout *node, struct cow_box box, int gap)
{
	int	       available, first, low, high;
	struct cow_box a = box, b = box, divider = box;

	node->box     = box;
	node->divider = (struct cow_box) { 0 };
	if (node->axis == COW_CONTAINER_LEAF)
		return;
	if (cow_container_layout_hidden(node->child[0]) ||
	    cow_container_layout_hidden(node->child[1])) {
		struct cow_box empty = { .x = box.x, .y = box.y };
		for (int i = 0; i < 2; i++)
			arrange(node->child[i],
			    cow_container_layout_hidden(node->child[i]) ? empty : box,
			    gap);
		return;
	}
	if (node->axis == COW_CONTAINER_HORIZONTAL) {
		available = box.width - gap;
		low	  = node->child[0]->min_width;
		high	  = available - node->child[1]->min_width;
	} else {
		available = box.height - gap;
		low	  = node->child[0]->min_height;
		high	  = available - node->child[1]->min_height;
	}
	first = (int)((int64_t)available * node->share / 10000);
	if (node->child[0]->collapsed == node->axis)
		first = low;
	else if (node->child[1]->collapsed == node->axis)
		first = high;
	if (first < low)
		first = low;
	if (first > high)
		first = high;
	if (node->axis == COW_CONTAINER_HORIZONTAL) {
		a.width = first;
		divider.x += first;
		divider.width = gap;
		b.x += first + gap;
		b.width = available - first;
	} else {
		a.height = first;
		divider.y += first;
		divider.height = gap;
		b.y += first + gap;
		b.height = available - first;
	}
	node->divider = divider;
	arrange(node->child[0], a, gap);
	arrange(node->child[1], b, gap);
}

bool
cow_container_layout_arrange(struct cow_container_layout *node,
    struct cow_box box, int gap)
{
	int w, h;

	/*
	 * Reject an impossible layout before changing any node geometry.
	 * The caller decides whether to grow the container or reject a resize.
	 */
	if (!cow_container_layout_minimum(node, gap, &w, &h) || box.width < w ||
	    box.height < h || (int64_t)box.x + box.width > INT_MAX ||
	    (int64_t)box.y + box.height > INT_MAX)
		return false;
	arrange(node, box, gap);
	return true;
}
