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
#include <stdlib.h>

#include "cow/focus-history.h"
#include "cow/memory.h"
#include "cow/seat.h"
#include "cow/view.h"
#include "cow/wm.h"
#include "river-window-management-v1-client-protocol.h"

/* Focus is WM-wide. Only one seat can own its circulation snapshot. */
static uint64_t sequence;
static struct cow_seat *cycle_seat;
static uint32_t cycle_modifiers;
static struct cow_view *cycle_target;
struct entry {
	struct cow_view *view;
	size_t order;
};
static struct cow_view **entries;
static size_t count;

void
cow_focus_cycle_end(void)
{
	if (entries == NULL)
		return;
	struct cow_view *view = cow_wm_effective_focus();
	if (view != NULL && !view->closed)
		view->focus_seq = ++sequence;
	xfree(entries);
	entries = NULL;
	count = 0;
	cycle_seat = NULL;
	cycle_target = NULL;
	cycle_modifiers = 0;
}

void
cow_focus_history_changed(struct cow_view *view)
{
	/* Following a target across desks may clear focus before applying it. */
	if (entries != NULL && (view == cycle_target ||
	    (view == NULL && cycle_target != NULL &&
	    cow_wm.pending_focus == cycle_target)))
		return;
	cow_focus_cycle_end();
	if (view != NULL)
		view->focus_seq = ++sequence;
}

void
cow_focus_history_forget(struct cow_view *view)
{
	for (size_t i = 0; i < count; i++) {
		if (entries[i] == view)
			entries[i] = NULL;
	}
	if (cycle_target == view)
		cycle_target = NULL;
}

void
cow_focus_cycle_modifiers(struct cow_seat *seat, uint32_t modifiers)
{
	if (seat == cycle_seat && (modifiers & cycle_modifiers) == 0)
		cow_focus_cycle_end();
}

static int
compare_entries(const void *a, const void *b)
{
	const struct entry *left = a, *right = b;
	if (left->view->focus_seq != right->view->focus_seq)
		return left->view->focus_seq > right->view->focus_seq ? -1 : 1;
	return (left->order > right->order) - (left->order < right->order);
}

struct cow_view **
cow_focus_mru_snapshot(size_t *n)
{
	size_t capacity = wl_list_length(&cow_wm.views);
	struct entry *sorted = xcalloc(capacity + 1, sizeof(*sorted));
	*n = 0;
	struct cow_view *view;
	wl_list_for_each(view, &cow_wm.views, wm_link) {
		if (!view->closed) {
			sorted[*n] = (struct entry){ view, *n };
			(*n)++;
		}
	}
	qsort(sorted, *n, sizeof(*sorted), compare_entries);
	struct cow_view **views = xcalloc(*n + 1, sizeof(*views));
	for (size_t i = 0; i < *n; i++)
		views[i] = sorted[i].view;
	xfree(sorted);
	return views;
}

struct cow_view *
cow_focus_mru_next(bool forward, bool (*eligible)(const struct cow_view *))
{
	struct cow_seat *seat = cow_wm.current_interaction_seat;
	uint32_t modifiers = seat != NULL && cow_wm.xkb_bindings_version >= 3 ?
	    seat->binding_action_modifiers & seat->active_modifiers : 0;
	/* Shift reverses circulation; releasing it must not commit Alt-Tab. */
	if (modifiers & ~RIVER_SEAT_V1_MODIFIERS_SHIFT)
		modifiers &= ~RIVER_SEAT_V1_MODIFIERS_SHIFT;
	if (entries != NULL && (seat != cycle_seat ||
	    (modifiers & cycle_modifiers) == 0))
		cow_focus_cycle_end();
	struct cow_view *focused = cow_wm_effective_focus();
	if (entries == NULL) {
		size_t n;
		entries = cow_focus_mru_snapshot(&n);
		for (size_t i = 0; i < n; i++) {
			if (eligible(entries[i]))
				entries[count++] = entries[i];
		}
		cycle_seat = seat;
		cycle_modifiers = modifiers;
	}
	if (count == 0) {
		cow_focus_cycle_end();
		return NULL;
	}
	/* Start at the appropriate end when focus is outside the filter. */
	size_t index = forward ? count - 1 : 0;
	for (size_t i = 0; i < count; i++) {
		if (entries[i] == focused) {
			index = i;
			break;
		}
	}
	struct cow_view *next = NULL;
	for (size_t i = 0; i < count; i++) {
		index = forward ? (index + 1) % count : (index + count - 1) % count;
		struct cow_view *view = entries[index];
		if (view != NULL && eligible(view)) {
			next = view;
			break;
		}
	}
	cycle_target = next;
	if (modifiers == 0 || next == NULL)
		cow_focus_cycle_end();
	return next;
}
