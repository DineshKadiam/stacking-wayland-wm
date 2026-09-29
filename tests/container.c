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
	wl_list_init(&cow_wm.containers);
	struct cow_container *c = cow_container_create(&a, &b);
	CHECK(c->n_members == 2 && c->root->n_tabs == 2);
	CHECK(cow_view_pane(&a) == cow_view_pane(&b));
	CHECK(cow_view_is_active_tab(&a) && !cow_view_is_active_tab(&b));
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
	extra.min_width = 0;
	CHECK(cow_container_add_to(&d, &extra));
	CHECK(cow_view_pane(&extra) == cow_view_pane(&d));
	CHECK(!cow_view_is_active_tab(&extra));
	cow_container_forget_view(&extra);
	CHECK(c->n_members == 3);

	/* Nested splits and movement preserve pane offsets and proportions. */
	CHECK(cow_container_split(&a, &b, COW_CONTAINER_VERTICAL, true));
	CHECK(b.pending_y < a.pending_y);
	struct cow_box full = c->frame;
	struct cow_box small = full;
	small.width = small.height = 1;
	CHECK(!cow_container_arrange(c, small));
	CHECK(c->frame.width == full.width && c->frame.height == full.height);
	int dx = d.pending_x - a.pending_x;
	int dy = b.pending_y - a.pending_y;
	a.x    = a.pending_x + 17;
	a.y    = a.pending_y - 9;
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
