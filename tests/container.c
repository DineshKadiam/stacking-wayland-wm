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
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "cow/view.h"
#include "cow/stack.h"
#include "cow/output.h"
#include "cow/decoration.h"
#include "cow/border.h"
#include "cow/titlebar.h"
#include "cow/wm.h"
#include "state-snapshot.h"
#include "cow/cmd.h"
#include "cow/memory.h"
#include "cow/seat.h"
#include "cow/pointer_op.h"
#include "river-window-management-v1-client-protocol.h"

#define CHECK(expr)                                                        \
	do {                                                               \
		if (!(expr)) {                                             \
			fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, \
			    #expr);                                        \
			abort();                                           \
		}                                                          \
	} while (0)

static struct cow_container *
pending_split(struct cow_view *view)
{
	struct cow_container *c;
	wl_list_for_each(c, &cow_wm.containers, wm_link)
	{
		if (c->split_target == view)
			return c;
	}
	return NULL;
}

static void
check_command_result(const char *text, bool success)
{
	struct cmd_list *list  = NULL;
	char		*cause = NULL;
	CHECK(cmd_string_parse(text, &list, NULL, 0, &cause) == 0);
	struct cmd_q *queue = cmdq_new();
	queue->client_fd    = -1;
	cmdq_run(queue, list);
	if (queue->failed && success)
		fprintf(stderr, "command failed: %s\n", text);
	for (u_int i = 0; i < ARRAY_LENGTH(&queue->causes); i++)
		fprintf(stderr, "%s\n", ARRAY_ITEM(&queue->causes, i));
	CHECK(!queue->failed == success);
	cmdq_free(queue);
	cmd_list_free(list);
	xfree(cause);
}

static void
check_command(const char *text)
{
	check_command_result(text, true);
}

static void
check_return_joins(void)
{
	uint32_t straight[24 * 24] = {0}, joined[24 * 24];
	/* A north-east inside elbow, mirrored for the other orientations. */
	const bool horizontal[4][4] = {
		{false, false, false, true},
		{false, false, true, true},
		{false, true, true, true},
		{true, true, true, true},
	};
	for (int i = 4; i < 20; i++) {
		for (int d = 0; d < 4; d++) {
			straight[d * 24 + i] = 1;
			straight[(20 + d) * 24 + i] = 2;
			straight[i * 24 + d] = 3;
			straight[i * 24 + 20 + d] = 4;
		}
	}
	for (int right = 0; right < 2; right++) {
		for (int bottom = 0; bottom < 2; bottom++) {
			memset(joined, 0xff, sizeof(joined));
			cow_border_join_return(joined, straight, 24, 24, 24,
			    8, 8, 4, right, bottom);
			for (int y = 0; y < 4; y++) {
				for (int x = 0; x < 4; x++) {
					bool h = horizontal[bottom ? 3 - y : y]
					    [right ? x : 3 - x];
					CHECK(joined[(8 + y) * 24 + 8 + x] ==
					    (uint32_t)(h ? (bottom ? 2 : 1) :
					    (right ? 4 : 3)));
				}
			}
			CHECK(joined[0] == UINT32_MAX);
		}
	}
}

int
main(void)
{
	check_return_joins();
	/* Internal joins are straight, with handles only at outer corners. */
	uint32_t framed[80 * 60], plain[80 * 60], pane_pixels[80 * 60];
	struct cow_border_colour colours = { 0 };
	float *sides[] = { colours.n, colours.s, colours.w, colours.e };
	for (size_t i = 0; i < sizeof(sides) / sizeof(sides[0]); i++) {
		sides[i][0] = sides[i][1] = sides[i][2] = 0.5f;
		sides[i][3]				= 1.0f;
	}
	for (int style = 0; style <= 1; style++) {
		cow_border_style_render(framed, 80, 80, 60, &colours, 8, 20,
		    style);
		cow_border_style_render(plain, 80, 80, 60, &colours, 8, 0,
		    style);
		cow_border_style_render_pane(pane_pixels, 80, 80, 60, &colours,
		    8, 20, style, 5);
		for (int y = 0; y < 60; y++) {
			for (int x = 0; x < 80; x++) {
				if ((x >= 72 && y >= 8 && y < 52) ||
				    (y >= 52 && x >= 8 && x < 72))
					CHECK(pane_pixels[y * 80 + x] ==
					    plain[y * 80 + x]);
				if (x < 30 && y < 30)
					CHECK(pane_pixels[y * 80 + x] ==
					    framed[y * 80 + x]);
			}
		}
	}
	struct cow_view a = { .use_ssd = true };
	struct cow_view b = { .use_ssd = true };
	struct cow_view d = { .use_ssd = true };

	/* Fixed metrics avoid fonts and real Wayland resources in this test. */
	struct cow_view *fixtures[] = { &a, &b, &d };
	for (size_t i = 0; i < 3; i++) {
		struct cow_view *v = fixtures[i];
		v->content_width = 800;
		v->content_height = 600;
		v->cfg.border = v->dec.border_width = 4;
		v->cfg.titlebar.enabled = true;
		v->cfg.titlebar.height = v->dec.title_thickness =
		    v->dec.nominal_title_thickness = 20;
	}
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
	/* Squeezed body borders no longer own diagonal resize handles. */
	a.cfg.corner_length = 24;
	const uint32_t corner_edges[] = {
		COW_EDGE_TOP,
		COW_EDGE_BOTTOM,
		COW_EDGE_BOTTOM,
		COW_EDGE_BOTTOM,
	};
	for (size_t i = 0; i < sizeof(positions) / sizeof(positions[0]); i++) {
		a.cfg.titlebar.position = positions[i];
		struct cow_box body, title;
		cow_decoration_frame_rects(&a, 800, 600, &body, &title);
		int x = positions[i] == COW_TITLEBAR_LEFT ? body.x + 2 :
		    body.x + body.width - 2;
		int y = positions[i] == COW_TITLEBAR_TOP ? body.y + 2 :
		    body.y + body.height - 2;
		CHECK(cow_decoration_edges_at(&a, x, y, 800, 600) ==
		    corner_edges[i]);
	}
	a.cfg.titlebar.position = COW_TITLEBAR_TOP;
	/* The relocated titlebar end is the new NE handle, including
	 * its vertical return. Transparent space above the body stays inert.
	 */
	struct cow_box title = cow_titlebar_squeezed_rect(&a);
	CHECK(cow_decoration_edges_at(&a, title.x + title.width + 2,
	    title.y + 10, 800, 600) == (COW_EDGE_TOP | COW_EDGE_RIGHT));
	CHECK(cow_decoration_edges_at(&a, 500, 2, 800, 600) == 0);
	a.cfg.corner_length = 0;
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
	struct cow_titlebar_config tabbed_tb;
	cow_titlebar_effective_config(&a, &tabbed_tb);
	CHECK(tabbed_tb.buttons[0].vector.num_points ==
	    a.cfg.titlebar.buttons[0].vector.num_points);
	CHECK(tabbed_tb.buttons[1].vector.num_points ==
	    a.cfg.titlebar.buttons[1].vector.num_points);
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
	a.cfg.handles = true;
	a.cfg.corner_length = 20;
	CHECK(cow_decoration_corner_length(&a) > 0);
	CHECK(cow_container_split(&a, &d, COW_CONTAINER_HORIZONTAL, false));
	CHECK(cow_container_is_split(&a));
	CHECK(cow_decoration_corner_length(&a) == 0);
	CHECK(cow_view_pane(&a) != cow_view_pane(&d));
	CHECK(a.pending_width < 800 && d.pending_width < 800);
	CHECK(a.pending_x < d.pending_x);
	CHECK(cow_container_shared_edges(&a) == 0);
	CHECK(cow_container_shared_edges(&d) == 4);
	CHECK(d.pending_x - (a.pending_x + a.pending_width) == a.cfg.border);
	CHECK(cow_decoration_edges_at(&d, 0, 100, d.pending_width,
		  d.pending_height) == 0);
	/* Each painted frame still fills its pane, for all titlebar sides. */
	for (size_t i = 0; i < sizeof(positions) / sizeof(positions[0]); i++) {
		a.cfg.titlebar.position = d.cfg.titlebar.position =
		    positions[i];
		CHECK(cow_container_arrange(c, c->frame));
		struct cow_view *views[] = { &a, &d };
		for (size_t j = 0; j < 2; j++) {
			struct cow_view *v = views[j];
			int		 fw, fh, cx, cy;
			cow_titlebar_frame_geometry(v, v->pending_width,
			    v->pending_height, &fw, &fh, &cx, &cy);
			struct cow_box box  = cow_view_pane(v)->layout.box;
			int	       trim = j == 1 ? v->dec.border_width : 0;
			CHECK(v->pending_x - cx + trim == box.x);
			CHECK(v->pending_y - cy == box.y);
			CHECK(fw - trim == box.width && fh == box.height);
		}
	}
	a.cfg.titlebar.position = d.cfg.titlebar.position = COW_TITLEBAR_TOP;
	CHECK(cow_container_arrange(c, c->frame));
	cow_container_activate(c, d.container_member);
	CHECK(cow_view_is_active_tab(&a) && cow_view_is_active_tab(&d));
	CHECK(!cow_container_hides_view(&a));
	CHECK(!cow_container_hides_view(&d));
	CHECK(cow_container_hides_view(&b));
	d.fullscreen = true;
	CHECK(cow_container_shared_edges(&d) == 0);
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
	int  old_title_thickness	    = b.dec.title_thickness;
	bool old_title_enabled		    = b.cfg.titlebar.enabled;
	b.cfg.titlebar.enabled		    = false;
	CHECK(!cow_container_split(&a, &b, COW_CONTAINER_VERTICAL, true));
	CHECK(b.dec.title_thickness == old_title_thickness);
	b.cfg.titlebar.enabled = old_title_enabled;
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
	/* Row collapse reallocates space within its column. */
	struct cow_box unshaded_frame = c->frame;
	int	       a_height	      = a.pending_height;
	CHECK(cow_container_collapse(&b));
	CHECK(b.sd == COW_SHADE_NONE);
	CHECK(cow_view_pane(&b)->layout.collapsed == COW_CONTAINER_VERTICAL);
	CHECK(a.pending_height > a_height);
	CHECK(c->frame.x == unshaded_frame.x && c->frame.y == unshaded_frame.y);
	/* The last expanded pane must not collapse into an empty region. */
	CHECK(!cow_container_collapse(&a));
	CHECK(cow_view_pane(&a)->layout.collapsed == COW_CONTAINER_LEAF);
	CHECK(cow_container_collapse(&b));
	CHECK(cow_view_pane(&b)->layout.collapsed == COW_CONTAINER_LEAF);
	CHECK(a.pending_height == a_height);
	/* A horizontal titlebar cannot collapse a side-by-side split. */
	CHECK(!cow_container_collapse(&d));
	CHECK(d.sd == COW_SHADE_NONE);
	CHECK(cow_view_pane(&d)->layout.collapsed == COW_CONTAINER_LEAF);
	/* Whole-container shading keeps the saved row layout untouched. */
	cow_view_shade(&a, COW_SHADE_NORTH);
	CHECK(c->shaded);
	int shaded_w, shaded_h;
	cow_view_effective_content_size(&d, &shaded_w, &shaded_h);
	CHECK(shaded_w > 0 && shaded_h == 0);
	CHECK(a.pending_y == b.pending_y && b.pending_y == d.pending_y);
	CHECK(a.clip_h == 1 && b.clip_h == 1 && d.clip_h == 1);
	cow_view_unshade(&d);
	CHECK(!c->shaded && a.pending_height == a_height);
	/* A third row may collapse even when its immediate sibling is
	 * collapsed. */
	extra.cfg			  = b.cfg;
	extra.dec.border_width		  = b.dec.border_width;
	extra.dec.title_thickness	  = b.dec.title_thickness;
	extra.dec.nominal_title_thickness = b.dec.nominal_title_thickness;
	CHECK(cow_container_add_to(&b, &extra));
	CHECK(cow_container_split(&b, &extra, COW_CONTAINER_VERTICAL, true));
	CHECK(cow_container_collapse(&extra));
	CHECK(cow_container_collapse(&b));
	CHECK(!cow_container_collapse(&a));
	CHECK(cow_container_collapse(&b));
	cow_container_forget_view(&extra);
	/* A horizontal split beside a row creates a full-height column. */
	CHECK(cow_container_add_to(&b, &extra));
	CHECK(cow_container_split(&b, &extra, COW_CONTAINER_HORIZONTAL, true));
	CHECK(cow_view_pane(&extra)->layout.box.height == c->frame.height);
	CHECK(
	    cow_view_pane(&a)->layout.box.x == cow_view_pane(&b)->layout.box.x);
	cow_container_forget_view(&extra);
	CHECK(cow_container_shared_edges(&a) == 1);
	CHECK(cow_container_shared_edges(&b) == 0);
	CHECK(a.pending_y - (b.pending_y + b.pending_height) ==
	    b.cfg.border + a.dec.title_thickness);
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
	/* Old pane coordinates must not relocate a newly arranged group. */
	struct cow_desk desk = { .page_vx = 1920, .page_vy = 1080 };
	a.desk		     = &desk;
	a.vx		     = 2000;
	a.vy		     = 1200;
	CHECK(a.pending_resize);
	cow_view_sync_global_from_vxy(&a);
	CHECK(c->frame.x == full.x && c->frame.y == full.y);
	a.pending_resize    = false;
	a.deferred_position = true;
	cow_view_sync_global_from_vxy(&a);
	CHECK(c->frame.x == full.x && c->frame.y == full.y);
	a.deferred_position = false;
	a.desk		    = NULL;
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
	CHECK(cow_container_split(&a, &b, COW_CONTAINER_HORIZONTAL, false));
	CHECK(a.clip_w > 0 && a.clip_h > 0);
	cow_wm.drag_drop_pane = cow_view_pane(&b);
	cow_container_forget_view(&b);
	CHECK(cow_wm.drag_drop_pane == NULL);
	CHECK(a.container_member == NULL && wl_list_empty(&cow_wm.containers));
	struct cow_titlebar_config standalone_tb;
	cow_titlebar_effective_config(&a, &standalone_tb);
	CHECK(standalone_tb.buttons[0].vector.num_points ==
	    a.cfg.titlebar.buttons[0].vector.num_points);
	CHECK(!cow_container_is_split(&a));
	CHECK(cow_decoration_corner_length(&a) > 0);
	CHECK(a.pending_clip && a.clip_w == 0 && a.clip_h == 0);
	CHECK(a.pending_tiled_edges_value == 0);
	cow_view_sync_container_geometry(&a, a.x, a.y, 1000, 700);
	CHECK(a.clip_w == 0 && a.clip_h == 0);
	cow_container_forget_view(&a);
	CHECK(wl_list_empty(&cow_wm.containers));

	/* Hiding titles keeps a handle, including after closing its owner. */
	c = cow_container_create(&a, &b);
	CHECK(cow_container_split(&a, &b, COW_CONTAINER_HORIZONTAL, false));
	CHECK(cow_container_title_hide(&a));
	CHECK(a.dec.title_thickness == 0);
	CHECK(a.cfg.titlebar.enabled);
	CHECK(!cow_container_title_hide(&b));
	CHECK(b.dec.title_thickness == 20);
	CHECK(cow_container_titles_restore(&a));
	CHECK(a.dec.title_thickness == 20);
	CHECK(cow_container_title_hide(&b));
	struct cow_box hidden_frame = cow_view_frame_box(&b);
	CHECK(cow_container_shared_edges(&b) & COW_EDGE_LEFT);
	CHECK(cow_container_border_title_target(&a, hidden_frame.x,
		  hidden_frame.y + hidden_frame.height / 2) == &b);
	CHECK(cow_container_border_title_target(&b, hidden_frame.x,
		  hidden_frame.y + hidden_frame.height / 2) == &b);
	CHECK(cow_container_border_title_target(&a,
		  hidden_frame.x + hidden_frame.width / 2,
		  hidden_frame.y + hidden_frame.height / 2) == &a);
	CHECK(cow_container_border_title_target(&a, hidden_frame.x - 1,
		  hidden_frame.y + hidden_frame.height / 2) == &a);
	CHECK(cow_container_titles_restore(&a));
	CHECK(cow_container_titles_toggle(&a, false));
	CHECK(a.dec.title_thickness == 0 && b.dec.title_thickness == 20);
	CHECK(cow_container_titles_toggle(&a, false));
	CHECK(a.dec.title_thickness == 20);
	CHECK(cow_container_titles_toggle(&a, true));
	CHECK(
	    cow_container_title_visible(&a) || cow_container_title_visible(&b));
	CHECK(cow_container_titles_toggle(&b, true));
	CHECK(a.dec.title_thickness == 20 && b.dec.title_thickness == 20);
	int saved_frame_width = c->frame.width;
	c->frame.width	      = 0;
	CHECK(!cow_container_titles_toggle(&a, true));
	CHECK(!cow_view_pane(&a)->title_hidden &&
	    !cow_view_pane(&b)->title_hidden);
	CHECK(a.dec.title_thickness == 20 && b.dec.title_thickness == 20);
	c->frame.width = saved_frame_width;
	CHECK(cow_container_title_hide(&a));
	struct cow_titlebar_config tb;
	cow_titlebar_effective_config(&b, &tb);
	int left_buttons, right_buttons;
	cow_titlebar_button_counts(&tb, &left_buttons, &right_buttons);
	CHECK(left_buttons == 2 && right_buttons == 5);
	CHECK(tb.buttons[1].vector.num_points == 8);
	CHECK(tb.buttons[0].vector.num_points == 4);
	CHECK(tb.buttons[3].vector.num_points == 5);
	/* Leave another tab so closing the handle does not dissolve the group. */
	d.cfg.titlebar.enabled = false;
	d.dec.title_thickness = 0;
	CHECK(cow_container_add_to(&a, &d));
	CHECK(cow_container_title_visible(&a));
	CHECK(a.dec.title_thickness == 20 && d.dec.title_thickness == 20);
	CHECK(!cow_container_title_hide(&a));
	CHECK(!cow_container_title_hide(&d));
	cow_titlebar_effective_config(&a, &tb);
	cow_titlebar_button_counts(&tb, &left_buttons, &right_buttons);
	CHECK(left_buttons == 2 && right_buttons == 4);
	CHECK(tb.buttons[4].vector.num_points == 0);
	/* Collections contain one record per group/pane, not per tab. */
	a.identifier		       = "pane-a";
	b.identifier		       = "pane-b";
	d.identifier		       = "pane-d";
	struct state_value *containers = state_snapshot_collection(
	    STATE_SRC_CONTAINERS);
	struct state_value *panes = state_snapshot_collection(STATE_SRC_PANES);
	CHECK(containers->u.list.n == 1 && panes->u.list.n == 2);
	struct state_value *group = containers->u.list.items[0];
	CHECK(state_record_get(group, "window_count")->u.n == 3);
	CHECK(state_record_get(group, "pane_count")->u.n == 2);
	CHECK(state_value_truthy(state_record_get(group, "split")));
	struct state_value *pane_record = state_snapshot_pane(c,
	    cow_view_pane(&a));
	CHECK(
	    state_record_get(pane_record, "id")->u.n == cow_view_pane(&a)->id);
	CHECK(state_record_get(pane_record, "tab_count")->u.n == 2);
	CHECK(strcmp(state_record_get(pane_record, "active_window_id")->u.s,
		  cow_view_pane(&a)->active->view->identifier) == 0);
	CHECK(
	    state_value_truthy(state_record_get(pane_record, "title_hidden")));
	CHECK(
	    state_value_truthy(state_record_get(pane_record, "title_visible")));
	CHECK(strcmp(state_record_get(pane_record, "container_id")->u.s,
		  state_record_get(group, "id")->u.s) == 0);
	struct state_value *frame = state_record_get(pane_record, "frame");
	CHECK(state_record_get(frame, "width")->u.n ==
	    cow_view_pane(&a)->layout.box.width);
	state_value_unref(pane_record);
	state_value_unref(containers);
	state_value_unref(panes);
	check_command("show -a container");
	check_command("show -a -F 'self.tab_count == 2' pane");
	check_command("show -t '#pane-a' container");
	check_command("show -t '#pane-a' pane");
	check_command("script { for c in (containers) { "
		      "if c.window_count != 3 { nop #{c.invalid} } "
		      "for p in (panes where p.container_id == c.id) { "
		      "if p.tab_count < 1 { nop #{p.invalid} } "
		      "show -t \"##{p.active_window_id}\" pane } } }");
	cow_container_forget_view(&b);
	CHECK(cow_container_title_visible(&a));
	CHECK(!cow_container_is_split(&a));
	cow_titlebar_effective_config(&a, &tb);
	CHECK(tb.buttons[0].vector.num_points ==
	    a.cfg.titlebar.buttons[0].vector.num_points);
	CHECK(a.dec.title_thickness == 20);
	/* An untitled tab promoted into the handle gains a temporary title. */
	cow_container_activate(c, d.container_member);
	CHECK(cow_container_title_visible(&d));
	CHECK(d.dec.title_thickness == 20 && !d.cfg.titlebar.enabled);
	CHECK(!cow_container_title_hide(&d));
	cow_container_destroy(c);
	CHECK(d.dec.title_thickness == 0);
	CHECK(a.dec.title_thickness == 20);

	/* Closing a tab also dissolves a plain two-window tab group. */
	c = cow_container_create(&a, &b);
	cow_container_forget_view(&a);
	CHECK(b.container_member == NULL);
	CHECK(wl_list_empty(&cow_wm.containers) && b.dec.dirty);

	/* Names bootstrap a group and target panes independently of focus. */
	check_command("window-container -t '#pane-a' -N work");
	c = cow_container_find("work");
	CHECK(c != NULL && c->n_members == 1);
	CHECK(!cow_container_set_name(&b, "work"));
	CHECK(b.container_member == NULL);
	check_command("window-container -G work -B editor");
	check_command(
	    "window-container -G work -b editor -s '#pane-b' -e right");
	CHECK(c->n_members == 2 && cow_container_is_split(&a));
	check_command("window-container -t '#pane-b' -B terminal");
	CHECK(!cow_container_pane_set_name(&b, "editor"));
	CHECK(cow_container_pane_find(c, "editor") == cow_view_pane(&a));
	check_command("window-container -G work -b terminal -s '#pane-d' -a");
	CHECK(cow_view_pane(&b) == cow_view_pane(&d));
	cow_container_remove_view(&d);
	cow_container_forget_view(&b);
	CHECK(cow_container_find("work") == c && c->n_members == 1);
	CHECK(cow_container_pane_find(c, "editor") == cow_view_pane(&a));
	check_command("window-container -G work -N renamed");
	CHECK(cow_container_find("work") == NULL);
	CHECK(cow_container_find("renamed") == c);
	check_command("window-container -G renamed -N ''");
	CHECK(a.container_member == NULL && wl_list_empty(&cow_wm.containers));
	CHECK(cow_container_set_name(&a, "final"));
	cow_container_forget_view(&a);
	CHECK(cow_container_find("final") == NULL);
	/* The same script can bootstrap a group and split subsequent windows.
	 */
	const char *ids[] = { "pane-a", "pane-b", "pane-d" };
	for (size_t i = 0; i < 3; i++) {
		char *script;
		xasprintf(&script,
		    "script { let id = \"%s\"\n"
		    "for c in (containers where c.name == \"automatic\") { "
		    "window-container -G automatic -s \"##{id}\" -e down\n break }\n"
		    "window-container -t \"##{id}\" -N automatic }",
		    ids[i]);
		check_command(script);
		xfree(script);
	}
	c = cow_container_find("automatic");
	CHECK(c != NULL && c->n_members == 3 && cow_container_is_split(&a));
	struct cow_box split_frame = c->frame;
	CHECK(!cow_container_split_empty(c, cow_view_pane(&a), "",
	    COW_CONTAINER_HORIZONTAL, true));
	CHECK(!c->persistent);
	check_command("window-container -t '#pane-a' -E -e left -B spare");
	CHECK(c->persistent && c->n_members == 3);
	CHECK(memcmp(&c->frame, &split_frame, sizeof(split_frame)) == 0);
	CHECK(cow_container_pane_find(c, "spare") != NULL);
	cow_container_destroy(c);
	/* An ordinary window becomes the occupied half of a persistent layout.
	 * Validate options before creating it and undo failed splits.
	 */
	CHECK(a.container_member == NULL);
	const char *invalid_actions[] = {
		"window-container",
		"window-container -n -p",
		"window-container -H -U",
		"window-container -a -r",
		"window-container -N validation -B pane",
		"window-container -E -N validation -D",
		"window-container -z column -o clockwise",
		"window-container -A",
	};
	for (size_t i = 0;
	    i < sizeof(invalid_actions) / sizeof(invalid_actions[0]); i++)
		check_command_result(invalid_actions[i], false);
	CHECK(cow_container_find("validation") == NULL);
	check_command_result("window-container -t '#pane-a' -E -e invalid -B spare",
	    false);
	CHECK(a.container_member == NULL);
	a.fullscreen = true;
	check_command_result("window-container -t '#pane-a' -E -e right -B spare",
	    false);
	CHECK(a.container_member == NULL);
	a.fullscreen = false;
	int saved_min_width = a.min_width;
	a.min_width = 100000;
	check_command_result("window-container -t '#pane-a' -E -e right -B spare",
	    false);
	CHECK(a.container_member == NULL);
	a.min_width = saved_min_width;
	struct cow_box standalone_frame = cow_view_frame_box(&a);
	check_command("window-container -t '#pane-a' -E -e right -B pane-right");
	c = cow_view_container(&a);
	CHECK(c != NULL && c->persistent && c->n_members == 1);
	CHECK(memcmp(&c->frame, &standalone_frame, sizeof(standalone_frame)) == 0);
	struct cow_container_pane *right_pane = cow_container_pane_find(c,
	    "pane-right");
	CHECK(right_pane != NULL && right_pane->n_tabs == 0);
	CHECK(right_pane->layout.box.x > cow_view_pane(&a)->layout.box.x);
	check_command("window-container -t '#pane-a' -E -e down");
	CHECK(cow_container_pane_next(c, cow_view_pane(&a))->name == NULL);
	cow_container_destroy(c);
	/* Existing anonymous tab groups can gain empty panes and survive
	 * restart.
	 */
	c					 = cow_container_create(&a, &b);
	struct cow_container_pane *original_tabs = cow_view_pane(&a);
	struct cow_box		   original_frame = cow_view_frame_box(&a);
	check_command("window-container -t '#pane-a' -E -e left -B spare");
	CHECK(c->persistent && c->name == NULL && c->n_members == 2);
	CHECK(cow_view_pane(&a) == original_tabs &&
	    cow_view_pane(&b) == original_tabs);
	CHECK(memcmp(&c->frame, &original_frame, sizeof(original_frame)) == 0);
	CHECK(cow_container_pane_find(c, "spare")->n_tabs == 0);
	check_command("window-container -t '#pane-a' -E -e left -B spare");
	CHECK(cow_container_pane_next(c, cow_container_pane_next(c, NULL)) ==
	    original_tabs);
	cow_container_remove_view(&a);
	cow_container_forget_view(&b);
	cJSON *converted = cow_container_save(c);
	cow_container_destroy(c);
	c = cow_container_restore(converted);
	CHECK(c != NULL && c->persistent && c->name == NULL);
	CHECK(cow_container_pane_find(c, "spare") != NULL);
	cJSON_Delete(converted);
	cow_container_destroy(c);
	/* Empty named layouts are addressable before any window exists. */
	check_command("window-container -E -N unnamed-test");
	c = cow_container_find("unnamed-test");
	CHECK(c != NULL && c->root->name == NULL);
	cow_container_destroy(c);
	check_command("window-container -E -N workspace -B editor");
	check_command(
	    "window-container -G workspace -b editor -E -e right -B terminal");
	check_command(
	    "window-container -G workspace -b terminal -E -e down -B monitor");
	c = cow_container_find("workspace");
	CHECK(c != NULL && c->persistent && c->n_members == 0 &&
	    c->active == NULL);
	struct state_value *empty = state_snapshot_container(c);
	CHECK(state_record_get(empty, "pane_count")->u.n == 3);
	state_value_unref(empty);
	check_command("show -a -F 'self.empty' pane");
	check_command(
	    "window-container -G workspace -b editor -s '#pane-a' -a");
	check_command(
	    "window-container -G workspace -b terminal -s '#pane-b' -a");
	CHECK(c->n_members == 2);
	CHECK(cow_view_pane(&a) != cow_view_pane(&b));
	struct cow_container_pane *reserved = cow_view_pane(&b);
	cow_container_forget_view(&b);
	CHECK(cow_container_pane_find(c, "terminal") == reserved);
	CHECK(reserved->active == NULL && reserved->n_tabs == 0);
	check_command(
	    "window-container -G workspace -b terminal -s '#pane-b' -a");
	cow_container_remove_view(&a);
	CHECK(cow_container_shade(&b, true));
	cow_container_remove_view(&b);
	CHECK(c->active == NULL && c->n_members == 0 && !c->shaded);
	CHECK(cow_container_pane_find(c, "editor") != NULL);
	cJSON *saved_layout = cow_container_save(c);
	/* Restore over the still-empty config declaration. */
	c = cow_container_restore(saved_layout);
	CHECK(c != NULL && c->persistent && c->n_members == 0);
	CHECK(cow_container_pane_find(c, "terminal") != NULL);
	CHECK(cow_container_pane_find(c, "monitor") != NULL);
	cJSON_Delete(saved_layout);
	check_command(
	    "window-container -G workspace -b monitor -s '#pane-d' -a");
	CHECK(cow_view_container(&d) == c);
	check_command("window-container -G workspace -D");
	CHECK(d.container_member == NULL &&
	    cow_container_find("workspace") == NULL);
	/* Empty panes are drop targets, but not through covering windows. */
	output.x = output.y = 0;
	output.width	    = 1280;
	output.height	    = 720;
	output.usable_area  = (struct cow_box) { 0, 30, 1280, 690 };
	output.current_desk = &desk;
	desk.page_vx = desk.page_vy = 0;
	wl_list_init(&output.desk_set.desks);
	wl_list_insert(&output.desk_set.desks, &desk.set_link);
	wl_list_insert(&cow_wm.outputs, &output.wm_link);
	cow_wm.current_output = &output;
	a.closed = b.closed = d.closed = true;
	check_command("window-container -E -N drop-test -B left");
	check_command(
	    "window-container -G drop-test -b left -E -e right -B right");
	c = cow_container_find("drop-test");
	c->output = &output;
	c->desk	  = &desk;
	CHECK(cow_container_arrange(c, output.usable_area));
	CHECK(c->output == &output && c->desk == &desk && c->frame.y == 30);
	check_command("window-container -G drop-test -o clockwise");
	CHECK(c->root->layout.axis == COW_CONTAINER_VERTICAL);
	check_command("window-container -G drop-test -o anticlockwise");
	CHECK(c->root->layout.axis == COW_CONTAINER_HORIZONTAL);
	check_command("window-container -G drop-test -o 180");
	check_command("window-container -G drop-test -o 180");
	/* A backing window is managed normally, but must never become a member.
	 */
	cow_container_set_frame(c, &a);
	CHECK(cow_container_window_owner(&a) == c);
	CHECK(cow_container_window_view(&a));
	a.closed = false;
	check_command("show -t '#pane-a' container");
	a.closed = true;
	CHECK(!cow_container_insert(c, c->root, &a));
	CHECK(c->n_members == 0);
	cow_container_window_forget(&a);
	CHECK(c->frame_view == NULL && !cow_container_window_view(&a));
	struct cow_container_pane *hit = NULL;
	CHECK(cow_container_empty_at(900, 100, NULL, &hit) == c);
	CHECK(strcmp(hit->name, "right") == 0);
	a.closed	= false;
	a.output	= &output;
	a.desk		= &desk;
	a.x		= 800;
	a.y		= 50;
	a.content_width = a.content_height = 200;
	a.pending_resize = a.deferred_position = false;
	a.vx				       = 800;
	a.vy				       = 50;
	CHECK(cow_container_empty_at(900, 100, NULL, &hit) == NULL);
	CHECK(cow_container_empty_at(900, 100, &a, &hit) == c);
	desk.page_vx = 1280;
	struct cow_box shifted = c->frame;
	shifted.x -= 1280;
	CHECK(cow_container_arrange(c, shifted));
	CHECK(c->frame.x == -1280);
	CHECK(cow_container_empty_at(900, 100, &a, &hit) == NULL);
	desk.page_vx = 0;
	CHECK(cow_container_arrange(c, output.usable_area));
	CHECK(cow_container_empty_at(900, 100, &a, &hit) == c);
	cow_wm.drag_drop_pane = hit;
	check_command("window-container -G drop-test -D");
	CHECK(cow_wm.drag_drop_pane == NULL);
	/* Vacant pane views share normal membership, not application counts.
	 * The permanent frame supplies the handle when its pane title is
	 * hidden.
	 */
	c	  = cow_container_create_empty("framed", "vacant");
	c->output = &output;
	c->desk	  = &desk;
	CHECK(cow_container_arrange(c, output.usable_area));
	cow_container_set_frame(c, &a);
	c->framed		= true;
	b.closed		= false;
	b.container_placeholder = true;
	b.output		= &output;
	b.desk			= &desk;
	CHECK(cow_container_add_placeholder(c, c->root, &b));
	wl_list_init(&b.selections);
	struct state_value *window_record = state_snapshot_view(&b);
	CHECK(strcmp(state_record_get(window_record, "container_window_id")->u.s,
	    a.identifier) == 0);
	state_value_unref(window_record);
	CHECK(cow_container_window_count(c) == 0);
	CHECK(cow_container_pane_window_count(c->root) == 0);
	CHECK(cow_container_is_split(&b));
	CHECK(!cow_container_is_split(&a));
	CHECK(cow_container_pane_rename(c, cow_view_pane(&b), "named"));
	char *pane_title = cow_decoration_display_title(&b);
	CHECK(strcmp(pane_title, "named") == 0);
	xfree(pane_title);
	b.container_placeholder = false;
	char *saved_title = b.title;
	b.title = "Terminal";
	pane_title = cow_decoration_display_title(&b);
	CHECK(strcmp(pane_title, "[named] Terminal") == 0);
	xfree(pane_title);
	xstrset(&b.cfg.pane_title_format, "%p: %n (%%)");
	pane_title = cow_decoration_display_title(&b);
	CHECK(strcmp(pane_title, "named: Terminal (%)") == 0);
	xfree(pane_title);
	xstrset(&b.cfg.pane_title_format, NULL);
	b.title = saved_title;
	b.container_placeholder = true;
	CHECK(cow_decoration_corner_length(&a) > 0);
	b.cfg.handles = true;
	b.cfg.corner_length = 20;
	CHECK(cow_decoration_corner_length(&b) == 0);
	CHECK(cow_container_title_hide(&b));
	CHECK(!cow_container_title_visible(&b));
	CHECK(cow_container_titles_restore(&a));
	CHECK(cow_container_title_visible(&b));
	/* Pane-border bindings leave ordinary edges and the frame alone.
	 */
	cow_config = cow_config_create();
	wl_list_init(&b.selections);
	CHECK(cow_dec_region_parse("pane-titlebar") ==
	    COW_DEC_REGION_PANE_TITLEBAR);
	CHECK(strcmp(cow_dec_region_name(COW_DEC_REGION_PANE_TITLEBAR),
	    "pane-titlebar") == 0);
	cow_decoration_register_default_bindings();
	/* Loading old user bindings must not replace pane defaults. */
	check_command("bind-decoration titlebar menu-show WindowOps");
	check_command("bind-decoration -2 titlebar window-shade -d titlebar");
	CHECK(strcmp(cow_dec_binding_for_view(&b, COW_DEC_REGION_TITLEBAR, 1),
	    "window-move") == 0);
	CHECK(strcmp(cow_dec_binding_for_view(&b, COW_DEC_REGION_TITLEBAR, 2),
	    "window-container -c") == 0);
	CHECK(strcmp(cow_dec_binding_for_view(&a, COW_DEC_REGION_TITLEBAR, 2),
	    "window-shade -d titlebar") == 0);
	check_command("bind-decoration -2 pane-titlebar window-container -T");
	CHECK(cow_dec_binding_fire(&b, COW_DEC_REGION_TITLEBAR, 2));
	CHECK(!cow_container_title_visible(&b));
	CHECK(cow_dec_binding_fire(&b, COW_DEC_REGION_TITLEBAR, 2));
	CHECK(cow_container_title_visible(&b));
	check_command("bind-decoration 1 menu-show WindowOps");
	check_command("bind-decoration -2 1 winops -c");
	CHECK(cow_dec_binding_for_view(&b, COW_DEC_REGION_BUTTON_1, 1) == NULL);
	CHECK(cow_dec_binding_for_view(&b, COW_DEC_REGION_BUTTON_1, 2) == NULL);
	check_command("bind-decoration pane:1 window-container -T");
	check_command("bind-decoration -2 pane:1 window-container -U");
	CHECK(strcmp(cow_dec_binding_for_view(&a, COW_DEC_REGION_BUTTON_1, 1),
		  "menu-show WindowOps") == 0);
	CHECK(strcmp(cow_dec_binding_for_view(&a, COW_DEC_REGION_BUTTON_1, 2),
		  "winops -c") == 0);
	CHECK(strcmp(cow_dec_binding_for_view(&b, COW_DEC_REGION_BUTTON_1, 2),
		  "window-container -U") == 0);
	CHECK(cow_dec_binding_fire(&b, COW_DEC_REGION_BUTTON_1, 1));
	CHECK(!cow_container_title_visible(&b));
	CHECK(cow_dec_binding_fire(&b, COW_DEC_REGION_BUTTON_1, 2));
	CHECK(cow_container_title_visible(&b));
	for (int i = 0; i < COW_TITLEBAR_NUM_BUTTONS; i++) {
		enum cow_dec_region pane_region = COW_DEC_REGION_PANE_BUTTON_0 +
		    i;
		CHECK(cow_dec_region_parse(cow_dec_region_name(pane_region)) ==
		    pane_region);
	}
	check_command("bind-decoration -2 pane-border script { "
		      "if self.pane_title_hidden { window-container -T } }");
	CHECK(cow_container_title_hide(&b));
	CHECK(!cow_dec_binding_fire(&a, COW_DEC_REGION_N, 2));
	CHECK(!cow_container_title_visible(&b));
	CHECK(cow_dec_binding_fire(&b, COW_DEC_REGION_N, 2));
	CHECK(cow_container_title_visible(&b));
	CHECK(cow_dec_binding_fire(&b, COW_DEC_REGION_SW, 2));
	CHECK(cow_container_title_visible(&b));
	check_command("bind-decoration N script { window-container -T }");
	CHECK(cow_dec_binding_fire(&b, COW_DEC_REGION_N, 1));
	CHECK(!cow_container_title_visible(&b));
	CHECK(cow_dec_binding_fire(&b, COW_DEC_REGION_N, 2));
	CHECK(cow_container_title_visible(&b));
	cow_config_destroy(cow_config);
	cow_config = NULL;
	CHECK(cow_container_titles_toggle(&a, false));
	CHECK(!cow_container_title_visible(&b));
	CHECK(cow_container_titles_toggle(&a, true));
	CHECK(cow_container_title_visible(&b));
	check_command("window-container -G framed -TA");
	CHECK(!cow_container_title_visible(&b));
	check_command("window-container -G framed -T");
	CHECK(cow_container_title_visible(&b));
	CHECK(cow_container_title_hide(&b));
	a.needs_raise = true;
	cow_stack_apply_pending(&a);
	CHECK(a.raise_seq == b.raise_seq);
	b.needs_raise = true;
	cow_stack_apply_pending(&b);
	CHECK(a.raise_seq == b.raise_seq);
	cJSON *empty_state = cow_container_save(c);
	CHECK(cJSON_IsTrue(cJSON_GetObjectItem(empty_state, "framed")));
	CHECK(cJSON_GetArraySize(
		  cJSON_GetObjectItem(cJSON_GetObjectItem(empty_state, "tree"),
		      "tabs")) == 0);
	cJSON_Delete(empty_state);
	cow_container_forget_view(&b);
	b.container_placeholder = false;
	cow_container_destroy(c);
	/* Expand either pane through the outer frame, on an offset output.
	 * Treating a pane's requested growth as a layout delta moves the frame
	 * off its output and hides every pane under origin page visibility.
	 */
	output.x	   = 1920;
	output.y	   = 1080;
	output.width	   = 1920;
	output.height	   = 1080;
	output.usable_area = (struct cow_box) { 1920, 1098, 1920, 1062 };
	a.x		   = 2100;
	a.y		   = 1200;
	a.vx		   = 180;
	a.vy		   = 120;
	a.content_width	   = 600;
	a.content_height   = 400;
	a.expanded	   = false;
	a.pending_resize   = false;
	c		   = cow_container_create_empty("expand", "left");
	c->output	   = &output;
	c->desk		   = &desk;
	cow_container_set_frame(c, &a);
	c->framed	   = true;
	CHECK(cow_container_arrange(c,
	    (struct cow_box) { 2100, 1200, 600, 400 }));
	CHECK(cow_container_split_empty(c, c->root, "right",
	    COW_CONTAINER_HORIZONTAL, false));
	d.closed = false;
	d.output = &output;
	d.desk	 = &desk;
	CHECK(cow_container_insert(c, cow_container_pane_find(c, "left"), &b));
	CHECK(cow_container_insert(c, cow_container_pane_find(c, "right"), &d));
	a.pending_resize = b.pending_resize = d.pending_resize = false;
	check_command("window-expand -t '#pane-d' -T");
	CHECK(a.expanded && a.pending_resize);
	CHECK(!b.expanded && !d.expanded && !b.pending_resize &&
	    !d.pending_resize);
	int extra_w, extra_h, inset_x, inset_y;
	cow_titlebar_frame_geometry(&a, 0, 0, &extra_w, &extra_h, &inset_x,
	    &inset_y);
	CHECK(a.pending_x == output.usable_area.x + inset_x);
	CHECK(a.pending_y == output.usable_area.y + inset_y);
	CHECK(a.pending_width == output.usable_area.width - extra_w);
	CHECK(a.pending_height == output.usable_area.height - extra_h);
	/* Toggling from the other pane restores the same outer geometry. */
	check_command("window-expand -t '#pane-b' -T");
	CHECK(!a.expanded && a.pending_x == 2100 && a.pending_y == 1200);
	CHECK(a.pending_width == 600 && a.pending_height == 400);
	/* Unrelated windows still constrain directional expansion. */
	struct cow_view obstacle = { .has_dimensions = true,
		.output				     = &output,
		.desk				     = &desk,
		.x				     = 3000,
		.y				     = 1200,
		.content_width			     = 100,
		.content_height			     = 400 };
	wl_list_insert(&cow_wm.views, &obstacle.wm_link);
	struct cow_box expand_frame = cow_view_frame_box(&a);
	check_command("window-expand -t '#pane-b' -d right -T");
	CHECK(a.pending_x == 2100 && a.pending_y == 1200);
	CHECK(a.pending_width == obstacle.x - expand_frame.x - extra_w);
	CHECK(a.pending_height == 400);
	check_command("window-expand -t '#pane-a' -T");
	CHECK(!a.expanded && a.pending_width == 600 && a.pending_height == 400);
	wl_list_remove(&obstacle.wm_link);
	/* Holding the outer titlebar must not snap the frame to its own panes.
	 */
	struct cow_config config = { .snap_distance = 12,
		.clamp_mode			    = COW_CLAMP_NONE };
	cow_config		 = &config;
	a.pending_resize	 = false;
	a.closed = b.closed = d.closed = false;
	a.has_dimensions = b.has_dimensions = d.has_dimensions = true;
	CHECK(cow_container_arrange(c,
	    (struct cow_box) { a.x, a.y, a.content_width, a.content_height }));
	struct cow_view *drag_panes[] = { &b, &d };
	for (size_t i = 0; i < 2; i++) {
		struct cow_view *pane = drag_panes[i];
		pane->x		      = pane->pending_x;
		pane->y		      = pane->pending_y;
		pane->content_width   = pane->pending_width;
		pane->content_height  = pane->pending_height;
		pane->pending_resize  = false;
		cow_view_sync_vxy_from_global(pane);
	}
	struct cow_seat seat = {
		.op_in_progress = true,
		.op		= { .type = COW_POINTER_OP_MOVE,
				   .view	  = &a,
				   .start_x  = a.x,
				   .start_y  = a.y }
	};
	for (int i = 0; i < 3; i++) {
		cow_pointer_op_process(&seat);
		CHECK(a.x == 2100 && a.y == 1200);
	}
	/* Snapping to a separate window remains available. */
	obstacle.x = cow_view_frame_box(&a).x - 3 - obstacle.content_width;
	wl_list_insert(&cow_wm.views, &obstacle.wm_link);
	cow_pointer_op_process(&seat);
	CHECK(a.x == 2097);
	wl_list_remove(&obstacle.wm_link);
	cow_config = NULL;
	/* Combined stickiness normalises either mixed state across the group.
	 */
	uint32_t sticky_mask = COW_VIEW_STICKY_DESK_FLAG |
	    COW_VIEW_STICKY_PAGE_FLAG;
	for (unsigned state = 0; state < 4; state++) {
		a.flags &= ~sticky_mask;
		if (state & 1)
			a.flags |= COW_VIEW_STICKY_DESK_FLAG;
		if (state & 2)
			a.flags |= COW_VIEW_STICKY_PAGE_FLAG;
		cow_container_sync_flags(&a, sticky_mask);
		check_command("winops -t '#pane-b' -Ss");
		uint32_t expected = state == 3 ? 0 : sticky_mask;
		CHECK((a.flags & sticky_mask) == expected);
		CHECK((b.flags & sticky_mask) == expected);
		CHECK((d.flags & sticky_mask) == expected);
	}
	check_command("winops -t '#pane-b' -s");
	CHECK((a.flags & sticky_mask) == COW_VIEW_STICKY_DESK_FLAG);
	check_command("winops -t '#pane-b' -S");
	CHECK((a.flags & sticky_mask) == sticky_mask);
	check_command("winops -t '#pane-b' -s");
	CHECK((a.flags & sticky_mask) == COW_VIEW_STICKY_PAGE_FLAG);
	check_command("winops -t '#pane-b' -S");
	CHECK((a.flags & sticky_mask) == 0);
	/* A complete layout can be one outer tab without flattening panes. */
	a.x		    = 2100;
	a.pending_resize    = false;
	obstacle.identifier = "outer-tab";
	obstacle.use_ssd    = true;
	wl_list_insert(&cow_wm.views, &obstacle.wm_link);
	/* The frame must not intercept pane drops, regardless of list order. */
	a.layer = b.layer = d.layer = 4;
	a.raise_seq = b.raise_seq = d.raise_seq = 10;
	wl_list_remove(&a.wm_link);
	wl_list_insert(&cow_wm.views, &a.wm_link);
	CHECK(
	    cow_container_drop_target_at(b.x + 20, b.y + 20, &obstacle) == &b);
	CHECK(
	    cow_container_drop_target_at(d.x + 20, d.y + 20, &obstacle) == &d);
	CHECK(
	    cow_container_drop_target_at(a.x + 40, a.y - 10, &obstacle) == &a);
	wl_list_remove(&a.wm_link);
	wl_list_insert(cow_wm.views.prev, &a.wm_link);
	CHECK(
	    cow_container_drop_target_at(b.x + 20, b.y + 20, &obstacle) == &b);
	struct cow_view cover = { .has_dimensions = true,
		.output				  = &output,
		.desk				  = &desk,
		.x				  = b.x,
		.y				  = b.y,
		.content_width			  = 100,
		.content_height			  = 100,
		.layer				  = 4,
		.raise_seq			  = 11 };
	wl_list_insert(&cow_wm.views, &cover.wm_link);
	CHECK(cow_container_drop_target_at(b.x + 20, b.y + 20, &obstacle) ==
	    &cover);
	wl_list_remove(&cover.wm_link);
	struct cow_container *tabs = cow_container_create(&a, &obstacle);
	CHECK(cow_container_window_owner(&a) == c);
	CHECK(cow_view_container(&a) == tabs);
	CHECK(cow_container_title_hide(&b));
	CHECK(cow_container_titles_restore(&a));
	CHECK(cow_container_title_visible(&b));
	CHECK(cow_container_top_group(&b) == tabs);
	CHECK(cow_container_top_group(&d) == tabs);
	struct cow_view *member	     = NULL;
	int		 group_count = 0;
	while ((member = cow_container_next_view(tabs, member)) != NULL)
		group_count++;
	CHECK(group_count == 4);
	CHECK(!cow_container_can_tab(c, &a));
	CHECK(!cow_container_split(&a, &obstacle, COW_CONTAINER_HORIZONTAL,
	    false));
	cow_container_activate(tabs, obstacle.container_member);
	CHECK(cow_container_hides_view(&b) && cow_container_hides_view(&d));
	check_command("window-container -G expand -n");
	CHECK(tabs->active->view == &a);
	cow_container_activate(tabs, obstacle.container_member);
	cow_container_activate(c, b.container_member);
	CHECK(tabs->active->view == &a);
	CHECK(!cow_container_hides_view(&b) && !cow_container_hides_view(&d));
	CHECK(cow_wm.pending_focus == &b);
	cJSON *tab_state = cow_container_save(tabs);
	CHECK(cow_container_restore_defer(tab_state));
	cow_container_destroy(tabs);
	c->restore_identifier = xstrdup(a.identifier);
	a.identifier	      = "recreated-frame";
	cow_container_set_frame(c, NULL);
	cow_container_restore_pending();
	CHECK(!a.container_member);
	cow_container_set_frame(c, &a);
	a.pending_resize = false;
	cow_container_restore_pending();
	tabs = cow_view_container(&a);
	CHECK(tabs && cow_view_container(&obstacle) == tabs);
	CHECK(tabs->active->view == &a);
	CHECK(cow_container_window_owner(&a) == c);
	cJSON_Delete(tab_state);
	cow_container_restore_clear();
	/* Losing a pane must not dissolve the layout out of its outer tab. */
	char *layout_name = c->name;
	c->name		  = NULL;
	cow_container_forget_view(&d);
	CHECK(cow_container_window_owner(&a) == c);
	CHECK(cow_view_container(&a) == tabs && cow_view_container(&b) == c);
	c->name = layout_name;
	cow_container_remove_view(&a);
	CHECK(!a.container_member && !obstacle.container_member);
	CHECK(cow_container_window_owner(&a) == c);
	CHECK(cow_view_container(&b) == c && !cow_view_container(&d));
	/* Split an ordinary outer tab without splitting its layout sibling. */
	obstacle.closed = d.closed = false;
	obstacle.content_width	   = 800;
	obstacle.content_height	   = 600;
	for (int named = 0; named < 2; named++) {
		tabs = cow_container_create(&a, &obstacle);
		cow_container_activate(tabs, obstacle.container_member);
		struct cow_container_member *old_slot = obstacle.container_member;
		CHECK(cow_container_split_empty(tabs, cow_view_pane(&obstacle),
		    named ? "spare" : NULL, COW_CONTAINER_HORIZONTAL, false));
		struct cow_container *empty_layout = pending_split(&obstacle);
		CHECK(empty_layout != NULL && empty_layout->split_empty);
		struct cow_view empty_frame = { .has_dimensions = true,
			.output = &output, .desk = &desk,
			.content_width = 800, .content_height = 600 };
		cow_container_set_frame(empty_layout, &empty_frame);
		CHECK(cow_container_complete_tab_split(empty_layout));
		CHECK(tabs->n_members == 2 && empty_frame.container_member == old_slot);
		CHECK(cow_view_container(&a) == tabs);
		CHECK(cow_container_window_owner(&a) == c);
		CHECK(cow_view_container(&obstacle) == empty_layout);
		CHECK(empty_layout->persistent && empty_layout->n_members == 1);
		struct cow_container_pane *vacant = cow_container_pane_next(
		    empty_layout, cow_view_pane(&obstacle));
		CHECK(vacant != NULL && vacant->n_tabs == 0);
		CHECK(named ? strcmp(vacant->name, "spare") == 0 : vacant->name == NULL);
		cow_container_destroy(tabs);
		cow_container_destroy(empty_layout);
	}
	tabs			   = cow_container_create(&a, &obstacle);
	CHECK(cow_container_add_to(&obstacle, &d));
	cow_container_activate(tabs, obstacle.container_member);
	struct cow_container_member *slot = obstacle.container_member;
	CHECK(cow_container_split(&obstacle, &d, COW_CONTAINER_HORIZONTAL,
	    false));
	struct cow_container *promoted = pending_split(&obstacle);
	CHECK(promoted != NULL);
	CHECK(obstacle.container_member == slot);
	struct cow_view promoted_frame = { .has_dimensions = true,
		.output					   = &output,
		.desk					   = &desk,
		.content_width				   = 800,
		.content_height				   = 600 };
	cow_container_set_frame(promoted, &promoted_frame);
	promoted_frame.identifier = "nested-frame";
	CHECK(cow_container_complete_tab_split(promoted));
	CHECK(promoted_frame.container_member == slot && tabs->n_members == 2);
	CHECK(cow_view_container(&obstacle) == promoted);
	CHECK(cow_view_container(&d) == promoted);
	CHECK(cow_view_pane(&obstacle) != cow_view_pane(&d));
	CHECK(cow_view_container(&a) == tabs && cow_view_container(&b) == c);
	CHECK(tabs->active == slot && cow_wm.pending_focus == &d);
	wl_list_init(&d.selections);
	window_record = state_snapshot_view(&d);
	CHECK(strcmp(state_record_get(window_record, "container_window_id")->u.s,
	    promoted_frame.identifier) == 0);
	state_value_unref(window_record);
	cow_container_destroy(tabs);
	cow_container_destroy(promoted);
	/* Cancelling before the new frame maps leaves the original tabs. */
	tabs = cow_container_create(&a, &obstacle);
	CHECK(cow_container_add_to(&obstacle, &d));
	CHECK(cow_container_split(&obstacle, &d, COW_CONTAINER_VERTICAL, true));
	promoted = pending_split(&obstacle);
	CHECK(promoted != NULL);
	cow_container_window_forget(&d);
	cow_container_set_frame(promoted, &promoted_frame);
	CHECK(!cow_container_complete_tab_split(promoted));
	CHECK(cow_view_container(&obstacle) == tabs);
	cow_container_destroy(promoted);
	cow_container_destroy(tabs);
	wl_list_remove(&obstacle.wm_link);
	cow_container_destroy(c);
	wl_list_remove(&output.wm_link);

	/* Rearranging panes retains their tabs and the outer frame. */
	{
		struct cow_view panes[3] = { 0 };
		for (size_t i = 0; i < 3; i++) {
			panes[i].content_width	= 600;
			panes[i].content_height = 400;
			panes[i].use_ssd	= true;
		}
		c = cow_container_create(&panes[0], &panes[1]);
		CHECK(c != NULL);
		CHECK(cow_container_add_to(&panes[0], &panes[2]));
		CHECK(cow_container_split(&panes[0], &panes[1],
		    COW_CONTAINER_HORIZONTAL, false));
		struct cow_box		   original = c->frame;
		struct cow_container_pane *moving   = cow_view_pane(&panes[0]);
		CHECK(cow_container_rearrange(&panes[1], &panes[0],
		    COW_CONTAINER_VERTICAL, false));
		CHECK(cow_view_pane(&panes[0]) == moving);
		CHECK(
		    cow_view_pane(&panes[2]) == moving && moving->n_tabs == 2);
		CHECK(panes[0].pending_y > panes[1].pending_y);
		CHECK(memcmp(&original, &c->frame, sizeof(original)) == 0);
		CHECK(!cow_container_rearrange(&panes[2], &panes[0],
		    COW_CONTAINER_HORIZONTAL, true));
		struct cow_container_pane *root = c->root;
		panes[0].min_width		= 10000;
		CHECK(!cow_container_rearrange(&panes[1], &panes[0],
		    COW_CONTAINER_HORIZONTAL, true));
		CHECK(c->root == root &&
		    root->layout.axis == COW_CONTAINER_VERTICAL);
		CHECK(
		    cow_view_pane(&panes[0]) == moving && moving->n_tabs == 2);
		CHECK(!cow_container_rearrange(&panes[1], &panes[0],
		    COW_CONTAINER_LEAF, false));
		CHECK(c->root == root && moving->n_tabs == 2);
		panes[0].min_width = 0;
		CHECK(cow_container_arrange(c, original));
		CHECK(cow_container_rearrange(&panes[1], &panes[0],
		    COW_CONTAINER_LEAF, false));
		CHECK(c->root->n_tabs == 3 && c->n_members == 3);
		CHECK(cow_view_pane(&panes[0]) == cow_view_pane(&panes[1]));
		CHECK(cow_view_pane(&panes[2]) == c->root);
		CHECK(memcmp(&original, &c->frame, sizeof(original)) == 0);
		CHECK(cow_container_split(&panes[0], &panes[1],
		    COW_CONTAINER_HORIZONTAL, false));
		CHECK(cow_container_split(&panes[0], &panes[2],
		    COW_CONTAINER_VERTICAL, false));
		CHECK(cow_container_rearrange(&panes[0], &panes[1],
		    COW_CONTAINER_VERTICAL, true));
		CHECK(panes[1].pending_y < panes[0].pending_y);
		CHECK(cow_container_rearrange(&panes[1], &panes[2],
		    COW_CONTAINER_HORIZONTAL, false));
		CHECK(panes[2].pending_x > panes[1].pending_x);
		CHECK(memcmp(&original, &c->frame, sizeof(original)) == 0);
		int row_height	 = panes[0].pending_height;
		int column_width = panes[0].pending_width;
		struct cow_container_pane *rotate_root	= c->root;
		struct cow_container_layout *rotate_first =
		    c->root->layout.child[0];
		unsigned rotate_share = c->root->layout.share;
		CHECK(cow_container_rotate(c, 1));
		CHECK(memcmp(&original, &c->frame, sizeof(original)) == 0);
		CHECK(cow_container_rotate(c, 3));
		CHECK(c->root == rotate_root &&
		    c->root->layout.child[0] == rotate_first &&
		    c->root->layout.share == rotate_share);
		CHECK(panes[0].pending_height == row_height);
		CHECK(panes[0].pending_width == column_width);
		panes[0].min_width = original.width + 1;
		CHECK(!cow_container_rotate(c, 1));
		CHECK(c->root == rotate_root &&
		    c->root->layout.child[0] == rotate_first &&
		    c->root->layout.share == rotate_share);
		CHECK(memcmp(&original, &c->frame, sizeof(original)) == 0);
		CHECK(panes[0].pending_height == row_height);
		panes[0].min_width = 0;
		cow_view_pane(&panes[0])->layout.collapsed =
		    COW_CONTAINER_VERTICAL;
		CHECK(!cow_container_rotate(c, 1));
		cow_view_pane(&panes[0])->layout.collapsed = COW_CONTAINER_LEAF;
		cow_container_activate(c, panes[0].container_member);
		cow_wm.pending_focus = &panes[0];
		CHECK(cow_container_pane_visible(c, cow_view_pane(&panes[0]),
		    false));
		CHECK(cow_container_hides_view(&panes[0]));
		CHECK(cow_container_hidden_count(c) == 1);
		CHECK(cow_container_hidden_border_edges(&panes[0]) == 0);
		CHECK(cow_container_hidden_border_edges(&panes[1]) != 0);
		cow_view_pane(&panes[0])->layout.collapsed =
		    COW_CONTAINER_VERTICAL;
		panes[1].min_width = original.width + 1;
		CHECK(!cow_container_rotate(c, 1));
		CHECK(cow_view_pane(&panes[0])->layout.collapsed ==
		    COW_CONTAINER_VERTICAL);
		panes[1].min_width = 0;
		/* Hidden-pane indicators follow the split through every rotation.
		 */
		for (int turn = 0; turn < 4; turn++) {
			CHECK(cow_container_rotate(c, 1));
			CHECK(cow_view_pane(&panes[0])->layout.hidden);
			CHECK(cow_view_pane(&panes[0])->layout.collapsed ==
			    COW_CONTAINER_LEAF);
			uint32_t edges = cow_container_hidden_border_edges(&panes[1]) |
			    cow_container_hidden_border_edges(&panes[2]);
			CHECK(edges != 0);
			CHECK((edges & (COW_EDGE_TOP | COW_EDGE_BOTTOM)) != 0 ||
			    turn % 2 != 0);
		}
		/* The restore button works on all titlebar orientations.
		 */
		struct cow_view badge = { .container_owner = c,
			.content_width = 800, .content_height = 400 };
		badge.dec.border_width = 4;
		badge.dec.title_thickness = 20;
		badge.cfg.titlebar.enabled = true;
		/* All ten user positions can coexist with the extra restore button.
		 */
		for (int i = 0; i < COW_TITLEBAR_NUM_BUTTONS; i++)
			badge.cfg.titlebar.buttons[i].vector.num_points = 1;
		for (int position = COW_TITLEBAR_TOP;
		    position <= COW_TITLEBAR_RIGHT; position++) {
			badge.cfg.titlebar.position = position;
			struct cow_titlebar_config titlebar;
			cow_titlebar_effective_config(&badge, &titlebar);
			int left, right, fw, fh, cx, cy;
			cow_titlebar_button_counts(&titlebar, &left, &right);
			cow_titlebar_frame_geometry(&badge, 800, 400,
			    &fw, &fh, &cx, &cy);
			struct cow_box rect = cow_titlebar_squeezed_rect(&badge);
			bool vertical = cow_titlebar_is_vertical(position);
			int along = (vertical ? rect.height : rect.width) -
			    right * 20 - 10;
			if (position == COW_TITLEBAR_LEFT)
				along = rect.height - 1 - along;
			int x = rect.x + (vertical ? 10 : along) - cx;
			int y = rect.y + (vertical ? along : 10) - cy;
			CHECK(cow_titlebar_button_at(&badge, x, y) ==
			    COW_TITLEBAR_HIDDEN_PANES);
		}
		badge.cfg.titlebar.position = COW_TITLEBAR_TOP;
		badge.content_width = 200;
		struct cow_box tight = cow_titlebar_squeezed_rect(&badge);
		for (int x = 0; x < tight.width; x++)
			CHECK(cow_titlebar_button_at(&badge, x, -10) !=
			    COW_TITLEBAR_HIDDEN_PANES);
		CHECK(cow_wm.pending_focus != &panes[0]);
		CHECK(!cow_container_hides_view(cow_wm.pending_focus));
		CHECK(panes[1].pending_height > row_height);
		CHECK(cow_container_pane_visible(c, cow_view_pane(&panes[0]),
		    true));
		CHECK(panes[0].pending_height == row_height);
		CHECK(cow_container_pane_visible(c, cow_view_pane(&panes[1]),
		    false));
		CHECK(cow_container_hidden_count(c) == 1);
		CHECK(cow_container_pane_visible(c, cow_view_pane(&panes[1]),
		    true));
		CHECK(cow_container_hidden_count(c) == 0);
		CHECK(cow_container_hidden_border_edges(&panes[1]) == 0);
		CHECK(cow_container_pane_visible(c, cow_view_pane(&panes[1]),
		    false));
		CHECK(cow_container_pane_visible(c, cow_view_pane(&panes[2]),
		    false));
		CHECK(!cow_container_pane_visible(c, cow_view_pane(&panes[0]),
		    false));
		CHECK(cow_container_pane_visible(c, cow_view_pane(&panes[1]),
		    true));
		CHECK(cow_container_pane_visible(c, cow_view_pane(&panes[2]),
		    true));
		CHECK(cow_container_zoom(&panes[0], COW_CONTAINER_ZOOM_COLUMN));
		CHECK(!cow_container_rotate(c, 1));
		CHECK(panes[0].pending_height > row_height);
		cow_titlebar_effective_config(&panes[0], &tb);
		CHECK(tb.buttons[6].vector.num_points == 5);
		CHECK(tb.buttons[8].vector.num_points == 5);
		CHECK(tb.buttons[6].active_relief == COW_RELIEF_SUNKEN);
		CHECK(tb.buttons[6].inactive_relief == COW_RELIEF_SUNKEN);
		CHECK(tb.buttons[8].active_relief ==
		    panes[0].cfg.titlebar.buttons[8].active_relief);
		CHECK(panes[0].pending_width == column_width);
		CHECK(cow_container_hides_view(&panes[1]));
		CHECK(!cow_container_hides_view(&panes[2]));
		CHECK(cow_container_zoom(&panes[0], COW_CONTAINER_ZOOM_ALL));
		CHECK(panes[0].pending_width > column_width);
		cow_titlebar_effective_config(&panes[0], &tb);
		CHECK(tb.buttons[8].active_relief == COW_RELIEF_SUNKEN);
		CHECK(tb.buttons[8].inactive_relief == COW_RELIEF_SUNKEN);
		CHECK(tb.buttons[6].active_relief ==
		    panes[0].cfg.titlebar.buttons[6].active_relief);
		CHECK(cow_container_hides_view(&panes[2]));
		CHECK(memcmp(&original, &c->frame, sizeof(original)) == 0);
		CHECK(cow_container_zoom(&panes[0], COW_CONTAINER_ZOOM_ALL));
		CHECK(panes[0].pending_height == row_height);
		cow_titlebar_effective_config(&panes[0], &tb);
		CHECK(tb.buttons[8].active_relief ==
		    panes[0].cfg.titlebar.buttons[8].active_relief);
		CHECK(tb.buttons[6].active_relief ==
		    panes[0].cfg.titlebar.buttons[6].active_relief);
		CHECK(panes[0].pending_width == column_width);
		CHECK(!cow_container_hides_view(&panes[1]));
		CHECK(cow_container_zoom(&panes[0], COW_CONTAINER_ZOOM_COLUMN));
		cow_container_activate(c, panes[1].container_member);
		CHECK(
		    cow_view_pane(&panes[0])->zoom == COW_CONTAINER_ZOOM_NONE);
		CHECK(panes[0].pending_height == row_height);
		CHECK(cow_container_zoom(&panes[0], COW_CONTAINER_ZOOM_ALL));
		cow_container_forget_view(&panes[0]);
		CHECK(!cow_container_hides_view(&panes[1]));
		CHECK(!cow_container_hides_view(&panes[2]));
		cow_container_destroy(c);
	}
	cow_wm.current_output = NULL;
	/* Mouse detachment replaces saved positions with the drop coordinates,
	 * including the destination desk and viewport. No compositor is needed
	 * to queue the seat's op_end request.
	 */
	int sockets[2];
	CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
	struct wl_display *display = wl_display_connect_to_fd(sockets[0]);
	CHECK(display != NULL);
	struct river_seat_v1 *river_seat = (struct river_seat_v1 *)
	    wl_proxy_create((struct wl_proxy *)display,
		&river_seat_v1_interface);
	CHECK(river_seat != NULL);
	for (int scenario = 0; scenario < 4; scenario++) {
		int cross_output = scenario % 2;
		bool pane_drag = scenario >= 2;
		struct cow_output outputs[2]  = { 0 };
		struct cow_desk	  desks[2]    = { 0 };
		struct cow_view	  windows[2]  = { 0 };
		struct cow_config drag_config = { .clamp_mode = COW_CLAMP_NONE,
			.page_cols			      = 3,
			.page_rows			      = 3,
			.edge_scroll			      = true,
			.page_visibility = COW_PAGE_VISIBILITY_ORIGIN };
		wl_list_init(&drag_config.icon_placement_overrides);
		cow_config = &drag_config;
		wl_list_init(&cow_wm.outputs);
		wl_list_init(&cow_wm.views);
		for (int i = 0; i < 2; i++) {
			outputs[i].x		= i * 1600;
			outputs[i].width	= 1600;
			outputs[i].height	= 900;
			outputs[i].current_desk = &desks[i];
			wl_list_init(&outputs[i].visible_views);
			wl_list_init(&outputs[i].desk_set.desks);
			wl_list_init(&desks[i].views);
			desks[i].nr	 = i;
			desks[i].page_vx = 3200;
			desks[i].page_vy = 900;
			wl_list_insert(&outputs[i].desk_set.desks,
			    &desks[i].set_link);
			wl_list_insert(&cow_wm.outputs, &outputs[i].wm_link);
			windows[i].output	  = &outputs[0];
			windows[i].desk		  = &desks[0];
			windows[i].has_dimensions = true;
			windows[i].content_width  = 400;
			windows[i].content_height = 300;
			windows[i].x		  = 100;
			windows[i].y		  = 100;
			wl_list_insert(&cow_wm.views, &windows[i].wm_link);
			wl_list_insert(&desks[0].views, &windows[i].desk_views);
			wl_list_insert(&outputs[0].visible_views,
			    &windows[i].output_views);
		}
		c = cow_container_create(&windows[0], &windows[1]);
		if (pane_drag)
			CHECK(cow_container_split(&windows[1], &windows[0],
			    COW_CONTAINER_VERTICAL, false));
		if (!cross_output) {
			wl_list_remove(&desks[1].set_link);
			wl_list_insert(&outputs[0].desk_set.desks,
			    &desks[1].set_link);
			outputs[0].current_desk = &desks[1];
		}
		struct cow_seat drag = {
			.river_seat	      = river_seat,
			.op_in_progress	      = true,
			.op_released	      = true,
			.container_drag	      = true,
			.pane_drag = pane_drag,
			.has_pointer_position = true,
			.pointer_x	      = cross_output ? 1650 : 800,
			.pointer_y	      = 450,
			.op		      = { .type = COW_POINTER_OP_MOVE,
						 .view	= &windows[0] }
		};
		if (pane_drag) {
			/* A click without a drop target inside the layout cancels,
			 * as does Escape even when the pointer is outside.
			 */
			struct cow_seat cancelled = drag;
			/* An interrupted button press must not survive operation cleanup.
			 */
			windows[0].pressed_button = 4;
			cancelled.pressed_view = &windows[0];
			cancelled.pressed_button_idx = 4;
			cancelled.titlebar_press = COW_TITLEBAR_PRESS_BUTTON;
			cancelled.pointer_x = c->frame.x + c->frame.width / 2;
			cancelled.pointer_y = c->frame.y + c->frame.height / 2;
			cow_pointer_op_process(&cancelled);
			CHECK(windows[0].container_member != NULL);
			CHECK(cancelled.op.cancelled);
			CHECK(windows[0].pressed_button == -1);
			CHECK(cancelled.pressed_view == NULL);
			CHECK(cancelled.pressed_button_idx == -1);
			cancelled = drag;
			cancelled.op.cancelled = true;
			cow_pointer_op_process(&cancelled);
			CHECK(windows[0].container_member != NULL);
		}
		windows[0].proposed_width = 123;
		cow_pointer_op_process(&drag);
		CHECK(windows[0].container_member == NULL);
		CHECK(windows[0].output == &outputs[cross_output]);
		CHECK(windows[0].desk == &desks[1]);
		CHECK(windows[0].pending_width == 400 &&
		    windows[0].pending_height == 300);
		CHECK(windows[0].x == (cross_output ? 1600 : 600) &&
		    windows[0].y == 300);
		CHECK(windows[0].pending_x == windows[0].x &&
		    windows[0].pending_y == windows[0].y);
		CHECK(windows[0].vx == (cross_output ? 3200 : 3800) &&
		    windows[0].vy == 1200);
		CHECK(wl_list_empty(&cow_wm.containers));
	}
	/* Fixed module slots share container lifetime and visibility without
	 * replacing the host with a synthetic frame.
	 */
	{
		cow_config		 = NULL;
		struct cow_output output = { 0 };
		struct cow_desk	  desk	 = { 0 };
		wl_list_init(&output.visible_views);
		wl_list_init(&desk.views);
		wl_list_init(&cow_wm.views);
		struct cow_view windows[3] = { 0 };
		char	       *ids[]	   = { "host", "first", "second" };
		for (int i = 0; i < 3; i++) {
			windows[i].identifier	  = ids[i];
			windows[i].output	  = &output;
			windows[i].desk		  = &desk;
			windows[i].x		  = 100;
			windows[i].y		  = 200;
			windows[i].content_width  = 120;
			windows[i].content_height = 30;
			windows[i].use_ssd	  = true;
			wl_list_init(&windows[i].selections);
			wl_list_insert(&cow_wm.views, &windows[i].wm_link);
			wl_list_insert(&desk.views, &windows[i].desk_views);
			wl_list_insert(&output.visible_views,
			    &windows[i].output_views);
		}
		struct cow_box slot = { 120, 0, 200, 150 };
		CHECK(cow_container_attach(&windows[0], &windows[1], "first",
		    slot, true, true));
		c = cow_container_window_owner(&windows[0]);
		CHECK(c && c->attached && !c->frame_window);
		CHECK(
		    windows[1].pending_x == 220 && windows[1].pending_y == 200);
		CHECK(windows[1].pending_width == 200 &&
		    windows[1].pending_height == 150);
		CHECK(!windows[1].use_ssd);
		CHECK(cow_container_is_attached_client(&windows[1]));
		CHECK(!cow_container_is_attached_client(&windows[0]));
		cow_container_remove_view(&windows[1]);
		CHECK(windows[1].container_member != NULL);
		struct cow_seat attached_seat = { 0 };
		cow_pointer_op_start_move(&attached_seat, &windows[1]);
		CHECK(!attached_seat.op_in_progress);
		CHECK(!cow_pointer_op_start_resize(&attached_seat, &windows[1],
		    COW_EDGE_RIGHT, false));
		cow_wm.pending_focus = &windows[1];
		CHECK(cow_container_attach(&windows[0], NULL, "first", slot,
		    false, true));
		CHECK(!windows[1].pending_close);
		CHECK(cow_container_hides_view(&windows[1]));
		CHECK(cow_wm.pending_focus == &windows[0]);
		slot = (struct cow_box) { 0, -100, 120, 100 };
		CHECK(cow_container_attach(&windows[0], &windows[2], "second",
		    slot, true, false));
		CHECK(!cow_container_attach(&windows[0], &windows[1], "second",
		    slot, true, false));
		cow_container_activate(c, windows[2].container_member);
		CHECK(!windows[2].use_ssd);
		uint32_t sticky = COW_VIEW_STICKY_DESK_FLAG |
		    COW_VIEW_STICKY_PAGE_FLAG;
		for (unsigned state = 0; state < 4; state++) {
			windows[0].flags &= ~sticky;
			if (state & 1)
				windows[0].flags |= COW_VIEW_STICKY_DESK_FLAG;
			if (state & 2)
				windows[0].flags |= COW_VIEW_STICKY_PAGE_FLAG;
			cow_container_attachment_sync(c);
			for (int i = 1; i < 3; i++)
				CHECK((windows[i].flags & sticky) ==
				    (windows[0].flags & sticky));
		}
		windows[0].x += 50;
		cow_container_attachment_sync(c);
		CHECK(
		    windows[2].pending_x == 150 && windows[2].pending_y == 100);
		cJSON *saved = cow_container_save(c);
		cow_container_destroy(c);
		c = cow_container_restore(saved);
		CHECK(c && c->attached && c->frame_view == &windows[0]);
		CHECK(cow_container_hides_view(&windows[1]));
		CHECK(!windows[2].use_ssd);
		CHECK(!windows[1].pending_close);
		cJSON_Delete(saved);
		cow_container_window_forget(&windows[0]);
		CHECK(windows[1].pending_close);
		CHECK(!windows[2].pending_close);
		CHECK(!windows[0].container_owner &&
		    !windows[1].container_member);
		CHECK(windows[1].use_ssd && windows[1].clip_w == 0);
		CHECK(!windows[1].closed && !windows[2].closed);
		CHECK(wl_list_empty(&cow_wm.containers));
		wl_list_init(&cow_wm.views);
		cow_wm.pending_focus = cow_wm.focused_view = NULL;
	}
	cow_config = NULL;
	wl_proxy_destroy((struct wl_proxy *)river_seat);
	wl_display_disconnect(display);
	close(sockets[1]);
	return 0;
}
