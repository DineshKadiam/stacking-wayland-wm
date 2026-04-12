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

/*
 * Switch the current page on the given output to (col, row).
 * col must be in [0, page_cols-1]; row in [0, page_rows-1].
 * Clamped silently if out of range.
 * Triggers a manage cycle; smooth animation is applied if configured.
 */
void cow_page_switch(struct cow_output *output, int col, int row);

/*
 * Return the current page column/row for the given output's current desk.
 */
int cow_page_col(const struct cow_output *output);
int cow_page_row(const struct cow_output *output);

/*
 * Move view->vx/vy so the window lives on page (col, row) of its output,
 * preserving its within-page position.
 * Triggers a manage cycle and re-renders the view.
 */
void cow_view_move_to_page(struct cow_view *view, int col, int row);

/*
 * Clamp any views whose vx/vy fall outside the current page grid.
 * Called after desktop_size is reduced.
 */
void cow_page_clamp_views(struct cow_output *output);

/*
 * Advance the smooth page scroll animation for one output.
 * Returns true while the animation is in progress; the caller should
 * keep the manage cycle running until this returns false.
 */
bool cow_page_scroll_tick(struct cow_output *output);

#endif
