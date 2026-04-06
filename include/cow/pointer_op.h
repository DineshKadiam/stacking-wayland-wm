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
#if !defined(COW_POINTER_OP_H)
#define COW_POINTER_OP_H

#include <stdbool.h>
#include <stdint.h>

struct cow_seat;
struct cow_view;

/*
 * Type of interactive pointer operation.
 */
enum cow_pointer_op_type {
	COW_POINTER_OP_NONE,
	COW_POINTER_OP_MOVE,
	COW_POINTER_OP_RESIZE,
};

/*
 * Resize edge flags (matching river's river_window_v1.edges enum).
 */
#define COW_EDGE_TOP 1
#define COW_EDGE_BOTTOM 2
#define COW_EDGE_LEFT 4
#define COW_EDGE_RIGHT 8

/*
 * State for an in-progress pointer operation.
 */
struct cow_pointer_op {
	enum cow_pointer_op_type type;
	struct cow_view		*view;

	/* Initial position of the view when the op started */
	int start_x, start_y;

	/* Initial dimensions when the op started */
	int start_width, start_height;

	/* For resize: which edges are being dragged */
	uint32_t resize_edges;
};

/*
 * Begin an interactive move of the given view.
 * Must be called during a manage sequence.
 */
void cow_pointer_op_start_move(struct cow_seat *seat, struct cow_view *view);

/*
 * Begin an interactive resize of the given view.
 * Must be called during a manage sequence.
 */
void cow_pointer_op_start_resize(struct cow_seat *seat, struct cow_view *view,
    uint32_t edges);

/*
 * Process pointer operation state during a manage sequence.
 * Applies op_delta to the view's position/dimensions.
 */
void cow_pointer_op_process(struct cow_seat *seat);

#endif
