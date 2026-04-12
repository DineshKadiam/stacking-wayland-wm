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
#include "cow/view.h"

#include <cairo/cairo.h>
#include <fnmatch.h>
#include <pango/pangocairo.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cow/colour.h"
#include "cow/compat.h"
#include "cow/config.h"
#include "cow/decoration.h"
#include "cow/desk_ops.h"
#include "cow/keybinding.h"
#include "cow/memory.h"
#include "cow/output.h"
#include "cow/pointer_op.h"
#include "cow/seat.h"
#include "cow/shm.h"
#include "cow/status.h"
#include "cow/wm.h"

#include "river-window-management-v1-client-protocol.h"

int next_container_id;

/*
 * river_window_v1 event handlers
 */

static void
window_handle_closed(void *data, __unused struct river_window_v1 *rw)
{
	struct cow_view *view = data;

	fprintf(stderr, "cow: window closed (app_id=%s)\n",
	    view->app_id ? view->app_id : "(null)");
	view->closed = true;
}

static void
window_handle_dimensions_hint(void *data, __unused struct river_window_v1 *rw,
    int32_t min_width, int32_t min_height, int32_t max_width,
    int32_t max_height)
{
	struct cow_view *view = data;
	view->min_width	      = min_width;
	view->min_height      = min_height;
	view->max_width	      = max_width;
	view->max_height      = max_height;
}

static void
window_handle_dimensions(void *data, __unused struct river_window_v1 *rw,
    int32_t width, int32_t height)
{
	struct cow_view *view = data;

	/* Mark decoration dirty if dimensions changed so the frame
	 * gets redrawn on the next render cycle. */
	if (view->content_width != width || view->content_height != height) {
		view->dec.dirty		  = true;
		cow_wm.needs_manage_dirty = true;
	}

	view->content_width  = width;
	view->content_height = height;
	view->has_dimensions = true;
}

static void
window_handle_app_id(void *data, __unused struct river_window_v1 *rw,
    const char *app_id)
{
	struct cow_view *view = data;

	xfree(view->app_id);
	view->app_id = app_id ? xstrdup(app_id) : NULL;

	(void)app_id;
}

static void
window_handle_title(void *data, __unused struct river_window_v1 *rw,
    const char *title)
{
	struct cow_view *view = data;

	xfree(view->title);
	view->title = title ? xstrdup(title) : NULL;
	view->dec.dirty		  = true;
	cow_wm.needs_manage_dirty = true;
	cow_status_emit();
}

static void
window_handle_parent(void *data, __unused struct river_window_v1 *rw,
    struct river_window_v1 *parent)
{
	struct cow_view *view = data;

	if (parent != NULL) {
		view->parent = cow_view_find(parent);
	} else {
		view->parent = NULL;
	}
}

static void
window_handle_decoration_hint(void *data, __unused struct river_window_v1 *rw,
    uint32_t hint)
{
	struct cow_view *view = data;
	view->decoration_hint = hint;
}

static void
window_handle_pointer_move_requested(void *data,
    __unused struct river_window_v1 *rw, struct river_seat_v1 *rs)
{
	struct cow_view *view = data;
	struct cow_seat *seat = cow_seat_find(rs);
	if (seat != NULL)
		cow_pointer_op_start_move(seat, view);
}

static void
window_handle_pointer_resize_requested(void *data,
    __unused struct river_window_v1 *rw, struct river_seat_v1 *rs,
    uint32_t edges)
{
	struct cow_view *view = data;
	struct cow_seat *seat = cow_seat_find(rs);
	if (seat != NULL)
		cow_pointer_op_start_resize(seat, view, edges);
}

static void
window_handle_show_window_menu_requested(__unused void *data,
    __unused struct river_window_v1 *rw, __unused int32_t x, __unused int32_t y)
{
	return;
}

static void
window_handle_maximize_requested(void *data,
    __unused struct river_window_v1   *rw)
{
	struct cow_view *view = data;

	if (!view->maximized)
		cow_view_maximize(view);
}

static void
window_handle_unmaximize_requested(void *data,
    __unused struct river_window_v1	*rw)
{
	struct cow_view *view = data;
	if (view->maximized)
		cow_view_unmaximize(view);
}

static void
window_handle_fullscreen_requested(void *data,
    __unused struct river_window_v1	*rw,
    __unused struct river_output_v1	*output)
{
	struct cow_view *view = data;
	if (!view->fullscreen)
		cow_view_fullscreen(view);
}

static void
window_handle_exit_fullscreen_requested(void *data,
    __unused struct river_window_v1	     *rw)
{
	struct cow_view *view = data;
	if (view->fullscreen)
		cow_view_exit_fullscreen(view);
}

static void
window_handle_minimize_requested(__unused void *data,
    __unused struct river_window_v1	       *rw)
{
	/* TODO: hide the window */
}

static void
window_handle_unreliable_pid(__unused void *data,
    __unused struct river_window_v1 *rw, __unused int32_t pid)
{
}

static void
window_handle_presentation_hint(__unused void *data,
    __unused struct river_window_v1 *rw, __unused uint32_t hint)
{
}

static void
window_handle_identifier(void *data, __unused struct river_window_v1 *rw,
    const char *identifier)
{
	struct cow_view *view = data;
	xfree(view->identifier);
	view->identifier = identifier ? xstrdup(identifier) : NULL;
}

static const struct river_window_v1_listener window_listener = {
	.closed			    = window_handle_closed,
	.dimensions_hint	    = window_handle_dimensions_hint,
	.dimensions		    = window_handle_dimensions,
	.app_id			    = window_handle_app_id,
	.title			    = window_handle_title,
	.parent			    = window_handle_parent,
	.decoration_hint	    = window_handle_decoration_hint,
	.pointer_move_requested	    = window_handle_pointer_move_requested,
	.pointer_resize_requested   = window_handle_pointer_resize_requested,
	.show_window_menu_requested = window_handle_show_window_menu_requested,
	.maximize_requested	    = window_handle_maximize_requested,
	.unmaximize_requested	    = window_handle_unmaximize_requested,
	.fullscreen_requested	    = window_handle_fullscreen_requested,
	.exit_fullscreen_requested  = window_handle_exit_fullscreen_requested,
	.minimize_requested	    = window_handle_minimize_requested,
	.unreliable_pid		    = window_handle_unreliable_pid,
	.presentation_hint	    = window_handle_presentation_hint,
	.identifier		    = window_handle_identifier,
};

struct cow_view *
cow_view_create(struct river_window_v1 *rw)
{
	struct cow_view *view = xcalloc(1, sizeof(*view));

	view->river_window	 = rw;
	view->node		 = NULL;
	view->decoration_surface = NULL;
	view->decoration	 = NULL;
	view->identifier	 = NULL;
	view->app_id		 = NULL;
	view->title		 = NULL;
	view->parent		 = NULL;
	view->has_dimensions	 = false;
	view->closed		 = false;
	view->configured	 = false;
view->fullscreen	 = false;
	view->maximized		 = false;
	view->flags		 = 0;

	/* Default: use SSD unless the window only supports CSD */
	view->use_ssd		   = true;
	view->title_pressed	   = false;
	view->pressed_button	   = -1;
	view->icon_inverted_button = -1;

	cow_decoration_init(&view->dec);

	/* Assign to the current output and its active sheet */
	if (cow_wm.current_output != NULL) {
		view->output = cow_wm.current_output;
		view->desk   = cow_wm.current_output->current_desk;
	}

	wl_list_init(&view->desk_views);
	wl_list_init(&view->output_views);

	river_window_v1_add_listener(rw, &window_listener, view);

	wl_list_insert(&cow_wm.views, &view->wm_link);
	cow_status_emit();

	return view;
}
void
cow_view_destroy(struct cow_view *view)
{
	cow_status_emit();

	/* Clear drag-drop references before freeing */
	if (cow_wm.drag_drop_target == view)
		cow_wm.drag_drop_target = NULL;
	if (cow_wm.drag_drop_source == view)
		cow_wm.drag_drop_source = NULL;

	/* Detach from container without cascade (WM is shutting down) */
	if (view->container_member != NULL) {
		struct cow_container_member *m = view->container_member;
		struct cow_container	    *c = m->container;
		wl_list_remove(&m->link);
		c->n_members--;
		view->container_member = NULL;
		xfree(m);
		if (c->n_members == 0) {
			wl_list_remove(&c->wm_link);
			xfree(c);
		} else if (c->active == m) {
			c->active = wl_container_of(c->members.next, c->active,
			    link);
			c->active->view->use_ssd   = true;
			c->active->view->dec.dirty = true;
		}
	}

	wl_list_remove(&view->wm_link);
	wl_list_remove(&view->desk_views);
	wl_list_remove(&view->output_views);

	if (cow_wm.focused_view == view)
		cow_wm.focused_view = NULL;
	cow_decoration_fini(&view->dec);

	if (view->node != NULL)
		river_node_v1_destroy(view->node);
	if (view->decoration != NULL)
		river_decoration_v1_destroy(view->decoration);
	if (view->decoration_surface != NULL)
		wl_surface_destroy(view->decoration_surface);

	river_window_v1_destroy(view->river_window);

	xfree(view->identifier);
	xfree(view->app_id);
	xfree(view->title);
	xfree(view);
}

void
cow_view_configure(struct cow_view *view)
{
	if (view->configured)
		return;

	/* Always use SSD -- the WM controls decorations, not the client.
	 * We tell the window to use SSD regardless of its preference.
	 * Windows that "only support CSD" will just draw their own
	 * decorations inside the content area, and we draw ours outside. */
	river_window_v1_use_ssd(view->river_window);
	view->use_ssd = true;

	/* Set capabilities */
	river_window_v1_set_capabilities(view->river_window,
	    RIVER_WINDOW_V1_CAPABILITIES_MAXIMIZE |
		RIVER_WINDOW_V1_CAPABILITIES_FULLSCREEN |
		RIVER_WINDOW_V1_CAPABILITIES_MINIMIZE |
		RIVER_WINDOW_V1_CAPABILITIES_WINDOW_MENU);

	/* Apply per-app-id view config rules */
	if (cow_config != NULL) {
		struct cow_view_config_entry *vc =
		    cow_config_find_view(cow_config, view->app_id, view->title,
			view->output, view->desk);
		if (vc != NULL) {
			if (vc->assign_desk_nr >= 0 && view->output != NULL) {
				cow_view_assign_desk(view,
				    (uint8_t)vc->assign_desk_nr);
			}
			if (vc->sticky)
				view->flags |= COW_VIEW_STICKY_FLAG;
		}
	}

	/* Resolve visual style from global config + matching rules */
	cow_view_resolve_style(view);

	/*
	 * Trigger River's .ready -> .initialized state transition by calling
	 * propose_dimensions.  Without this call, River never sends the first
	 * configure and the window stays invisible.
	 *
	 * Proposing 0x0 means "no preference":
	 * - XDG shell apps (Chrome, pavucontrol): receive configure(0, 0)
	 *   which means "choose your own size" per the XDG shell spec.
	 * - XWayland apps (xterm): River substitutes the X11 surface's own
	 *   geometry for 0, so xterm -g 80x24 is honoured.
	 */
	river_window_v1_propose_dimensions(view->river_window, 0, 0);

	view->configured = true;

	/* Focus and raise new windows on the next manage cycle */
	view->needs_raise	  = true;
	view->needs_focus	  = true;
	cow_wm.needs_manage_dirty = true;
}

void
cow_view_apply_render(struct cow_view *view)
{
	if (!view->has_dimensions || view->closed)
		return;

	/*
	 * Deferred decoration cleanup: if a view lost SSD ownership
	 * (e.g. a container tab was deactivated) its decoration surface
	 * must stay alive until this render cycle so that any pending
	 * window_interaction events are still routed to a valid surface.
	 * Destroy it now, before hide/show, so the compositor can release
	 * the surface atomically with the render_finish.
	 */
	if (!view->use_ssd && view->decoration != NULL) {
		river_decoration_v1_destroy(view->decoration);
		view->decoration = NULL;
		wl_surface_destroy(view->decoration_surface);
		view->decoration_surface = NULL;
		cow_decoration_fini(&view->dec);
		cow_decoration_init(&view->dec);
	}

	/* Get the render node if we don't have one yet */
	if (view->node == NULL) {
		view->node = river_window_v1_get_node(view->river_window);
	}

	/* Initial window placement based on the configured strategy. */
	if (view->x == 0 && view->y == 0 && view->output != NULL) {
		/* Place on the output where the pointer is, not
		 * necessarily the output that created the view. */
		struct cow_seat *ps;
		wl_list_for_each(ps, &cow_wm.seats, wm_link)
		{
			if (ps->has_pointer_position) {
				struct cow_output *ptr_out =
				    cow_output_at(ps->pointer_x, ps->pointer_y);
				if (ptr_out != NULL)
					cow_view_reassign_output(view, ptr_out);
				break;
			}
		}

		struct cow_box *ua	= &view->output->usable_area;
		int		gap	= cow_config ? cow_config->gap : 5;
		int		dec_top = view->dec.border_width +
		    view->dec.title_thickness;
		int			dec_left = view->dec.border_width;
		enum cow_placement_mode mode	 = cow_config ?
		    cow_config->placement :
		    COW_PLACEMENT_CASCADE;

		switch (mode) {
		case COW_PLACEMENT_UNDERMOUSE: {
			/* Centre the window on the pointer position
			 * (like FVWM's PositionPlacement UnderMouse). */
			struct cow_seat *seat;
			int		 px = ua->x + ua->width / 2;
			int		 py = ua->y + ua->height / 2;
			wl_list_for_each(seat, &cow_wm.seats, wm_link)
			{
				if (seat->has_pointer_position) {
					px = seat->pointer_x;
					py = seat->pointer_y;
					break;
				}
			}
			int fw = view->content_width +
			    2 * view->dec.border_width;
			int fh = view->content_height + dec_top +
			    view->dec.border_width;
			view->x = px - fw / 2 + dec_left;
			view->y = py - fh / 2 + dec_top;
			break;
		}
		case COW_PLACEMENT_CENTRE: {
			/* Centre the window in the usable area. */
			int fw = view->content_width +
			    2 * view->dec.border_width;
			int fh = view->content_height + dec_top +
			    view->dec.border_width;
			view->x = ua->x + (ua->width - fw) / 2 + dec_left;
			view->y = ua->y + (ua->height - fh) / 2 + dec_top;
			break;
		}
		case COW_PLACEMENT_CASCADE:
		default: {
			static int cascade_offset = 0;
			view->x = ua->x + dec_left + gap + cascade_offset;
			view->y = ua->y + dec_top + gap + cascade_offset;
			cascade_offset = (cascade_offset + 30) % 150;
			break;
		}
		}
	}

	/* Ensure non-maximized, non-fullscreen windows don't overlap
	 * the reserved area (e.g. after WM restart with new config). */
	if (view->output != NULL && !view->maximized && !view->fullscreen) {
		struct cow_box *ua	= &view->output->usable_area;
		int		dec_top = view->dec.border_width +
		    view->dec.title_thickness;
		int dec_left = view->dec.border_width;
		if (view->x < ua->x + dec_left)
			view->x = ua->x + dec_left;
		if (view->y < ua->y + dec_top)
			view->y = ua->y + dec_top;
	}

	/*
	 * Keep vx/vy (virtual position) in sync with x/y (global render
	 * position) while the page grid is 1x1 and page_vx=page_vy=0.
	 * In Stage 4 this will be reversed: vx/vy become primary and x/y
	 * are derived from them via:  x = output->x + (vx - page_vx).
	 */
	if (view->output != NULL && view->desk != NULL) {
		view->vx = view->x - view->output->x + view->desk->page_vx;
		view->vy = view->y - view->output->y + view->desk->page_vy;
	}

	river_node_v1_set_position(view->node, view->x, view->y);

	/* Raise if requested (from focus change) */
	if (view->needs_raise && view->node != NULL) {
		river_node_v1_place_top(view->node);
		view->needs_raise = false;
	}

	/* Lower if requested (from IPC) */
	if (view->pending_lower && view->node != NULL) {
		river_node_v1_place_bottom(view->node);
		view->pending_lower = false;
	}

	/*
	 * For contained views, pin the node to the container's position.
	 * propose_dimensions for out-of-sync members is handled by the
	 * pending_resize mechanism in the manage sequence (via
	 * view_sync_to_container → cow_view_set_pending_geometry), NOT here.
	 * Calling river_window_v1_propose_dimensions from the render sequence
	 * is a sequence_order protocol violation.
	 */
	if (view->container_member != NULL) {
		struct cow_container *c = view->container_member->container;
		river_node_v1_set_position(view->node, c->x, c->y);
	}

	/* Show or hide based on desk membership + sticky + iconified */
	if (view->output == NULL || !cow_view_visible(view, view->output)) {
		river_window_v1_hide(view->river_window);
	} else {
		river_window_v1_show(view->river_window);

		/* Z-ordering: ensure all container members stack together */
		if (view->container_member != NULL && view->needs_raise) {
			struct cow_container *c =
			    view->container_member->container;
			struct cow_container_member *active = c->active;
			if (active != NULL && active->view->node != NULL &&
			    view != active->view) {
				river_node_v1_place_below(view->node,
				    active->view->node);
			}
		}

		/* Update decoration surface (SSD frame) */
		if (view->use_ssd) {
			bool active = (view == cow_wm.focused_view);
			cow_decoration_update(view, active);
		}
	}

	/* Apply pending content clip box (rendering state).
	 * Used by shade to hide window content while keeping
	 * the decoration (titlebar + borders) visible. */
	if (view->pending_clip) {
		river_window_v1_set_content_clip_box(view->river_window,
		    view->clip_x, view->clip_y, view->clip_w, view->clip_h);
		view->pending_clip = false;
	}
}

void
cow_view_reassign_output(struct cow_view *view, struct cow_output *output)
{
	if (output == NULL || output == view->output)
		return;

	/* Remove from old output's sheet and visible list */
	wl_list_remove(&view->desk_views);
	wl_list_remove(&view->output_views);

	/* Assign to new output's current sheet */
	view->output = output;
	view->desk   = output->current_desk;
	wl_list_insert(&output->current_desk->views, &view->desk_views);
	wl_list_insert(&output->visible_views, &view->output_views);

	view->dec.dirty = true;
}

void
cow_view_update_output(struct cow_view *view)
{
	if (view->output == NULL)
		return;

	/* Use the centre of the view's frame to determine which
	 * output it belongs to. */
	int bw = view->dec.border_width;
	int tt = view->dec.title_thickness;
	int cx = view->x + view->content_width / 2;
	int cy = view->y - tt / 2 + view->content_height / 2;

	(void)bw;

	struct cow_output *target = cow_output_at(cx, cy);
	cow_view_reassign_output(view, target);
}

void
cow_view_set_pending_geometry(struct cow_view *view, int x, int y, int w, int h)
{
	view->pending_resize	  = true;
	view->pending_x		  = x;
	view->pending_y		  = y;
	view->pending_width	  = w;
	view->pending_height	  = h;
	view->dec.dirty		  = true;
	cow_wm.needs_manage_dirty = true;
}

struct cow_view *
cow_view_walk(struct cow_view *start, bool forward,
    bool (*pred)(const struct cow_view *))
{
	struct wl_list *head = &cow_wm.views;
	struct wl_list *cur  = &start->wm_link;

	for (;;) {
		cur = forward ? cur->next : cur->prev;
		if (cur == head) {
			/* Skip the list sentinel */
			cur = forward ? cur->next : cur->prev;
		}
		if (cur == &start->wm_link)
			return NULL; /* wrapped around without a match */

		struct cow_view *v = wl_container_of(cur, v, wm_link);
		if (pred(v))
			return v;
	}
}

/* ================================================================
 * View utility functions (moved from header)
 * ================================================================ */

bool
cow_view_focusable(const struct cow_view *v)
{
	return !v->closed && v->has_dimensions &&
	    !(v->flags & COW_VIEW_ICONIFIED_FLAG);
}

void
cow_view_save_geometry(struct cow_view *view)
{
	view->saved_x	   = view->x;
	view->saved_y	   = view->y;
	view->saved_width  = view->content_width;
	view->saved_height = view->content_height;
}

struct cow_box
cow_view_frame_box(const struct cow_view *v)
{
	int bw = v->dec.border_width;
	int tt = v->dec.title_thickness;
	return (struct cow_box) {
		.x	= v->x - bw,
		.y	= v->y - bw - tt,
		.width	= v->content_width + 2 * bw,
		.height = v->content_height + bw + tt + bw,
	};
}

bool
cow_view_is_iconified(const struct cow_view *view)
{
	return (view->flags & COW_VIEW_ICONIFIED_FLAG) != 0;
}

/* ================================================================
 * View style resolution
 * ================================================================ */

void
cow_view_resolve_style(struct cow_view *view)
{
	struct cow_view_style *s = &view->style;

	/* Start from global config defaults */
	s->border		    = cow_config ? cow_config->border : 6;
	s->has_border_override	    = false;
	s->has_titlebar_override    = false;
	s->has_titlebar_fg_override = false;
	if (cow_config != NULL) {
		s->border_active   = cow_config->border_active;
		s->border_inactive = cow_config->border_inactive;
		memcpy(s->titlebar_active, cow_config->titlebar.active_colour,
		    sizeof(float) * 4);
		memcpy(s->titlebar_inactive,
		    cow_config->titlebar.inactive_colour, sizeof(float) * 4);
		memcpy(s->titlebar_fg_active, cow_config->titlebar.fg_active,
		    sizeof(float) * 4);
		memcpy(s->titlebar_fg_inactive,
		    cow_config->titlebar.fg_inactive, sizeof(float) * 4);
		s->titlebar_enabled = cow_config->titlebar.enabled;
	} else {
		float white[4] = { 1, 1, 1, 1 };
		float grey[4]  = { 0.5, 0.5, 0.5, 1 };
		float black[4] = { 0, 0, 0, 1 };
		cow_colour_convert(s->border_active.n, 0xFFFFFF);
		cow_colour_convert(s->border_inactive.n, 0x808080);
		memcpy(s->titlebar_active, white, sizeof(float) * 4);
		memcpy(s->titlebar_inactive, grey, sizeof(float) * 4);
		memcpy(s->titlebar_fg_active, black, sizeof(float) * 4);
		memcpy(s->titlebar_fg_inactive, black, sizeof(float) * 4);
		s->titlebar_enabled = false;
	}

	/* Apply matching rules in order (last match wins for each field) */
	if (cow_config == NULL)
		goto done;

	struct cow_view_config_entry *vc;
	wl_list_for_each(vc, &cow_config->view_configs, link)
	{
		/* Check output/desk constraints */
		if (vc->output_name != NULL && view->output != NULL &&
		    view->output->name != NULL &&
		    strcmp(vc->output_name, view->output->name) != 0)
			continue;
		/* Desk constraint: match against the current desk
		 * being displayed.  All visible windows on this desk
		 * get the desk's style.  For collected windows, use
		 * their assigned desk instead so they retain their
		 * original desk's style. */
		if (vc->desk_nr >= 0 && view->output != NULL) {
			bool collected	= (view->desk != NULL &&
			    view->desk != view->output->current_desk &&
			    (view->output->collected_desks &
				(1u << view->desk->nr)));
			int  match_desk = collected ?
			    view->desk->nr :
			    view->output->current_desk->nr;
			if (match_desk != vc->desk_nr)
				continue;
		}

		/* Check app_id/title match */
		bool match = false;
		if (vc->title != NULL) {
			if (view->title != NULL) {
				if (vc->use_glob)
					match = fnmatch(vc->title, view->title,
						    0) == 0;
				else
					match = strcmp(vc->title,
						    view->title) == 0;
			}
		} else if (vc->app_id != NULL) {
			const char *id = view->app_id ? view->app_id : "";
			if (strcmp(vc->app_id, "*") == 0)
				match = true;
			else if (vc->use_glob)
				match = fnmatch(vc->app_id, id, 0) == 0;
			else
				match = strcmp(vc->app_id, id) == 0;
		}
		if (!match)
			continue;

		/* Apply visual overrides */
		if (vc->has_border)
			s->border = vc->border;
		if (vc->has_border_colour_active) {
			s->border_active       = vc->border_colour_active;
			s->has_border_override = true;
		}
		if (vc->has_border_colour_inactive) {
			s->border_inactive     = vc->border_colour_inactive;
			s->has_border_override = true;
		}
		if (vc->has_titlebar_active) {
			memcpy(s->titlebar_active, vc->titlebar_active_colour,
			    sizeof(float) * 4);
			s->has_titlebar_override = true;
		}
		if (vc->has_titlebar_inactive) {
			memcpy(s->titlebar_inactive,
			    vc->titlebar_inactive_colour, sizeof(float) * 4);
			s->has_titlebar_override = true;
		}
		if (vc->has_titlebar_fg_active) {
			memcpy(s->titlebar_fg_active, vc->titlebar_fg_active,
			    sizeof(float) * 4);
			s->has_titlebar_fg_override = true;
		}
		if (vc->has_titlebar_fg_inactive) {
			memcpy(s->titlebar_fg_inactive,
			    vc->titlebar_fg_inactive, sizeof(float) * 4);
			s->has_titlebar_fg_override = true;
		}
		if (vc->has_titlebar_enabled)
			s->titlebar_enabled = vc->titlebar_enabled;
	}

done:
	s->resolved = true;
}

/* ================================================================
 * View helper functions (public API used by wm.c, cmd-*.c)
 * ================================================================ */

int
cow_find_maximize_button(void)
{
	if (cow_config == NULL)
		return -1;
	for (int i = 0; i < 3; i++) {
		if (cow_config->titlebar.buttons[i].action ==
		    COW_BUTTON_ACTION_MAXIMIZE)
			return i;
	}
	return -1;
}

void
cow_view_maximize(struct cow_view *view)
{
	if (view->output == NULL)
		return;

	view->saved_x	   = view->x;
	view->saved_y	   = view->y;
	view->saved_width  = view->content_width;
	view->saved_height = view->content_height;

	struct cow_box *ua = &view->output->usable_area;
	int		bw = view->dec.border_width;
	int		tt = view->dec.title_thickness;

	view->x = ua->x + bw;
	view->y = ua->y + bw + tt;
	int w	= ua->width - 2 * bw;
	int h	= ua->height - 2 * bw - tt;
	if (w < 50)
		w = 50;
	if (h < 50)
		h = 50;
	river_window_v1_propose_dimensions(view->river_window, w, h);
	river_window_v1_inform_maximized(view->river_window);
	view->maximized = true;

	/*
	 * Sync the container with the maximized geometry.  Use the proposed
	 * w/h directly because the dimensions event hasn't arrived yet and
	 * view->content_width still holds the old value.
	 */
	if (view->container_member != NULL) {
		struct cow_container *c = view->container_member->container;
		c->x			= view->x;
		c->y			= view->y;
		c->content_width	= w;
		c->content_height	= h;
		struct cow_container_member *m;
		wl_list_for_each(m, &c->members, link)
		{
			if (m->view == view)
				continue;
			m->view->x	   = view->x;
			m->view->y	   = view->y;
			m->view->dec.dirty = true;
			cow_view_set_pending_geometry(m->view, view->x, view->y,
			    w, h);
		}
	}

	int mbi = cow_find_maximize_button();
	if (mbi >= 0) {
		view->icon_inverted_button = mbi;
		view->dec.dirty		   = true;
	}
}

void
cow_view_unmaximize(struct cow_view *view)
{
	view->x = view->saved_x;
	view->y = view->saved_y;
	river_window_v1_propose_dimensions(view->river_window,
	    view->saved_width, view->saved_height);
	river_window_v1_inform_unmaximized(view->river_window);
	view->maximized = false;

	/* Restore container geometry to the saved (pre-maximize) values. */
	if (view->container_member != NULL) {
		struct cow_container *c = view->container_member->container;
		c->x			= view->saved_x;
		c->y			= view->saved_y;
		c->content_width	= view->saved_width;
		c->content_height	= view->saved_height;
		struct cow_container_member *m;
		wl_list_for_each(m, &c->members, link)
		{
			if (m->view == view)
				continue;
			m->view->x	   = view->saved_x;
			m->view->y	   = view->saved_y;
			m->view->dec.dirty = true;
			cow_view_set_pending_geometry(m->view, view->saved_x,
			    view->saved_y, view->saved_width,
			    view->saved_height);
		}
	}

	view->icon_inverted_button = -1;
	view->dec.dirty		   = true;
}

void
cow_view_shade(struct cow_view *view, int direction)
{
	if (view->sd != COW_SHADE_NONE) {
		cow_view_unshade(view);
		return;
	}

	if (direction == 0)
		direction = COW_SHADE_NORTH;

	view->shade_saved_width	 = view->content_width;
	view->shade_saved_height = view->content_height;
	view->sd		 = direction;

	/* Clip the window content to 1x1 pixel so it's effectively
	 * invisible.  0x0 disables clipping per the protocol.
	 * Applied during the render callback via pending_clip. */
	view->pending_clip	  = true;
	view->clip_x		  = 0;
	view->clip_y		  = 0;
	view->clip_w		  = 1;
	view->clip_h		  = 1;
	cow_wm.needs_manage_dirty = true;

	view->dec.dirty = true;
}

void
cow_view_unshade(struct cow_view *view)
{
	if (view->sd == COW_SHADE_NONE)
		return;

	int sw = view->shade_saved_width;
	int sh = view->shade_saved_height;

	view->sd = COW_SHADE_NONE;

	view->content_width  = sw;
	view->content_height = sh;

	/* Disable clipping (0 width/height = disabled per protocol). */
	view->pending_clip = true;
	view->clip_x	   = 0;
	view->clip_y	   = 0;
	view->clip_w	   = 0;
	view->clip_h	   = 0;

	view->dec.dirty		      = true;
	view->dec.last_content_width  = -1;
	view->dec.last_content_height = -1;
	if (view->dec.buffer != NULL) {
		cow_shm_buffer_destroy(view->dec.buffer);
		view->dec.buffer = NULL;
	}

	cow_wm.needs_manage_dirty = true;
}

void
cow_view_fullscreen(struct cow_view *view)
{
	if (view->output == NULL)
		return;

	if (!view->fullscreen) {
		view->saved_x	   = view->x;
		view->saved_y	   = view->y;
		view->saved_width  = view->content_width;
		view->saved_height = view->content_height;
	}

	view->x = view->output->x;
	view->y = view->output->y;
	river_window_v1_propose_dimensions(view->river_window,
	    view->output->width, view->output->height);
	river_window_v1_fullscreen(view->river_window,
	    view->output->river_output);
	river_window_v1_inform_fullscreen(view->river_window);
	view->fullscreen = true;
	view->use_ssd	 = false;
}

void
cow_view_exit_fullscreen(struct cow_view *view)
{
	view->x = view->saved_x;
	view->y = view->saved_y;
	river_window_v1_propose_dimensions(view->river_window,
	    view->saved_width, view->saved_height);
	river_window_v1_exit_fullscreen(view->river_window);
	river_window_v1_inform_not_fullscreen(view->river_window);
	view->fullscreen = false;

	if (view->decoration_hint !=
	    RIVER_WINDOW_V1_DECORATION_HINT_ONLY_SUPPORTS_CSD)
		view->use_ssd = true;
}

/* ================================================================
 * Iconify (minimize) support
 * ================================================================ */

/*
 * Compute icon positions for all iconified views on an output.
 * Icons are laid out in a row at the bottom of the usable area.
 */
void
cow_output_layout_icons(struct cow_output *output)
{
	int icon_sz = cow_config ? cow_config->icon_size : 64;
	int icon_h  = icon_sz + 20; /* extra height for text below */
	int icon_w  = icon_sz;
	int gap	    = 8;
	int x	    = output->usable_area.x + gap;
	int y = output->usable_area.y + output->usable_area.height - icon_h -
	    gap;

	struct cow_view *view;
	wl_list_for_each(view, &cow_wm.views, wm_link)
	{
		if (!cow_view_is_iconified(view) || view->closed)
			continue;
		if (view->output != output)
			continue;

		view->icon_x	  = x;
		view->icon_y	  = y;
		view->icon_width  = icon_w;
		view->icon_height = icon_h;

		x += icon_w + gap;
		if (x + icon_w >
		    output->usable_area.x + output->usable_area.width) {
			x = output->usable_area.x + gap;
			y -= icon_h + gap;
		}
	}
}

/*
 * Render the icon surface for an iconified view.
 * Draws a bordered rectangle with the app_id as text.
 */
void
cow_view_icon_render(struct cow_view *view)
{
	if (!cow_view_is_iconified(view) || view->closed)
		return;
	if (view->output == NULL)
		return;

	int icon_w = view->icon_width;
	int icon_h = view->icon_height;
	if (icon_w <= 0 || icon_h <= 0)
		return;

	/* Create surfaces if needed */
	if (view->icon_surface == NULL) {
		view->icon_surface = wl_compositor_create_surface(
		    cow_wm.compositor);
		if (view->icon_surface == NULL)
			return;
	}
	if (view->icon_shell == NULL) {
		view->icon_shell = river_window_manager_v1_get_shell_surface(
		    cow_wm.manager, view->icon_surface);
		if (view->icon_shell == NULL)
			return;
	}
	if (view->icon_node == NULL) {
		view->icon_node = river_shell_surface_v1_get_node(
		    view->icon_shell);
	}

	/* Allocate or reallocate the SHM buffer */
	if (view->icon_buffer != NULL &&
	    (view->icon_buffer->width != icon_w ||
		view->icon_buffer->height != icon_h)) {
		cow_shm_buffer_destroy(view->icon_buffer);
		view->icon_buffer = NULL;
	}
	if (view->icon_buffer == NULL) {
		view->icon_buffer = cow_shm_buffer_create(icon_w, icon_h);
		if (view->icon_buffer == NULL)
			return;
	}

	uint32_t *pixels    = view->icon_buffer->data;
	int	  stride_px = view->icon_buffer->stride / 4;

	/* Clear to transparent */
	memset(pixels, 0, (size_t)stride_px * icon_h * 4);

	/* Use inactive border colors for the icon box */
	struct cow_border_colour *bc	  = &cow_config->border_inactive;
	uint32_t		  px_base = cow_colour_to_pixel(bc->n);
	float			  lt[4], dk[4];
	cow_colour_lighten(lt, bc->n);
	cow_colour_darken(dk, bc->n);
	uint32_t px_lt = cow_colour_to_pixel(lt);
	uint32_t px_dk = cow_colour_to_pixel(dk);

	int icon_sz = cow_config ? cow_config->icon_size : 64;
	int box_x   = (icon_w - icon_sz) / 2;
	int box_y   = 0;
	int bw	    = 3; /* icon border width */

	/* Fill the icon box */
	for (int y = box_y; y < box_y + icon_sz; y++) {
		for (int x = box_x; x < box_x + icon_sz; x++) {
			if (x >= 0 && x < icon_w && y >= 0 && y < icon_h)
				pixels[y * stride_px + x] = px_base;
		}
	}

	/* Draw 3D relief if border_style is set */
	if (cow_config->border_style != COW_BORDER_STYLE_NONE) {
		for (int r = 0; r < bw; r++) {
			/* Top + left = light */
			for (int x = box_x + r; x < box_x + icon_sz - r; x++) {
				if ((box_y + r) < icon_h)
					pixels[(box_y + r) * stride_px + x] =
					    px_lt;
			}
			for (int y = box_y + r; y < box_y + icon_sz - r; y++) {
				if ((box_x + r) < icon_w)
					pixels[y * stride_px + box_x + r] =
					    px_lt;
			}
			/* Bottom + right = dark */
			for (int x = box_x + r; x < box_x + icon_sz - r; x++) {
				int by = box_y + icon_sz - 1 - r;
				if (by < icon_h)
					pixels[by * stride_px + x] = px_dk;
			}
			for (int y = box_y + r; y < box_y + icon_sz - r; y++) {
				int bx = box_x + icon_sz - 1 - r;
				if (bx < icon_w)
					pixels[y * stride_px + bx] = px_dk;
			}
		}
	}

	/* Render app_id text below the icon box using pango */
	const char *label	= view->app_id ? view->app_id : "?";
	int	    text_y	= icon_sz + 2;
	int	    text_area_h = icon_h - text_y;

	if (text_area_h > 0) {
		/* Create a cairo surface over just the text area */
		cairo_surface_t *cs = cairo_image_surface_create_for_data(
		    (unsigned char *)(pixels + text_y * stride_px),
		    CAIRO_FORMAT_ARGB32, icon_w, text_area_h, stride_px * 4);
		cairo_t *cr = cairo_create(cs);

		const char  *fname	 = cow_config && cow_config->font_name ?
		    cow_config->font_name :
		    "monospace 9";
		PangoLayout *layout	 = pango_cairo_create_layout(cr);
		PangoFontDescription *fd = pango_font_description_from_string(
		    fname);
		/* Use a smaller font for the icon label */
		pango_font_description_set_size(fd, 8 * PANGO_SCALE);
		pango_layout_set_font_description(layout, fd);
		pango_layout_set_text(layout, label, -1);
		pango_layout_set_width(layout, icon_w * PANGO_SCALE);
		pango_layout_set_alignment(layout, PANGO_ALIGN_CENTER);
		pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);

		cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 1.0);
		pango_cairo_show_layout(cr, layout);

		g_object_unref(layout);
		pango_font_description_free(fd);
		cairo_destroy(cr);
		cairo_surface_destroy(cs);
	}

	/* Draw stipple on sticky iconified windows --
	 * alternating light/dark pixels */
	if (view->flags & COW_VIEW_STICKY_FLAG) {
		for (int y = 0; y < icon_h; y++) {
			for (int x = 0; x < icon_w; x++) {
				if (pixels[y * stride_px + x] != px_base)
					continue;
				if ((x + y) % 2 == 0)
					pixels[y * stride_px + x] = px_lt;
				else
					pixels[y * stride_px + x] = px_dk;
			}
		}
	}

	/* Commit the buffer */
	wl_surface_attach(view->icon_surface, view->icon_buffer->buffer, 0, 0);
	wl_surface_damage_buffer(view->icon_surface, 0, 0, icon_w, icon_h);
	river_shell_surface_v1_sync_next_commit(view->icon_shell);
	wl_surface_commit(view->icon_surface);

	/* Position the icon */
	river_node_v1_set_position(view->icon_node, view->icon_x, view->icon_y);
}

static void
cow_view_icon_destroy(struct cow_view *view)
{
	if (view->icon_buffer != NULL) {
		cow_shm_buffer_destroy(view->icon_buffer);
		view->icon_buffer = NULL;
	}
	if (view->icon_node != NULL) {
		view->icon_node = NULL;
	}
	if (view->icon_shell != NULL) {
		river_shell_surface_v1_destroy(view->icon_shell);
		view->icon_shell = NULL;
	}
	if (view->icon_surface != NULL) {
		wl_surface_destroy(view->icon_surface);
		view->icon_surface = NULL;
	}
}

void
cow_view_iconify(struct cow_view *view)
{
	if (cow_view_is_iconified(view))
		return;

	/* Save geometry for restore */
	cow_view_save_geometry(view);

	view->flags |= COW_VIEW_ICONIFIED_FLAG;

	/* Layout all icons on this output */
	if (view->output != NULL)
		cow_output_layout_icons(view->output);

	/* Focus next visible view */
	struct cow_view *next = cow_view_walk(view, true, cow_view_focusable);
	if (next != NULL && next != view) {
		cow_wm.pending_focus = next;
	}

	cow_wm.needs_manage_dirty = true;
}

void
cow_view_deiconify(struct cow_view *view)
{
	if (!cow_view_is_iconified(view))
		return;

	view->flags &= ~COW_VIEW_ICONIFIED_FLAG;

	/* Destroy the icon surface */
	cow_view_icon_destroy(view);

	/* Re-layout remaining icons on this output */
	if (view->output != NULL)
		cow_output_layout_icons(view->output);

	/* Focus the restored window */
	view->needs_raise	  = true;
	cow_wm.pending_focus	  = view;
	cow_wm.needs_manage_dirty = true;
}

struct cow_view *
cow_view_find(struct river_window_v1 *river_window)
{
	struct cow_view *view;
	wl_list_for_each(view, &cow_wm.views, wm_link)
	{
		if (view->river_window == river_window)
			return view;
	}
	return NULL;
}

/*
 * Remove a view's decoration surfaces so the container can reassign them.
 */
static void
view_drop_decoration(struct cow_view *view)
{
	if (view->decoration != NULL) {
		river_decoration_v1_destroy(view->decoration);
		view->decoration = NULL;
	}
	if (view->decoration_surface != NULL) {
		wl_surface_destroy(view->decoration_surface);
		view->decoration_surface = NULL;
	}
	cow_decoration_fini(&view->dec);
	cow_decoration_init(&view->dec);
}

/*
 * Assign a view's geometry from its container and queue a pending resize.
 */
static void
view_sync_to_container(struct cow_view *view, struct cow_container *c)
{
	cow_view_set_pending_geometry(view, c->x, c->y, c->content_width,
	    c->content_height);
}

struct cow_container *
cow_container_create(struct cow_view *first, struct cow_view *second)
{
	struct cow_container *c = xcalloc(1, sizeof(*c));

	wl_list_init(&c->members);
	c->n_members = 0;
	c->id	     = next_container_id++;

	/* Geometry, desk and output come from the drop target (first) */
	c->x		  = first->x;
	c->y		  = first->y;
	c->content_width  = first->content_width;
	c->content_height = first->content_height;
	c->desk		  = first->desk;
	c->output	  = first->output;
	c->flags	  = first->flags;

	wl_list_insert(&cow_wm.containers, &c->wm_link);

	cow_container_add(c, first);
	cow_container_add(c, second);

	/* first (the drop target) is the active tab */
	c->active = wl_container_of(c->members.next, c->active, link);

	cow_wm.needs_manage_dirty = true;
	return c;
}

bool
cow_container_add(struct cow_container *c, struct cow_view *view)
{
	if (view->container_member != NULL)
		return false;

	struct cow_container_member *m = xcalloc(1, sizeof(*m));
	m->view			       = view;
	m->container		       = c;
	wl_list_insert(c->members.prev, &m->link); /* append */
	c->n_members++;

	view->container_member = m;

	/* Save pre-container geometry for restoration on detach */
	m->pre_x      = view->x;
	m->pre_y      = view->y;
	m->pre_width  = view->content_width;
	m->pre_height = view->content_height;

	/* Non-active members must not draw their own frame */
	if (c->active != NULL && c->active != m) {
		view->use_ssd = false;
		view_drop_decoration(view);
	}

	/* Sync dimensions to container */
	view_sync_to_container(view, c);
	return true;
}

void
cow_container_activate(struct cow_container *c,
    struct cow_container_member		    *member)
{
	if (c->active == member)
		return;

	/* Deactivate the current active member.
	 * Do NOT destroy the decoration here: the old surface must remain
	 * alive until the render cycle so that window_interaction events
	 * from rapid clicks are still routed to a valid decoration surface.
	 * The render cycle's deferred cleanup (cow_view_apply_render) will
	 * destroy it once the new active member's decoration is ready. */
	if (c->active != NULL) {
		struct cow_view *old = c->active->view;
		old->use_ssd	     = false;
		old->dec.dirty	     = true;
		old->needs_raise     = false;
	}

	c->active		  = member;
	member->view->use_ssd	  = true;
	member->view->dec.dirty	  = true;
	member->view->needs_raise = true;
	cow_wm.pending_focus	  = member->view;
	cow_wm.needs_manage_dirty = true;
}

static void
container_redraw(struct cow_container *c)
{
	if (c->active != NULL && c->active->view->use_ssd)
		c->active->view->dec.dirty = true;
	cow_wm.needs_manage_dirty = true;
}

void
cow_container_swap_next(struct cow_container *c)
{
	if (c->active == NULL)
		return;
	struct wl_list *cur  = &c->active->link;
	struct wl_list *next = cur->next;
	if (next == &c->members)
		return; /* already the last tab */

	/* Remove cur from its current position then re-insert after next */
	wl_list_remove(cur);
	wl_list_insert(next, cur);
	container_redraw(c);
}

void
cow_container_swap_prev(struct cow_container *c)
{
	if (c->active == NULL)
		return;
	struct wl_list *cur  = &c->active->link;
	struct wl_list *prev = cur->prev;
	if (prev == &c->members)
		return; /* already the first tab */

	/* Remove cur then re-insert before prev (i.e. after prev->prev) */
	struct wl_list *before_prev = prev->prev;
	wl_list_remove(cur);
	wl_list_insert(before_prev, cur);
	container_redraw(c);
}

void
cow_container_activate_next(struct cow_container *c)
{
	struct wl_list *next = c->active->link.next;
	if (next == &c->members)
		next = next->next; /* wrap */
	struct cow_container_member *m = wl_container_of(next, m, link);
	cow_container_activate(c, m);
}

void
cow_container_activate_prev(struct cow_container *c)
{
	struct wl_list *prev = c->active->link.prev;
	if (prev == &c->members)
		prev = prev->prev; /* wrap */
	struct cow_container_member *m = wl_container_of(prev, m, link);
	cow_container_activate(c, m);
}

void
cow_container_remove_view(struct cow_view *view)
{
	struct cow_container_member *m = view->container_member;
	if (m == NULL)
		return;

	struct cow_container *c		 = m->container;
	bool		      was_active = (c->active == m);

	wl_list_remove(&m->link);
	c->n_members--;
	view->container_member = NULL;
	int saved_pre_x      = m->pre_x;
	int saved_pre_y      = m->pre_y;
	int saved_pre_width  = m->pre_width;
	int saved_pre_height = m->pre_height;
	xfree(m);
	m = NULL;

	/* Restore independent decoration */
	view->use_ssd = true;
	cow_decoration_init(&view->dec);
	cow_view_resolve_style(view);

	/* Restore pre-container geometry */
	if (saved_pre_width > 0 && saved_pre_height > 0) {
		view->x = saved_pre_x;
		view->y = saved_pre_y;
		river_window_v1_propose_dimensions(view->river_window,
		    saved_pre_width, saved_pre_height);
		view->proposed_width  = saved_pre_width;
		view->proposed_height = saved_pre_height;
	} else {
		view->x = c->x + 30;
		view->y = c->y + 30;
	}
	view->dec.dirty = true;

	if (c->n_members <= 1) {
		/* Disband: release the last member too */
		if (!wl_list_empty(&c->members)) {
			struct cow_container_member *last =
			    wl_container_of(c->members.next, last, link);
			struct cow_view *lv = last->view;
			wl_list_remove(&last->link);
			c->n_members--;
			lv->container_member = NULL;
			xfree(last);
			lv->use_ssd = true;
			cow_decoration_init(&lv->dec);
			cow_view_resolve_style(lv);
			lv->dec.dirty = true;
		}
		wl_list_remove(&c->wm_link);
		xfree(c);
	} else if (was_active) {
		/* Activate the first remaining member */
		struct cow_container_member *next =
		    wl_container_of(c->members.next, next, link);
		c->active = NULL; /* clear first so activate doesn't skip */
		cow_container_activate(c, next);
	}

	cow_wm.needs_manage_dirty = true;
}

void
cow_container_destroy(struct cow_container *c)
{
	struct cow_container_member *m, *tmp;
	wl_list_for_each_safe(m, tmp, &c->members, link)
	{
		m->view->container_member = NULL;
		m->view->use_ssd	  = true;
		wl_list_remove(&m->link);
		xfree(m);
	}
	wl_list_remove(&c->wm_link);
	xfree(c);
}

struct cow_container *
cow_view_container(const struct cow_view *view)
{
	if (view->container_member == NULL)
		return NULL;
	return view->container_member->container;
}

bool
cow_view_is_active_tab(const struct cow_view *view)
{
	if (view->container_member == NULL)
		return true; /* uncontained views are always "active" */
	struct cow_container *c = view->container_member->container;
	return c->active == view->container_member;
}
bool
cow_view_is_drop_target(const struct cow_view *view)
{
	return cow_wm.drag_drop_target == view;
}

void
cow_view_sync_container_pos(struct cow_view *view)
{
	if (view->container_member == NULL)
		return;

	struct cow_container	    *c = view->container_member->container;
	struct cow_container_member *m;

	c->x = view->x;
	c->y = view->y;

	wl_list_for_each(m, &c->members, link)
	{
		if (m->view == view)
			continue;
		m->view->x	   = c->x;
		m->view->y	   = c->y;
		m->view->dec.dirty = true;
	}
}

void
cow_view_sync_container_geometry(struct cow_view *view, int x, int y, int w,
    int h)
{
	if (view->container_member == NULL)
		return;

	struct cow_container	    *c = view->container_member->container;
	struct cow_container_member *m;

	c->x		  = x;
	c->y		  = y;
	c->content_width  = w;
	c->content_height = h;

	wl_list_for_each(m, &c->members, link)
	{
		if (m->view == view)
			continue;
		m->view->x	   = x;
		m->view->y	   = y;
		m->view->dec.dirty = true;
		cow_view_set_pending_geometry(m->view, x, y, w, h);
	}
}

void
view_warp_to_focused(void)
{
        const struct cow_view *v = cow_wm.pending_focus != NULL ?
            cow_wm.pending_focus :
            cow_wm.focused_view;
	int cx, cy;

        if (v == NULL)
          return;

        view_centre(v, &cx, &cy);
	cow_wm.pending_warp	  = true;
	cow_wm.warp_x		  = cx;
	cow_wm.warp_y		  = cy;
	cow_wm.needs_manage_dirty = true;
}

void
view_centre(const struct cow_view *v, int *cx, int *cy)
{
	*cx = v->x + v->content_width / 2;
	*cy = v->y - v->dec.title_thickness / 2 + v->content_height / 2;
}

