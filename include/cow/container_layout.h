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
#if !defined(COW_CONTAINER_LAYOUT_H)
#define COW_CONTAINER_LAYOUT_H

#include <stdbool.h>
#include "cow/cow_box.h"

enum cow_container_axis {
	COW_CONTAINER_LEAF,
	COW_CONTAINER_HORIZONTAL,
	COW_CONTAINER_VERTICAL,
};

/*
 * The owner manages node lifetime. Leaves supply minimum frame dimensions;
 * branches supply two children and the first child's share (1..9999).
 * Measurement refreshes branch minima and collapse state bottom-up.
 * Layout never changes shares, so shrinking and growing is reversible.
 */
struct cow_container_layout {
	enum cow_container_axis	     axis;
	struct cow_container_layout *parent, *child[2];
	unsigned		     share;
	enum cow_container_axis	     collapsed;
	int			     min_width, min_height;
	struct cow_box		     box, divider;
};

bool cow_container_layout_minimum(struct cow_container_layout *, int, int *,
    int *);
bool cow_container_layout_arrange(struct cow_container_layout *, struct cow_box,
    int);

#endif
