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
#include "cow/output.h"
#include "cow/log.h"
#include "cow/pan_frames.h"
#include "cow/pointer_input.h"
#include "cow/pointer_op.h"
#include "cow/seat.h"

#include <stdio.h>
#include <stdlib.h>

#include "cow/compat.h"
#include "cow/config.h"
#include "cow/keybinding.h"
#include "cow/layer_shell.h"
#include "cow/menu.h"
#include "cow/memory.h"
#include "cow/desk_ops.h"
#include "cow/view.h"
#include "cow/update.h"
#include "cow/wm.h"

#include "river-window-management-v1-client-protocol.h"
#include "river-xkb-bindings-v1-client-protocol.h"

/*
 * river_seat_v1 event handlers
 */

static void
seat_handle_removed(void *data, __unused struct river_seat_v1 *river_seat)
{
	struct cow_seat *seat = data;
	cow_log_debug("seat removed");
	seat->removed = true;
}

static void
seat_handle_wl_seat(void *data, __unused struct river_seat_v1 *river_seat,
    uint32_t name)
{
	struct cow_seat *seat = data;
	seat->wl_seat_name    = name;
	cow_log_debug("seat discovered (global=%u)", name);
}

static void
seat_handle_pointer_enter(void *data, __unused struct river_seat_v1 *river_seat,
    struct river_window_v1 *river_window)
{
	struct cow_seat *seat	   = data;
	struct cow_view *view	   = cow_view_find(river_window);
	seat->pointer_entered_view = view;
	cow_keybinding_set_context(seat, COW_BIND_CONTEXT_WINDOW, view);

	/* Cancel any pending edge scroll — pointer is on a window */
	cow_output_pan_pointer_enter();

	/* Sloppy focus: focus the view when the pointer enters it */
	if (cow_config != NULL &&
	    cow_config->focus_mode == COW_FOCUS_MODE_SLOPPY && view != NULL &&
	    !view->closed && view != seat->focused_view) {
		seat->sloppy_focus_view = view;
	}
	cow_update_request(COW_UPDATE_STATUS);
}

static void
seat_handle_pointer_leave(void *data, __unused struct river_seat_v1 *river_seat)
{
	struct cow_seat *seat	   = data;
	struct cow_view *view	   = seat->pointer_entered_view;
	seat->pointer_entered_view = NULL;
	if (view != NULL && seat->binding_context_view == view)
		cow_keybinding_set_context(seat, 0, NULL);
	cow_pointer_cursor_clear();

	/* Arm edge scroll if the pointer is now at a screen edge */
	if (seat->has_pointer_position)
		cow_output_pan_pointer_leave(seat->pointer_x, seat->pointer_y);

	cow_update_request(COW_UPDATE_STATUS);
}

static void
seat_handle_window_interaction(void *data,
    __unused struct river_seat_v1   *river_seat,
    struct river_window_v1	    *river_window)
{
	struct cow_seat *seat = data;

	/*
	 * Clicking outside any menu popup closes all open menus.
	 * river only fires window_interaction when the click lands on
	 * a window (not a shell-surface), so reaching this point with
	 * menus open means the user clicked a window, which dismisses
	 * the menu and lets the click also do its usual focus job.
	 */
	if (cow_wm.open_menus.next != NULL &&
	    !wl_list_empty(&cow_wm.open_menus)) {
		cow_menu_close_all();
	}

	struct cow_view *view = cow_view_find(river_window);
	seat->interacted_view = view;

	/* Focus-follows-click: focus the interacted window */
	if (view != NULL && view != seat->focused_view) {
		seat->previous_focused_view = seat->focused_view;
		seat->focused_view  = view;
		cow_wm.focused_view = view;
		if (view->output != NULL)
			cow_output_set_current(view->output);
	}
}

static void
seat_handle_shell_surface_interaction(void *data,
    __unused struct river_seat_v1	   *river_seat,
    struct river_shell_surface_v1	   *shell_surface)
{
	struct cow_seat *seat = data;
	if (seat->consumed_icon_interaction != NULL &&
	    seat->consumed_icon_interaction->icon_shell == shell_surface) {
		seat->consumed_icon_interaction = NULL;
		return;
	}

	/* Menu popup click?  Walk open menus and flag the click for
	 * cow_menu_process_input() on the next manage cycle. */
	if (cow_wm.open_menus.next != NULL &&
	    !wl_list_empty(&cow_wm.open_menus)) {
		struct cow_menu_popup *mp;
		wl_list_for_each(mp, &cow_wm.open_menus, link)
		{
			if (mp->shell_surface == shell_surface) {
				struct cow_seat *seat = data;
				if (seat != NULL)
					seat->menu_click_pending = true;
				cow_update_request(COW_UPDATE_MANAGE);
				return;
			}
		}
		cow_menu_close_all();
		return;
	}

	/* Defer the default icon action until the following manage sequence.
	 * The icon's wl_pointer.button event may match a local contextual
	 * binding and cancel it before then.
	 */
	struct cow_view *view;
	wl_list_for_each(view, &cow_wm.views, wm_link)
	{
		if (view->icon_shell == shell_surface &&
		    cow_view_is_iconified(view)) {
			seat->pending_icon_interaction = view;
			return;
		}
	}
}

static void
seat_handle_op_delta(void *data, __unused struct river_seat_v1 *river_seat,
    int32_t dx, int32_t dy)
{
	struct cow_seat *seat = data;
	if (seat->cursor_hide_op_active && (dx != 0 || dy != 0))
		cow_wm_set_cursor_hidden(false);
	seat->op_dx	      = dx;
	seat->op_dy	      = dy;
}

static void
seat_handle_op_release(void *data, __unused struct river_seat_v1 *river_seat)
{
	struct cow_seat *seat = data;
	if (seat->cursor_hide_op_active)
		cow_wm_set_cursor_hidden(false);
	seat->op_released     = true;
}

static void
seat_handle_pointer_position(void *data,
    __unused struct river_seat_v1 *river_seat, int32_t x, int32_t y)
{
	struct cow_seat *seat = data;

	seat->pointer_x		   = x;
	seat->pointer_y		   = y;
	seat->has_pointer_position = true;

	/*
	 * Menu hover tracking is driven by the wm main loop's poll
	 * timeout while menus are open, so we don't need to request
	 * a manage cycle here -- the timeout already paces the
	 * cadence at ~30 Hz.
	 */
	cow_pointer_cursor_reconcile_position(x, y);
}

static const struct river_seat_v1_listener seat_listener = {
	.removed		   = seat_handle_removed,
	.wl_seat		   = seat_handle_wl_seat,
	.pointer_enter		   = seat_handle_pointer_enter,
	.pointer_leave		   = seat_handle_pointer_leave,
	.window_interaction	   = seat_handle_window_interaction,
	.shell_surface_interaction = seat_handle_shell_surface_interaction,
	.op_delta		   = seat_handle_op_delta,
	.op_release		   = seat_handle_op_release,
	.pointer_position	   = seat_handle_pointer_position,
};

/*
 * river_xkb_bindings_seat_v1 event: an unbound key was eaten.
 * Used for sheet-assign mode.
 */
static void
xkb_seat_handle_ate_unbound_key(void	       *data,
    __unused struct river_xkb_bindings_seat_v1 *xkb_seat)
{
	struct cow_seat *seat = data;

	if (seat->awaiting_desk_assign) {
		seat->awaiting_desk_assign = false;
	}
}

static void
xkb_seat_handle_modifiers_update(void *data,
    __unused struct river_xkb_bindings_seat_v1 *xkb_seat, uint32_t old,
    uint32_t new)
{
	cow_seat_handle_modifiers_update(data, old, new);
}

static const struct river_xkb_bindings_seat_v1_listener xkb_seat_listener = {
	.ate_unbound_key  = xkb_seat_handle_ate_unbound_key,
	.modifiers_update = xkb_seat_handle_modifiers_update,
};

struct cow_seat *
cow_seat_create(struct river_seat_v1 *river_seat)
{
	struct cow_seat *seat = xcalloc(1, sizeof(*seat));

	seat->river_seat	   = river_seat;
	seat->removed		   = false;
	seat->op_in_progress	   = false;
	seat->op_released	   = false;
	seat->has_pointer_position = false;
	seat->bindings_enabled	   = false;
	seat->binding_context_dirty = true;
	seat->modifier_watch_dirty = true;
	seat->titlebar_press	   = COW_TITLEBAR_PRESS_NONE;
	seat->pressed_button_idx   = -1;
	seat->pressed_view	   = NULL;
	seat->pending_button_click_idx = -1;
	seat->pending_button_click_deadline_ms = -1;

	wl_list_init(&seat->keybindings);
	wl_list_init(&seat->pointer_bindings);

	river_seat_v1_add_listener(river_seat, &seat_listener, seat);

	seat->awaiting_desk_assign = false;

	/* Create xkb bindings seat object for ensure_next_key_eaten */
	if (cow_wm.xkb_bindings != NULL) {
		seat->xkb_seat = river_xkb_bindings_v1_get_seat(
		    cow_wm.xkb_bindings, river_seat);
		river_xkb_bindings_seat_v1_add_listener(seat->xkb_seat,
		    &xkb_seat_listener, seat);
	}

	wl_list_insert(&cow_wm.seats, &seat->wm_link);

	/* Register with layer shell for focus events */
	cow_layer_shell_init_seat(seat);

	/* Register default keybindings */
	cow_seat_register_default_bindings(seat);

	return seat;
}

void
cow_seat_destroy(struct cow_seat *seat)
{
	wl_list_remove(&seat->wm_link);

	cow_pointer_op_disarm_attached(seat);
	cow_seat_destroy_bindings(seat);
	cow_layer_shell_fini_seat(seat);

	if (seat->xkb_seat != NULL)
		river_xkb_bindings_seat_v1_destroy(seat->xkb_seat);

	river_seat_v1_destroy(seat->river_seat);
	xfree(seat);
}

void
cow_seat_focus_view(struct cow_seat *seat, struct cow_view *view)
{
	if (view == NULL || view->closed)
		return;

	river_seat_v1_focus_window(seat->river_seat, view->river_window);
	cow_log_debug("focus window (id=%s app_id=%s raise=%s)",
	    view->identifier ? view->identifier : "unknown",
	    view->app_id ? view->app_id : "unknown",
	    cow_config->raise_on_focus ? "yes" : "no");
	if (seat->focused_view != view)
		seat->previous_focused_view = seat->focused_view;
	seat->focused_view  = view;
	cow_wm.focused_view = view;
	view->needs_raise   = cow_config->raise_on_focus;

	/* Update current output to follow focus */
	if (view->output != NULL)
		cow_output_set_current(view->output);

	cow_update_request(COW_UPDATE_STATUS);
}

void
cow_seat_focus_view_no_raise(struct cow_seat *seat, struct cow_view *view)
{
	if (view == NULL || view->closed)
		return;

	river_seat_v1_focus_window(seat->river_seat, view->river_window);
	cow_log_debug("focus window (id=%s app_id=%s raise=no)",
	    view->identifier ? view->identifier : "unknown",
	    view->app_id ? view->app_id : "unknown");
	if (seat->focused_view != view)
		seat->previous_focused_view = seat->focused_view;
	seat->focused_view  = view;
	cow_wm.focused_view = view;
	view->needs_raise   = false;

	if (view->output != NULL)
		cow_output_set_current(view->output);

	cow_update_request(COW_UPDATE_STATUS);
}

struct cow_seat *
cow_seat_find(struct river_seat_v1 *river_seat)
{
	struct cow_seat *seat;
	wl_list_for_each(seat, &cow_wm.seats, wm_link)
	{
		if (seat->river_seat == river_seat)
			return seat;
	}
	return NULL;
}

struct cow_seat *
cow_seat_primary(void)
{
	if (wl_list_empty(&cow_wm.seats))
		return NULL;

	return wl_container_of(cow_wm.seats.next, (struct cow_seat *)NULL,
	    wm_link);
}

bool
cow_seat_get_pointer(int *x, int *y)
{
	struct cow_seat *seat = cow_seat_first_with_pointer();
	if (seat != NULL) {
		*x = seat->pointer_x;
		*y = seat->pointer_y;
		return true;
	}
	return false;
}

struct cow_seat *
cow_seat_first_with_pointer(void)
{
	struct cow_seat *seat;
	wl_list_for_each(seat, &cow_wm.seats, wm_link)
	{
		if (seat->has_pointer_position)
			return seat;
	}
	return NULL;
}

void
cow_seat_forget_view(struct cow_seat *seat, struct cow_view *view)
{
	if (seat == NULL || view == NULL)
		return;
	if (seat->focused_view == view) {
		river_seat_v1_clear_focus(seat->river_seat);
		seat->focused_view = NULL;
	}
	if (seat->previous_focused_view == view)
		seat->previous_focused_view = NULL;
	if (seat->pointer_entered_view == view)
		seat->pointer_entered_view = NULL;
	if (seat->interacted_view == view)
		seat->interacted_view = NULL;
	if (seat->consumed_titlebar_button_interaction == view)
		seat->consumed_titlebar_button_interaction = NULL;
	if (seat->pending_titlebar_button_press_view == view) {
		seat->pending_titlebar_button_press_view = NULL;
		seat->pending_titlebar_button_release = false;
	}
	if (seat->pending_icon_interaction == view)
		seat->pending_icon_interaction = NULL;
	if (seat->consumed_icon_interaction == view)
		seat->consumed_icon_interaction = NULL;
	if (seat->sloppy_focus_view == view)
		seat->sloppy_focus_view = NULL;
	if (seat->last_click_view == view)
		seat->last_click_view = NULL;
	if (seat->pending_button_click_view == view) {
		seat->pending_button_click_view = NULL;
		seat->pending_button_click_idx = -1;
		seat->pending_button_click_deadline_ms = -1;
	}
	if (seat->pressed_view == view)
		seat->pressed_view = NULL;
	if (seat->op.view == view)
		seat->op.view = NULL;
	if (seat->binding_context_view == view) {
		seat->binding_context_view = NULL;
		seat->binding_context = 0;
		seat->binding_context_dirty = true;
		cow_update_request(COW_UPDATE_MANAGE);
	}
	if (seat->binding_context_deferred_view == view) {
		seat->binding_context_deferred_value = 0;
		seat->binding_context_deferred_view = NULL;
	}
	if (seat->binding_action_view == view)
		seat->binding_action_view = NULL;
	struct cow_keybinding *kb;
	wl_list_for_each(kb, &seat->keybindings, link)
	{
		if (kb->release_action_view == view)
			kb->release_action_view = NULL;
	}
	struct cow_pointer_binding *pb;
	wl_list_for_each(pb, &seat->pointer_bindings, link)
	{
		if (pb->enabled_view == view)
			pb->enabled_view = NULL;
		if (pb->pending_view == view)
			pb->pending_view = NULL;
	}
}
