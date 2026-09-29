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
#include <stdio.h>
#include <stdlib.h>

#include "cow/view.h"
#include "cow/stack.h"
#include "cow/output.h"
#include "cow/decoration.h"
#include "cow/titlebar.h"
#include "cow/wm.h"

#define CHECK(expr)                                                        \
	do {                                                               \
		if (!(expr)) {                                             \
			fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, \
			    #expr);                                        \
			abort();                                           \
		}                                                          \
	} while (0)

int
main(void)
{
	struct cow_view a = { .use_ssd = true };
	struct cow_view b = { .use_ssd = true };
	struct cow_view d = { .use_ssd = true };

	/* Fixed metrics avoid fonts and real Wayland resources in this test. */
	a.cfg.titlebar.height = b.cfg.titlebar.height = d.cfg.titlebar.height =
	    20;
	a.content_width = b.content_width = d.content_width = 800;
	a.content_height = b.content_height = d.content_height = 600;
	a.cfg.border = b.cfg.border = d.cfg.border = 4;
	a.cfg.titlebar.enabled			   = b.cfg.titlebar.enabled =
	    d.cfg.titlebar.enabled		   = true;
	a.dec.border_width = b.dec.border_width = d.dec.border_width = 4;
	a.dec.title_thickness = b.dec.title_thickness = d.dec.title_thickness =
	    20;
	/* Squeeze holes must pass through, on every titlebar orientation. */
	a.cfg.titlebar.squeeze.mode = COW_SQUEEZE_FIXED;
	a.cfg.titlebar.squeeze.width = 100;
	const enum cow_titlebar_position positions[] = {
		COW_TITLEBAR_TOP, COW_TITLEBAR_BOTTOM,
		COW_TITLEBAR_LEFT, COW_TITLEBAR_RIGHT
	};
	for (size_t i = 0; i < sizeof(positions) / sizeof(positions[0]); i++) {
		a.cfg.titlebar.position = positions[i];
		int fw, fh, cx, cy;
		cow_titlebar_frame_geometry(&a, 800, 600, &fw, &fh, &cx, &cy);
		struct cow_box title = cow_titlebar_squeezed_rect(&a);
		int x = title.x + 10 - cx;
		int y = title.y + 10 - cy;
		CHECK(cow_view_contains_point(&a, x, y));
		if (cow_titlebar_is_vertical(positions[i]))
			y += 200;
		else
			x += 200;
		CHECK(!cow_view_contains_point(&a, x, y));
		CHECK(cow_view_contains_point(&a, 50, 50));
		CHECK(cow_view_contains_point(&a, -cx, -cy));
		CHECK(!cow_view_contains_point(&a, -cx - 1, -cy));
	}
	a.cfg.titlebar.position = COW_TITLEBAR_TOP;
	wl_list_init(&cow_wm.containers);
	wl_list_init(&cow_wm.views);
	wl_list_init(&cow_wm.seats);
	wl_list_insert(&cow_wm.views, &a.wm_link);
	wl_list_insert(&cow_wm.views, &b.wm_link);
	wl_list_insert(&cow_wm.views, &d.wm_link);
	struct cow_container *c = cow_container_create(&a, &b);
	CHECK(c->n_members == 2 && c->root->n_tabs == 2);
	CHECK(cow_view_pane(&a) == cow_view_pane(&b));
	CHECK(cow_view_is_active_tab(&a) && !cow_view_is_active_tab(&b));
	struct cow_titlebar_squeeze squeeze;
	cow_titlebar_effective_squeeze(&a, &squeeze);
	CHECK(squeeze.mode == COW_SQUEEZE_NONE);
	cow_container_activate_next(c);
	CHECK(cow_view_is_active_tab(&b) && !a.use_ssd);
	cow_container_activate_next(c);
	CHECK(cow_view_is_active_tab(&a) && !b.use_ssd);
	cow_container_activate_prev(c);
	CHECK(cow_view_is_active_tab(&b));

	cow_container_swap_prev(c);
	CHECK(c->root->tabs.next == &b.container_member->pane_link);
	CHECK(c->members.next == &b.container_member->link);
	cow_container_swap_next(c);
	CHECK(c->root->tabs.next == &a.container_member->pane_link);
	CHECK(c->members.next == &a.container_member->link);
	cow_container_activate(c, a.container_member);
	CHECK(cow_container_add(c, &d));
	CHECK(!cow_container_add(c, &d));

	/* Split a tab into an independent pane, without real River objects. */
	CHECK(cow_container_split(&a, &d, COW_CONTAINER_HORIZONTAL, false));
	CHECK(cow_container_is_split(&a));
	CHECK(cow_view_pane(&a) != cow_view_pane(&d));
	CHECK(a.pending_width < 800 && d.pending_width < 800);
	CHECK(a.pending_x < d.pending_x);
	cow_container_activate(c, d.container_member);
	CHECK(cow_view_is_active_tab(&a) && cow_view_is_active_tab(&d));
	CHECK(!cow_container_hides_view(&a));
	CHECK(!cow_container_hides_view(&d));
	CHECK(cow_container_hides_view(&b));
	d.fullscreen = true;
	CHECK(cow_container_hides_view(&a));
	CHECK(!cow_container_hides_view(&d));
	d.fullscreen = false;
	CHECK(!cow_container_hides_view(&a));

	/* Raising or lowering any pane keeps the whole group together. */
	struct cow_view dialog = { .parent = &a };
	unsigned	depth  = 0;
	CHECK(cow_container_ancestor(&dialog, &depth) == &a && depth == 1);
	CHECK(dialog.container_member == NULL);
	CHECK(!cow_container_hides_view(&dialog));
	a.flags |= COW_VIEW_ICONIFIED_FLAG;
	CHECK(cow_container_hides_view(&dialog));
	a.flags &= ~COW_VIEW_ICONIFIED_FLAG;
	dialog.needs_raise = true;
	cow_stack_apply_pending(&dialog);
	CHECK(a.raise_seq == b.raise_seq && b.raise_seq == d.raise_seq);
	CHECK(dialog.raise_seq == a.raise_seq && !dialog.needs_raise);
	dialog.parent = &dialog;
	CHECK(cow_container_ancestor(&dialog, NULL) == NULL);
	struct cow_view outsider = { .raise_seq = 20 };
	wl_list_insert(&cow_wm.views, &outsider.wm_link);
	cow_wm.raise_seq_counter = 20;
	a.needs_raise = d.needs_raise = true;
	cow_stack_apply_pending(&d);
	CHECK(a.raise_seq == b.raise_seq && b.raise_seq == d.raise_seq);
	CHECK(a.raise_seq > outsider.raise_seq && !a.needs_raise);
	uint64_t raised = a.raise_seq;
	cow_stack_apply_pending(&a);
	CHECK(a.raise_seq == raised);
	b.pending_lower = true;
	cow_stack_apply_pending(&b);
	CHECK(a.raise_seq == b.raise_seq && b.raise_seq == d.raise_seq);
	CHECK(a.raise_seq < outsider.raise_seq && !b.pending_lower);
	outsider.raise_seq = 0;
	d.pending_lower	   = true;
	cow_stack_apply_pending(&d);
	CHECK(a.raise_seq == 0 && b.raise_seq == 0 && d.raise_seq == 0);
	CHECK(outsider.raise_seq > 0);

	/* One icon represents the group; geometry and restore state survive. */
	a.cfg.icon_enabled = b.cfg.icon_enabled = d.cfg.icon_enabled = true;
	a.saved_width						     = 123;
	struct cow_box before_iconify				     = c->frame;
	int	       pane_width = a.pending_width;
	outsider.has_dimensions	  = true;
	cow_wm.focused_view	  = &d;
	cow_wm.pending_focus	  = NULL;
	cow_view_iconify(&a);
	CHECK(cow_view_is_iconified(&a) && cow_view_is_iconified(&b) &&
	    cow_view_is_iconified(&d));
	CHECK(!cow_view_has_icon(&a) && !cow_view_has_icon(&b) &&
	    cow_view_has_icon(&d));
	CHECK(cow_wm.pending_focus == &outsider);
	CHECK(a.saved_width == 123 && a.pending_width == pane_width);
	cow_view_deiconify(&b);
	CHECK(!cow_view_is_iconified(&a) && !cow_view_is_iconified(&b) &&
	    !cow_view_is_iconified(&d));
	CHECK(cow_view_is_active_tab(&b) && cow_view_is_active_tab(&d));
	CHECK(cow_wm.pending_focus == &b && a.saved_width == 123);
	CHECK(c->frame.x == before_iconify.x &&
	    c->frame.y == before_iconify.y &&
	    c->frame.width == before_iconify.width &&
	    c->frame.height == before_iconify.height);
	cow_container_activate(c, a.container_member);
	cow_wm.focused_view  = &outsider;
	cow_wm.pending_focus = NULL;
	cow_view_iconify(&d);
	CHECK(cow_wm.focused_view == &outsider && cow_wm.pending_focus == NULL);
	cow_view_deiconify(&a);
	cow_wm.focused_view  = NULL;
	cow_wm.pending_focus = NULL;
	wl_list_remove(&outsider.wm_link);
	CHECK(a.use_ssd && d.use_ssd && !b.use_ssd);
	cow_container_activate(c, b.container_member);
	CHECK(!a.use_ssd && b.use_ssd && d.use_ssd);
	cow_container_activate_next(c);
	CHECK(cow_view_is_active_tab(&a) && cow_view_is_active_tab(&d));

	/* Split rejection preserves both membership and the previous layout. */
	struct cow_container_pane *original = c->root;
	struct cow_container_pane *apane    = cow_view_pane(&a);
	int			   ax	    = a.pending_x;
	b.min_width			    = 2000;
	CHECK(!cow_container_split(&a, &b, COW_CONTAINER_VERTICAL, true));
	CHECK(c->root == original && cow_view_pane(&a) == apane);
	CHECK(cow_view_pane(&b) == apane && apane->n_tabs == 2);
	CHECK(a.pending_x == ax && cow_view_is_active_tab(&a));
	b.min_width = 0;

	/* Joining a specific pane must not depend on the group's focus. */
	struct cow_view extra	  = { .use_ssd = true,
		    .min_width		       = 10000,
		    .content_width	       = 120,
		    .content_height	       = 100 };
	extra.cfg.titlebar.height = 20;
	CHECK(!cow_container_add_to(&d, &extra));
	CHECK(extra.container_member == NULL && c->n_members == 3);
	CHECK(!extra.pending_resize);
	extra.min_width	     = 0;
	cow_wm.pending_focus = NULL;
	cow_view_iconify(&a);
	CHECK(cow_container_add_to(&d, &extra));
	CHECK(cow_view_is_iconified(&extra) && !cow_view_has_icon(&extra));
	CHECK(cow_view_pane(&extra) == cow_view_pane(&d));
	CHECK(!cow_view_is_active_tab(&extra));
	cow_container_forget_view(&extra);
	CHECK(c->n_members == 3);
	cow_view_deiconify(&a);

	/* Nested splits and movement preserve pane offsets and proportions. */
	CHECK(cow_container_split(&a, &b, COW_CONTAINER_VERTICAL, true));
	cow_titlebar_effective_squeeze(&a, &squeeze);
	CHECK(squeeze.mode == COW_SQUEEZE_FIXED);
	CHECK(b.pending_y < a.pending_y);
	struct cow_box full	      = c->frame;
	int	       initial_height = cow_view_pane(&b)->layout.box.height;
	CHECK(cow_container_collapse(&b));
	CHECK(cow_view_pane(&b)->layout.box.height < initial_height);
	CHECK(b.clip_w == 1 && b.clip_h == 1);
	CHECK(cow_container_collapse(&b));
	CHECK(cow_view_pane(&b)->layout.box.height == initial_height);
	CHECK(cow_container_neighbor(&a, 8) == &d);
	unsigned       share	= c->root->layout.share;
	struct cow_box pane_box = cow_view_pane(&a)->layout.box;
	int	       divider_ids[2];
	unsigned       divider_shares[2];
	cow_container_resize_save(&a, 8, divider_ids, divider_shares);
	CHECK(cow_container_resize(&a, 8, full, pane_box, 40, 0));
	CHECK(c->frame.width == full.width && c->root->layout.share > share);
	CHECK(cow_view_pane(&d)->layout.box.x > pane_box.x + pane_box.width);
	CHECK(cow_container_resize_restore(&a, 8, full, divider_ids,
	    divider_shares));
	CHECK(c->root->layout.share == share);
	divider_ids[0]++;
	CHECK(!cow_container_resize_restore(&a, 8, full, divider_ids,
	    divider_shares));
	CHECK(c->root->layout.share == share);
	pane_box = cow_view_pane(&b)->layout.box;
	CHECK(cow_container_resize(&b, 4, full, pane_box, -40, 0));
	CHECK(c->frame.x == full.x - 40 && c->frame.width == full.width + 40);
	CHECK(cow_container_arrange(c, full));
	struct cow_box small = full;
	small.width = small.height = 1;
	CHECK(!cow_container_arrange(c, small));
	CHECK(c->frame.width == full.width && c->frame.height == full.height);
	/* Changed hints grow the group without replacing split proportions. */
	b.min_width = full.width;
	CHECK(cow_container_refresh(&b));
	CHECK(c->frame.width > full.width && b.pending_width >= b.min_width);
	CHECK(c->root->layout.share == share);
	int grown_width = c->frame.width;
	b.min_width	= 0;
	CHECK(cow_container_refresh(&b));
	CHECK(c->frame.width == grown_width);
	CHECK(cow_container_arrange(c, full));
	struct cow_output output = {
		.width	     = 1920,
		.height	     = 1080,
		.usable_area = { 0, 30, 1920, 1050 }
	};
	a.output = b.output = d.output = &output;
	CHECK(cow_container_maximize(&a, true, false, false, false, false));
	CHECK(c->frame.width == 1920 && c->frame.height == full.height);
	CHECK(a.maximized_h && b.maximized_h && d.maximized_h);
	CHECK(cow_container_maximize(&d, false, true, false, false, false));
	CHECK(c->frame.y == 30 && c->frame.height == 1050);
	CHECK(cow_container_maximize(&b, true, true, true, false, false));
	CHECK(c->frame.x == full.x && c->frame.y == full.y &&
	    c->frame.width == full.width && c->frame.height == full.height);
	CHECK(!a.maximized && !b.maximized && !d.maximized);
	cow_view_fullscreen(&a);
	CHECK(a.fullscreen && a.pending_width == 1920 &&
	    a.pending_height == 1080);
	CHECK(a.clip_w == 0 && a.clip_h == 0);
	cow_view_exit_fullscreen(&a);
	CHECK(!a.fullscreen && c->frame.width == full.width &&
	    c->frame.height == full.height);
	CHECK(a.pending_width < full.width && a.clip_w == a.pending_width);
	cow_view_fullscreen(&a);
	cow_container_activate(c, d.container_member);
	CHECK(!a.fullscreen && cow_view_is_active_tab(&d));
	a.flags |= COW_VIEW_STICKY_DESK_FLAG | COW_VIEW_STICKY_PAGE_FLAG;
	cow_container_sync_flags(&a,
	    COW_VIEW_STICKY_DESK_FLAG | COW_VIEW_STICKY_PAGE_FLAG);
	CHECK(a.flags == b.flags && b.flags == d.flags);
	a.flags &= ~(COW_VIEW_STICKY_DESK_FLAG | COW_VIEW_STICKY_PAGE_FLAG);
	cow_container_sync_flags(&a,
	    COW_VIEW_STICKY_DESK_FLAG | COW_VIEW_STICKY_PAGE_FLAG);
	CHECK(a.flags == b.flags && b.flags == d.flags);
	/* Viewport translation must not be undone by a later pane operation. */
	a.x = a.pending_x - 1920;
	a.y = a.pending_y;
	cow_container_sync_frame(&a);
	CHECK(c->frame.x == full.x - 1920);
	CHECK(cow_container_collapse(&b));
	CHECK(c->frame.x == full.x - 1920);
	CHECK(cow_container_collapse(&b));
	CHECK(cow_container_arrange(c, full));
	cow_view_clear_expanded_state(&a);
	CHECK(a.pending_tiled_edges_value != 0);

	/* Output ownership must not depend on which pane a command targets. */
	struct cow_output right = {
		.x = full.x + full.width / 2,
		.y = full.y,
		.width = full.width,
		.height = full.height
	};
	output.x = full.x;
	output.y = full.y;
	output.width = full.width / 2;
	output.height = full.height;
	wl_list_init(&cow_wm.outputs);
	wl_list_insert(&cow_wm.outputs, &output.wm_link);
	wl_list_insert(&cow_wm.outputs, &right.wm_link);
	d.x = d.pending_x;
	d.y = d.pending_y;
	cow_view_update_output(&d);
	CHECK(a.output == &output && b.output == &output && d.output == &output);
	a.orphan_desk_nr = b.orphan_desk_nr = d.orphan_desk_nr = 2;
	cow_view_clear_orphan(&d);
	CHECK(a.orphan_desk_nr == -1 && b.orphan_desk_nr == -1 &&
	    d.orphan_desk_nr == -1);
	wl_list_remove(&right.wm_link);
	wl_list_remove(&output.wm_link);
	a.output = b.output = d.output = NULL;
	int dx			       = d.pending_x - a.pending_x;
	int dy			       = b.pending_y - a.pending_y;
	a.x			       = a.pending_x + 17;
	a.y			       = a.pending_y - 9;
	cow_view_sync_container_pos(&a);
	CHECK(d.x - a.x == dx && b.y - a.y == dy);
	CHECK(a.pending_x == a.x && b.pending_y == b.y);
	a.pending_resize = b.pending_resize = d.pending_resize = false;
	a.x += 5;
	cow_view_sync_container_pos(&a);
	CHECK(!a.pending_resize && !b.pending_resize && !d.pending_resize);

	/* Removing a whole pane promotes its sibling without a dangling node.
	 */
	cow_container_forget_view(&b);
	CHECK(cow_view_pane(&a)->layout.parent == &c->root->layout);
	CHECK(c->n_members == 2 && cow_view_is_active_tab(&d));
	cow_container_remove_view(&d);
	CHECK(a.container_member == NULL && d.container_member == NULL);
	CHECK(a.pending_width == 800 && a.pending_height == 600);
	CHECK(d.pending_width == d.content_width);
	CHECK(wl_list_empty(&cow_wm.containers));

	/* Destruction also releases an intact split tree. */
	c = cow_container_create(&a, &b);
	CHECK(cow_container_split(&a, &b, COW_CONTAINER_HORIZONTAL, true));
	cow_container_destroy(c);
	CHECK(a.container_member == NULL && b.container_member == NULL &&
	    d.container_member == NULL && wl_list_empty(&cow_wm.containers));

	c = cow_container_create(&a, &b);
	CHECK(cow_container_add(c, &d));
	cow_container_forget_view(&a);
	CHECK(c->n_members == 2 && c->root->n_tabs == 2);
	CHECK(c->root->active == b.container_member && b.use_ssd);
	cow_container_remove_view(&b);
	CHECK(b.container_member == NULL && d.container_member == NULL);
	CHECK(wl_list_empty(&cow_wm.containers));

	c = cow_container_create(&a, &b);
	cow_container_forget_view(&b);
	CHECK(c->n_members == 1 && c->root->n_tabs == 1);
	cow_container_forget_view(&a);
	CHECK(wl_list_empty(&cow_wm.containers));
	return 0;
}
