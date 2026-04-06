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

#include <stdint.h>

struct cow_output;
struct cow_desk;
struct cow_view;

/*
 * Switch the current output to display the given desk number (0-9).
 *
 * Views on the current desk are hidden; views on the target desk
 * (and views with the STICKY flag) are shown.
 */
void cow_desk_switch(struct cow_output *output, uint8_t desk_nr);

/*
 * Assign the focused view to the given desk number.
 * The view is moved from its current desk to the target desk.
 * If the target desk is not the current desk, the view is hidden.
 */
void cow_view_assign_desk(struct cow_view *view, uint8_t desk_nr);

/*
 * Check if a view should be visible on the given output's current desk.
 */
int cow_view_visible_on_current(struct cow_view *view,
    struct cow_output				*output);

#endif
