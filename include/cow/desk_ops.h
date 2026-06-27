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
#if !defined(COW_DESK_OPS_H)
#define COW_DESK_OPS_H

#include <stdbool.h>
#include <stdint.h>

struct cow_output;
struct cow_desk;
struct cow_view;

/* Switch the output to a desk. */
void cow_desk_switch(struct cow_output *, uint8_t);

/* Assign a view to a desk. */
void cow_view_assign_desk(struct cow_view *, uint8_t);

/* Switch the output to page (col, row). */
void cow_page_switch(struct cow_output *, int, int);

int cow_page_col(const struct cow_output *);
int cow_page_row(const struct cow_output *);

/* Move a view to page (col, row). */
void cow_view_move_to_page(struct cow_view *, int, int);

/* Pan the current desk viewport to a virtual pixel offset. */
void cow_page_scroll_to(struct cow_output *, int, int, bool);

/* Clamp view/page offsets after desktop_size shrinks. */
void cow_page_clamp_views(struct cow_output *);

/* Advance one output's page-scroll animation. */
bool cow_page_scroll_tick(struct cow_output *);

#endif
