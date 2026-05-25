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

#if !defined(COW_STACK_H)
#define COW_STACK_H

#include <stdint.h>

struct cow_view;

/*
 * Stacking model.
 *
 * The global stack is a partial order keyed by (layer, raise_seq).
 * cow_view_set_layer() and the raise/lower paths only update those
 * fields and mark the stack dirty; the actual river_node_v1
 * reordering is performed in one pass by cow_stack_apply().
 *
 * Iconified views' icon_nodes are pinned to layer 0 (fvwm-classic),
 * independently of their parent view's layer.
 *
 * Output backgrounds (placed at river-level place_bottom at output-
 * create time) sit below every layered view; this module does not
 * touch them.
 */

/*
 * Mark the stack as dirty.  Cheap; coalesced.  Subsequent calls
 * before the next cow_stack_apply() are no-ops.
 */
void cow_stack_mark_dirty(void);

/*
 * If the stack is dirty, walk cow_wm.views, sort by
 * (effective_layer ASC, raise_seq ASC), and emit river_node ordering
 * requests to bring the compositor's stack in line.  Iconified views'
 * icon_nodes are written first (lowest), pinned to layer 0.
 *
 * Called from the tail of cow_wm_manage_dirty_now().
 */
void cow_stack_apply(void);

/*
 * Return the smallest raise_seq among non-closed views that share
 * `layer`.  Used by cow_view_lower() to push the target one step
 * below the layer's current floor.  When no other view occupies that
 * layer the function returns 0.
 */
uint64_t cow_stack_min_raise_seq_in_layer(uint8_t layer);

#endif /* COW_STACK_H */
