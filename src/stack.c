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

/*
 * Global stacking pass.
 *
 * Everything that touches z-order ultimately sets a "dirty" flag here.
 * During each render sequence we
 * gather every non-closed view, sort by (effective_layer, raise_seq),
 * and walk the sorted list from bottom to top issuing
 * river_node_v1_place_above(prev) chains.  Output backgrounds are
 * layer-shell background surfaces and are therefore outside this River
 * node chain.
 *
 * Iconified views' icon_nodes are pinned to layer 0 (fvwm-classic).
 * They are placed BEFORE any non-iconified view, regardless of the
 * parent view's `layer` field.  Note: a hidden/iconified view's main
 * `node` is not ordered against the rest, since it's parked
 * off-screen.
 */

#include <stdlib.h>
#include <wayland-client.h>

#include "cow/memory.h"
#include "cow/stack.h"
#include "cow/view.h"
#include "cow/wm.h"

#include "river-window-management-v1-client-protocol.h"

static bool stack_dirty;

void
cow_stack_mark_dirty(void)
{
	stack_dirty = true;
}

struct sort_entry {
	struct cow_view *view;
	uint8_t		 layer; /* effective layer */
	uint64_t	 raise_seq;
};

static int
sort_cmp(const void *a_, const void *b_)
{
	const struct sort_entry *a = a_;
	const struct sort_entry *b = b_;

	if (a->layer != b->layer)
		return (int)a->layer - (int)b->layer;
	if (a->raise_seq < b->raise_seq)
		return -1;
	if (a->raise_seq > b->raise_seq)
		return 1;
	return 0;
}

void
cow_stack_apply(void)
{
	if (!stack_dirty)
		return;
	stack_dirty = false;

	/*
	 * Count candidates (non-closed views with a node) plus icon
	 * candidates (iconified views with an icon_node).  Two passes:
	 * one to size the array, one to populate.
	 */
	size_t		 view_count = 0;
	size_t		 icon_count = 0;
	struct cow_view *v;
	wl_list_for_each(v, &cow_wm.views, wm_link)
	{
		if (v->closed)
			continue;
		if (v->node != NULL && !cow_view_is_iconified(v))
			view_count++;
		if (v->icon_node != NULL && cow_view_is_iconified(v))
			icon_count++;
	}

	if (view_count == 0 && icon_count == 0)
		return;

	/* Build the sorted entries for non-iconified views. */
	struct sort_entry *entries = NULL;
	if (view_count > 0) {
		entries	 = xcalloc(view_count, sizeof(*entries));
		size_t i = 0;
		wl_list_for_each(v, &cow_wm.views, wm_link)
		{
			if (v->closed || v->node == NULL ||
			    cow_view_is_iconified(v))
				continue;
			entries[i].view	     = v;
			entries[i].layer     = cow_view_effective_layer(v);
			entries[i].raise_seq = v->raise_seq;
			i++;
		}
		qsort(entries, view_count, sizeof(*entries), sort_cmp);
	}

	/*
	 * Build the icon ordering -- icon_nodes form a contiguous block
	 * below all layered nodes, sorted by raise_seq among themselves
	 * so the most-recently-raised iconified window's icon sits on
	 * top of other icons.
	 */
	struct sort_entry *icon_entries = NULL;
	if (icon_count > 0) {
		icon_entries = xcalloc(icon_count, sizeof(*icon_entries));
		size_t i     = 0;
		wl_list_for_each(v, &cow_wm.views, wm_link)
		{
			if (v->closed || v->icon_node == NULL ||
			    !cow_view_is_iconified(v))
				continue;
			icon_entries[i].view	  = v;
			icon_entries[i].layer	  = 0;
			icon_entries[i].raise_seq = v->raise_seq;
			i++;
		}
		qsort(icon_entries, icon_count, sizeof(*icon_entries),
		    sort_cmp);
	}

	/*
	 * Emit the chain bottom-to-top.
	 *
	 * The first node uses place_top so the chain anchors above any
	 * unmanaged River shell surfaces.  Using place_bottom here would
	 * risk pushing the entire view chain below surfaces outside the
	 * managed stack.
	 *
	 * Each subsequent node is positioned immediately above its
	 * predecessor with place_above.  The final managed-node result is,
	 * from bottom to top: icons (layer 0), then views sorted by
	 * (layer, raise_seq).
	 */
	struct river_node_v1 *prev = NULL;
	for (size_t i = 0; i < icon_count; i++) {
		struct river_node_v1 *n = icon_entries[i].view->icon_node;
		if (prev == NULL)
			river_node_v1_place_top(n);
		else
			river_node_v1_place_above(n, prev);
		prev = n;
	}
	for (size_t i = 0; i < view_count; i++) {
		struct river_node_v1 *n = entries[i].view->node;
		if (prev == NULL)
			river_node_v1_place_top(n);
		else
			river_node_v1_place_above(n, prev);
		prev = n;
	}

	xfree(entries);
	xfree(icon_entries);
}

uint64_t
cow_stack_min_raise_seq_in_layer(uint8_t layer)
{
	uint64_t	 min   = 0;
	bool		 found = false;
	struct cow_view *v;
	wl_list_for_each(v, &cow_wm.views, wm_link)
	{
		if (v->closed)
			continue;
		if (cow_view_effective_layer(v) != layer)
			continue;
		if (!found || v->raise_seq < min) {
			min   = v->raise_seq;
			found = true;
		}
	}
	return found ? min : 0;
}
