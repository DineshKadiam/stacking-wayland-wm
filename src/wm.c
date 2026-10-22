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
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <cairo.h>
#include <cJSON.h>
#include <math.h>
#include <event2/event.h>

#include <wayland-client.h>
#include <wayland-cursor.h>

#include "cow/cmd.h"
#include "cow/log.h"
#include "cow/command.h"
#include "cow/config.h"
#include "cow/diagnostics.h"
#include "cow/event.h"
#include "cow/draw_exec.h"
#include "cow/geometry_window.h"
#include "cow/chord_hint.h"
#include "cow/notification.h"
#include "cow/ipc.h"
#include "cow/keybinding.h"
#include "cow/layer_shell.h"
#include "cow/memory.h"
#include "cow/menu.h"
#include "cow/keyboard_layout.h"
#include "cow/virtual_keyboard.h"
#include "cow/desk_ops.h"
#include "cow/output.h"
#include "cow/pan_frames.h"
#include "cow/pick.h"
#include "cow/pointer_input.h"
#include "cow/pointer_op.h"
#include "cow/rule.h"
#include "cow/style.h"
#include "cow/seat.h"
#include "cow/stack.h"
#include "cow/state.h"
#include "cow/status.h"
#include "cow/util.h"
#include "cow/titlebar.h"
#include "cow/shm.h"
#include "cow/view.h"
#include "cow/update.h"
#include "cow/wm.h"
#include "cow/focus-history.h"

#include "cursor-shape-v1-client-protocol.h"
#include "ext-idle-notify-v1-client-protocol.h"

#define COW_RESTART_ENV "COW_RESTARTING"
#include "pointer-constraints-unstable-v1-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "river-layer-shell-v1-client-protocol.h"
#include "river-window-management-v1-client-protocol.h"
#include "river-xkb-bindings-v1-client-protocol.h"
#include "river-xkb-config-v1-client-protocol.h"
#include "viewporter-client-protocol.h"

#define GHOST_RADIUS 24

struct cow_wm cow_wm;

struct cow_view *
cow_wm_effective_focus(void)
{
	if (cow_wm.pending_focus != NULL && !cow_wm.pending_focus->closed)
		return cow_wm.pending_focus;
	return cow_wm.focused_view;
}

/*
 * Signal events only set flags; shutdown and restart run after the loop.
 */
static volatile sig_atomic_t cow_wm_shutdown_requested = 0;
volatile sig_atomic_t cow_wm_restart_requested = 0;

/* Forward declarations for protocol listeners */
static void handle_wm_unavailable(void *, struct river_window_manager_v1 *);
static void handle_wm_finished(void *, struct river_window_manager_v1 *);
static void handle_wm_manage_start(void *, struct river_window_manager_v1 *);
static void handle_wm_render_start(void *, struct river_window_manager_v1 *);
static void drag_ghost_create(struct cow_view *);
static void manage_destroy_removed(void);
static void manage_tick_outputs(void);
static void manage_apply_output_presentation_mode(struct cow_output *);
static void manage_configure_views(void);
static void manage_process_seat(struct cow_seat *);
static void manage_cursor_hide(struct cow_seat *);
static void manage_apply_deferred_views(void);
static void manage_apply_deferred_close(struct cow_view *);
static void manage_apply_deferred_tiled_edges(struct cow_view *);
static void manage_apply_deferred_maximized_hint(struct cow_view *);
static void manage_apply_deferred_resize(struct cow_view *);
static void manage_apply_deferred_position(struct cow_view *, bool);
static void manage_apply_fullscreen_visibility(struct cow_view *);
static void manage_apply_deferred_client_state(struct cow_view *);
static void manage_apply_dimension_bounds(struct cow_view *);
static void manage_apply_focus(void);
static void manage_apply_pending_focus(void);
static void manage_apply_needs_focus(void);
static void manage_apply_seat_refocus(void);
static void manage_apply_warp(void);
static void manage_run_deferred_exec(void);
static void manage_mark_views_dirty(void);
static void drag_ghost_draw(struct cow_shm_buffer *);
static void drag_ghost_position_from_pointer(void);
static void render_outputs(void);
static void render_views(void);
static void render_drag_ghost(void);
static struct cow_view *session_unlock_focus_target(struct cow_seat *);
static struct cow_view *manage_focus_replacement_for_close(struct cow_seat *,
    struct cow_view *);
static void manage_prepare_focus_replacement(struct cow_view *);
static int  manage_click_count(struct cow_seat *, struct cow_view *,
    enum cow_dec_region, struct timespec *);
static void manage_store_click(struct cow_seat *, struct cow_view *,
    enum cow_dec_region, struct timespec *, int);
static bool manage_titlebar_button_press(struct cow_seat *, struct cow_view *,
    int, int);
static void manage_titlebar_button_raw_press(struct cow_seat *);
static void manage_titlebar_button_pending(struct cow_seat *);
static int  manage_next_button_click_ms(void);
static enum cow_dec_region decoration_outer_region(struct cow_view *, int, int,
    int, int);

static void handle_wm_session_locked(void *, struct river_window_manager_v1 *);
static void handle_wm_session_unlocked(void *,
    struct river_window_manager_v1 *);
static void handle_wm_window(void *, struct river_window_manager_v1 *,
    struct river_window_v1 *);
static void handle_wm_output(void *, struct river_window_manager_v1 *,
    struct river_output_v1 *);
static void handle_wm_seat(void *, struct river_window_manager_v1 *,
    struct river_seat_v1 *);

static const struct river_window_manager_v1_listener wm_listener = {
	.unavailable	  = handle_wm_unavailable,
	.finished	  = handle_wm_finished,
	.manage_start	  = handle_wm_manage_start,
	.render_start	  = handle_wm_render_start,
	.session_locked	  = handle_wm_session_locked,
	.session_unlocked = handle_wm_session_unlocked,
	.window		  = handle_wm_window,
	.output		  = handle_wm_output,
	.seat		  = handle_wm_seat,
};

/*
 * wl_output listener: capture the human-readable name.
 */
static void
wl_output_handle_geometry(__unused void *data,
    __unused struct wl_output *wl_output, __unused int32_t x,
    __unused int32_t y, __unused int32_t physical_width,
    __unused int32_t physical_height, __unused int32_t subpixel,
    __unused const char *make, __unused const char *model,
    __unused int32_t transform)
{
	return;
}

static void
wl_output_handle_mode(__unused void *data, __unused struct wl_output *wl_output,
    __unused uint32_t flags, __unused int32_t width, __unused int32_t height,
    __unused int32_t refresh)
{
	return;
}

static void
wl_output_handle_done(__unused void *data, __unused struct wl_output *wl_output)
{
	return;
}

static void
wl_output_handle_scale(__unused void *data,
    __unused struct wl_output *wl_output, __unused int32_t factor)
{
	return;
}

static void
wl_output_handle_name(void *data, __unused struct wl_output *wl_output,
    const char *name)
{
	struct cow_wl_output_entry *entry = data;

	xstrset(&entry->name, name);
	cow_log_info("output discovered (name=%s)", name);

	/* Try to match to an existing cow_output */
	struct cow_output *out;
	wl_list_for_each(out, &cow_wm.outputs, wm_link)
	{
		if (out->wl_output_name == entry->global_name &&
		    out->name == NULL) {
			out->wl_output = entry->wl_output;
			out->name      = xstrdup(name);
			cow_output_name_known(out);
		}
	}
}

static void
wl_output_handle_description(__unused void *data,
    __unused struct wl_output *wl_output, __unused const char *description)
{
	return;
}

static const struct wl_output_listener wl_output_listener = {
	.geometry    = wl_output_handle_geometry,
	.mode	     = wl_output_handle_mode,
	.done	     = wl_output_handle_done,
	.scale	     = wl_output_handle_scale,
	.name	     = wl_output_handle_name,
	.description = wl_output_handle_description,
};

/* -----------------------------------------------------------------------
 * Raw wl_seat listener: track pointer capability for pan frames.
 * ----------------------------------------------------------------------- */

static void
wl_seat_handle_capabilities(__unused void *data, __unused struct wl_seat *seat,
    uint32_t capabilities)
{
	cow_wm.wl_seat_has_pointer = (capabilities &
					 WL_SEAT_CAPABILITY_POINTER) != 0;
	cow_wm.wl_seat_has_keyboard = (capabilities &
					  WL_SEAT_CAPABILITY_KEYBOARD) != 0;

	if (cow_wm.wl_seat_has_pointer && cow_wm.wl_pointer_raw == NULL) {
		pan_frames_init();
		cow_pointer_input_init();
	}

	if (!cow_wm.wl_seat_has_pointer && cow_wm.wl_pointer_raw != NULL) {
		/* Pointer capability removed — release the wl_pointer. */
		if (cow_wm.wl_cursor_shape_dev != NULL) {
			wp_cursor_shape_device_v1_destroy(
			    cow_wm.wl_cursor_shape_dev);
			cow_wm.wl_cursor_shape_dev = NULL;
			cow_wm.wl_cursor_shape	    = 0;
		}
		wl_pointer_destroy(cow_wm.wl_pointer_raw);
		cow_wm.wl_pointer_raw = NULL;
	}
}

static void
wl_seat_handle_name(__unused void *data, __unused struct wl_seat *seat,
    __unused const char *name)
{
}

static const struct wl_seat_listener wl_seat_listener = {
	.capabilities = wl_seat_handle_capabilities,
	.name	      = wl_seat_handle_name,
};

static void
idle_notification_idled(__unused void *data,
    __unused struct ext_idle_notification_v1 *notification)
{
	cow_wm_set_cursor_hidden(true);
}

static void
idle_notification_resumed(__unused void *data,
    __unused struct ext_idle_notification_v1 *notification)
{
	cow_wm_set_cursor_hidden(false);
}

static const struct ext_idle_notification_v1_listener
idle_notification_listener = {
	.idled = idle_notification_idled,
	.resumed = idle_notification_resumed,
};

/*
 * Registry handler: bind river globals.
 */
static void
registry_global(__unused void *data, struct wl_registry *registry,
    uint32_t name, const char *interface, uint32_t version)
{
	if (cow_virtual_keyboard_bind(registry, name, interface))
		return;
	if (strcmp(interface, river_window_manager_v1_interface.name) == 0) {
		cow_wm.manager = wl_registry_bind(registry, name,
		    &river_window_manager_v1_interface, 4);
		river_window_manager_v1_add_listener(cow_wm.manager,
		    &wm_listener, NULL);
	} else if (strcmp(interface, river_xkb_bindings_v1_interface.name) ==
	    0) {
		uint32_t bind_version = version < 3 ? version : 3;
		cow_wm.xkb_bindings = wl_registry_bind(registry, name,
		    &river_xkb_bindings_v1_interface, bind_version);
		cow_wm.xkb_bindings_version = bind_version;
	} else if (strcmp(interface, river_xkb_config_v1_interface.name) == 0) {
		cow_keyboard_layout_bind(registry, name, version);
	} else if (strcmp(interface, river_layer_shell_v1_interface.name) ==
	    0) {
		cow_wm.layer_shell = wl_registry_bind(registry, name,
		    &river_layer_shell_v1_interface, 1);
		struct cow_output *output;
		wl_list_for_each(output, &cow_wm.outputs, wm_link)
			cow_layer_shell_init_output(output);
		struct cow_seat *seat;
		wl_list_for_each(seat, &cow_wm.seats, wm_link)
			cow_layer_shell_init_seat(seat);
	} else if (strcmp(interface, "wl_compositor") == 0) {
		cow_wm.compositor = wl_registry_bind(registry, name,
		    &wl_compositor_interface, 6);
	} else if (strcmp(interface, wp_viewporter_interface.name) == 0) {
		cow_wm.viewporter = wl_registry_bind(registry, name,
		    &wp_viewporter_interface, 1);
	} else if (strcmp(interface, "wl_shm") == 0) {
		cow_wm.shm = wl_registry_bind(registry, name, &wl_shm_interface,
		    1);
	} else if (strcmp(interface, "xdg_wm_base") == 0) {
		cow_container_window_bind(registry, name);
	} else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0 &&
	    version >= 1) {
		cow_wm.wl_layer_shell = wl_registry_bind(registry, name,
		    &zwlr_layer_shell_v1_interface, 1);
	} else if (strcmp(interface, wl_seat_interface.name) == 0 &&
	    version >= 5) {
		cow_wm.wl_seat_raw = wl_registry_bind(registry, name,
		    &wl_seat_interface, 5);
		wl_seat_add_listener(cow_wm.wl_seat_raw, &wl_seat_listener,
		    NULL);
		if (cow_wm.running)
			cow_wm_cursor_hide_configure();
	} else if (strcmp(interface,
		       ext_idle_notifier_v1_interface.name) == 0) {
		cow_wm.idle_notifier_version = version < 2 ? version : 2;
		cow_wm.idle_notifier = wl_registry_bind(registry, name,
		    &ext_idle_notifier_v1_interface,
		    cow_wm.idle_notifier_version);
		if (cow_wm.running)
			cow_wm_cursor_hide_configure();
	} else if (strcmp(interface,
		       zwp_pointer_constraints_v1_interface.name) == 0 &&
	    version >= 1) {
		cow_wm.wl_pointer_constraints = wl_registry_bind(registry, name,
		    &zwp_pointer_constraints_v1_interface, 1);
	} else if (strcmp(interface,
		       wp_cursor_shape_manager_v1_interface.name) == 0 &&
	    version >= 1) {
		cow_wm.wl_cursor_shape_mgr = wl_registry_bind(registry, name,
		    &wp_cursor_shape_manager_v1_interface, 1);
	} else if (strcmp(interface, "wl_output") == 0 && version >= 4) {
		struct cow_wl_output_entry *entry = xcalloc(1, sizeof(*entry));
		entry->wl_output   = wl_registry_bind(registry, name,
		      &wl_output_interface, 4);
		entry->global_name = name;
		wl_output_add_listener(entry->wl_output, &wl_output_listener,
		    entry);
		wl_list_insert(&cow_wm.wl_outputs, &entry->link);
	}
}

static void
registry_global_remove(__unused void *data,
    __unused struct wl_registry *registry, uint32_t name)
{
	cow_virtual_keyboard_remove(name);
}

static const struct wl_registry_listener registry_listener = {
	.global	       = registry_global,
	.global_remove = registry_global_remove,
};

static void
handle_wm_unavailable(__unused void	    *data,
    __unused struct river_window_manager_v1 *manager)
{
	cow_log_error("window management unavailable "
	    "(another WM is already running?)");
	cow_wm.running = false;
}

static void
handle_wm_finished(__unused void	    *data,
    __unused struct river_window_manager_v1 *manager)
{
	cow_wm.running = false;
}

static long
elapsed_ms(struct timespec *a, struct timespec *b)
{
	return (b->tv_sec - a->tv_sec) * 1000 +
	    (b->tv_nsec - a->tv_nsec) / 1000000;
}

static long long
elapsed_us(const struct timespec *a, const struct timespec *b)
{
	return (b->tv_sec - a->tv_sec) * 1000000LL +
	    (b->tv_nsec - a->tv_nsec) / 1000;
}

static void
handle_wm_manage_start(__unused void *data,
    struct river_window_manager_v1   *manager)
{
	struct timespec started, finished;
	clock_gettime(CLOCK_MONOTONIC, &started);
	cow_update_manage_begin();
	cow_wm.sequence = COW_WM_SEQUENCE_MANAGE;

	manage_destroy_removed();
	cow_virtual_keyboard_prepare();
	manage_tick_outputs();
	cow_container_manage_windows();
	manage_configure_views();
	cow_menu_run_deferred_action();

	if (cow_menu_is_open())
		cow_menu_process_input();

	struct cow_seat *s;
	wl_list_for_each(s, &cow_wm.seats, wm_link)
		manage_process_seat(s);

	cow_pick_run_deferred();

	if (cow_wm.exec_pending && !cow_wm.state_restored) {
		cow_wm.state_restored = true;
		cow_state_restore();
	}

	cow_container_manage_windows();
	manage_apply_deferred_views();
	manage_apply_focus();
	cow_keyboard_layout_reconcile_focus();
	manage_apply_warp();

	cow_virtual_keyboard_manage_finish();
	river_window_manager_v1_manage_finish(manager);
	cow_wm.sequence = COW_WM_SEQUENCE_NONE;
	cow_diagnostics_manage_end();
	clock_gettime(CLOCK_MONOTONIC, &finished);
	cow_log_trace("manage cycle end (duration_us=%lld)",
	    elapsed_us(&started, &finished));

	manage_run_deferred_exec();
}

static void
manage_destroy_removed(void)
{
	struct cow_view *view, *view_tmp;
	wl_list_for_each_safe(view, view_tmp, &cow_wm.views, wm_link)
	{
		if (view->closed) {
			manage_prepare_focus_replacement(view);
			cow_view_destroy(view);
		}
	}

	struct cow_output *output, *output_tmp;
	wl_list_for_each_safe(output, output_tmp, &cow_wm.outputs, wm_link)
	{
		if (output->removed)
			cow_output_destroy(output);
	}

	struct cow_seat *seat, *seat_tmp;
	wl_list_for_each_safe(seat, seat_tmp, &cow_wm.seats, wm_link)
	{
		if (seat->removed)
			cow_seat_destroy(seat);
	}
}

static void
manage_tick_outputs(void)
{
	struct cow_output *out;
	wl_list_for_each(out, &cow_wm.outputs, wm_link)
	{
		cow_output_prepare_background(out);
		manage_apply_output_presentation_mode(out);
		cow_page_scroll_tick(out);
		cow_output_pan_frame_tick(out);
	}
}

static void
manage_apply_output_presentation_mode(struct cow_output *output)
{
	uint32_t mode = RIVER_OUTPUT_V1_PRESENTATION_MODE_VSYNC;

	struct cow_view *view;
	wl_list_for_each(view, &cow_wm.views, wm_link)
	{
		if (view->closed || view->output != output ||
		    !cow_view_visible(view, output))
			continue;
		if (view->presentation_hint ==
		    RIVER_OUTPUT_V1_PRESENTATION_MODE_ASYNC) {
			mode = RIVER_OUTPUT_V1_PRESENTATION_MODE_ASYNC;
			break;
		}
	}

	if (output->has_presentation_mode &&
	    output->presentation_mode == mode)
		return;

	river_output_v1_set_presentation_mode(output->river_output, mode);
	output->presentation_mode     = mode;
	output->has_presentation_mode = true;
}

static void
manage_configure_views(void)
{
	struct cow_view *view;
	wl_list_for_each(view, &cow_wm.views, wm_link)
	{
		if (!view->configured && !view->closed)
			cow_view_configure(view);
	}
}

static bool
manage_titlebar_button_fire(struct cow_seat *seat, struct cow_view *view,
    int idx, int clicks)
{
	struct cow_seat *old_seat = cow_wm.current_interaction_seat;
	cow_wm.current_interaction_seat = seat;
	bool fired = cow_dec_binding_fire(view, cow_dec_region_button(idx),
	    clicks);
	cow_wm.current_interaction_seat = old_seat;
	return (fired);
}

static void
manage_titlebar_button_single(struct cow_seat *seat, struct cow_view *view,
    int idx)
{
	manage_titlebar_button_fire(seat, view, idx, 1);
}

static void
manage_titlebar_button_pending_clear(struct cow_seat *seat)
{
	seat->pending_button_click_view = NULL;
	seat->pending_button_click_idx = -1;
	seat->pending_button_click_deadline_ms = -1;
}

static void
manage_titlebar_button_pending(struct cow_seat *seat)
{
	struct cow_view *view = seat->pending_button_click_view;
	if (view == NULL)
		return;
	if (!view->closed && cow_mono_ms() <
	    seat->pending_button_click_deadline_ms)
		return;

	int idx = seat->pending_button_click_idx;
	manage_titlebar_button_pending_clear(seat);
	if (!view->closed)
		manage_titlebar_button_single(seat, view, idx);
}

static void
manage_titlebar_button_click(struct cow_seat *seat, struct cow_view *view,
    int idx)
{
	if (idx == COW_TITLEBAR_HIDDEN_PANES) {
		cow_container_hidden_menu(cow_container_window_owner(view));
		return;
	}
	enum cow_dec_region region = cow_dec_region_button(idx);
	long now = cow_mono_ms();

	if (seat->pending_button_click_view == view &&
	    seat->pending_button_click_idx == idx &&
	    now < seat->pending_button_click_deadline_ms) {
		manage_titlebar_button_pending_clear(seat);
		if (!manage_titlebar_button_fire(seat, view, idx, 2))
			manage_titlebar_button_single(seat, view, idx);
		return;
	}

	/* A click on another button resolves the previous click as single. */
	if (seat->pending_button_click_view != NULL) {
		struct cow_view *pending = seat->pending_button_click_view;
		int pending_idx = seat->pending_button_click_idx;
		manage_titlebar_button_pending_clear(seat);
		if (!pending->closed)
			manage_titlebar_button_single(seat, pending, pending_idx);
	}

	if (cow_dec_binding_for_view(view, region, 2) != NULL) {
		seat->pending_button_click_view = view;
		seat->pending_button_click_idx = idx;
		seat->pending_button_click_deadline_ms = now + DOUBLE_CLICK_MS;
	} else {
		manage_titlebar_button_single(seat, view, idx);
	}
}

static void
manage_titlebar_button_release(struct cow_seat *seat)
{
	if (!seat->op_released ||
	    seat->titlebar_press != COW_TITLEBAR_PRESS_BUTTON)
		return;
	/* Focus and raise the interacted window before running its action. */
	if (seat->consumed_titlebar_button_interaction == seat->pressed_view)
		return;

	struct cow_view *view = seat->pressed_view;
	bool click = false;
	int idx = seat->pressed_button_idx;
	if (view != NULL && !view->closed) {
		if (seat->has_pointer_position) {
			int lx = seat->pointer_x - view->x;
			int ly = seat->pointer_y - view->y;
			click = cow_titlebar_button_at(view, lx, ly) == idx;
		}
	}

	if (seat->op_in_progress) {
		river_seat_v1_op_end(seat->river_seat);
		seat->op_in_progress = false;
	}
	seat->op_released    = false;
	cow_seat_clear_decoration_press(seat);
	cow_pointer_cursor_hold_end();

	/*
	 * Dispatch after ending the pointer operation used to track the button
	 * press.  The binding may start another pointer operation itself, or
	 * open a menu whose action does so later.
	 */
	if (click && !view->closed)
		manage_titlebar_button_click(seat, view, idx);
}

static void
manage_update_container_drag(struct cow_seat *seat)
{
	if (!seat->container_drag || !seat->op_in_progress ||
	    seat->op.type != COW_POINTER_OP_MOVE ||
	    !seat->has_pointer_position || seat->op.view == NULL ||
	    seat->op.view->closed ||
	    (seat->op.view->container_member != NULL && !seat->pane_drag))
		return;

	struct cow_view		  *new_target = seat->pane_drag ?
		      cow_container_pane_drop_target_at(seat->pointer_x, seat->pointer_y,
			  seat->op.view) :
		      cow_container_drop_target_at(seat->pointer_x, seat->pointer_y,
			  seat->op.view);
	struct cow_container_pane *empty = NULL;
	if (!seat->pane_drag && !cow_container_window_owner(seat->op.view))
		cow_container_empty_at(seat->pointer_x, seat->pointer_y,
		    seat->op.view, &empty);
	if (empty != NULL)
		new_target = NULL;
	uint32_t	 edge	    = 0;
	if (new_target != NULL) {
		int x = seat->pointer_x - new_target->x;
		int y = seat->pointer_y - new_target->y;
		int w = new_target->content_width,
		    h = new_target->content_height;
		/* The titlebar and central area join tabs; content edges split.
		 */
		if (!cow_container_window_owner(new_target) &&
		    !cow_container_window_owner(seat->op.view) && x >= 0 &&
		    y >= 0 && x < w && y < h) {
			if (x < w / 4)
				edge = COW_EDGE_LEFT;
			else if (x >= w - w / 4)
				edge = COW_EDGE_RIGHT;
			else if (y < h / 4)
				edge = COW_EDGE_TOP;
			else if (y >= h - h / 4)
				edge = COW_EDGE_BOTTOM;
		}
	}
	if (new_target == cow_wm.drag_drop_target &&
	    edge == cow_wm.drag_drop_edge && empty == cow_wm.drag_drop_pane)
		return;
	if (cow_wm.drag_drop_target != NULL)
		cow_wm.drag_drop_target->dec.dirty = true;
	cow_wm.drag_drop_target = new_target;
	cow_wm.drag_drop_pane	= empty;
	cow_wm.drag_drop_edge	= edge;
	if (cow_wm.drag_drop_target != NULL)
		cow_wm.drag_drop_target->dec.dirty = true;
}

static enum cow_dec_region
corner_region_from_edges(uint32_t edges)
{
	if (edges == (COW_EDGE_TOP | COW_EDGE_LEFT))
		return (COW_DEC_REGION_NW);
	if (edges == (COW_EDGE_TOP | COW_EDGE_RIGHT))
		return (COW_DEC_REGION_NE);
	if (edges == (COW_EDGE_BOTTOM | COW_EDGE_LEFT))
		return (COW_DEC_REGION_SW);
	if (edges == (COW_EDGE_BOTTOM | COW_EDGE_RIGHT))
		return (COW_DEC_REGION_SE);
	return (COW_DEC_REGION_NONE);
}

static uint32_t
decoration_corner_edges(struct cow_view *view, int lx, int ly, int cw, int ch)
{
	int content_x, content_y, frame_w, frame_h;
	cow_titlebar_frame_geometry(view, cw, ch, &frame_w, &frame_h,
	    &content_x, &content_y);
	uint32_t edges = cow_decoration_edges_at(view, lx + content_x,
	    ly + content_y, cw, ch);
	if ((edges & (COW_EDGE_TOP | COW_EDGE_BOTTOM)) == 0 ||
	    (edges & (COW_EDGE_LEFT | COW_EDGE_RIGHT)) == 0)
		return (0);
	return (edges);
}

static bool
manage_container_tab_click(struct cow_seat *seat, struct cow_view *view, int lx,
    int content_w)
{
	int		       btn_s = view->dec.title_thickness;
	struct cow_container *ctr   = cow_view_container(view);
	struct cow_container_pane *pane	 = cow_view_pane(view);

	if (pane == NULL || pane->n_tabs <= 1)
		return (false);

	struct cow_titlebar_config tb;
	cow_titlebar_effective_config(view, &tb);
	int left, right;
	cow_titlebar_button_counts(&tb, &left, &right);
	int strip_x = left * btn_s;
	int strip_w = content_w - (left + right) * btn_s;
	if (strip_w <= 0 || lx < strip_x || lx >= strip_x + strip_w)
		return (false);

	int tab_w = strip_w / pane->n_tabs;
	if (tab_w < 1)
		tab_w = 1;
	int tidx = (lx - strip_x) / tab_w;
	if (tidx < 0)
		tidx = 0;
	if (tidx >= pane->n_tabs)
		tidx = pane->n_tabs - 1;

	struct cow_container_member *tm = NULL;
	int			     ti = 0;
	wl_list_for_each(tm, &pane->tabs, pane_link)
	{
		if (ti++ == tidx)
			break;
	}
	if (tm == NULL || tm == pane->active)
		return (false);

	cow_container_activate(ctr, tm);
	seat->interacted_view = NULL;
	return (true);
}

static int
manage_click_count(struct cow_seat *seat, struct cow_view *view,
    enum cow_dec_region region, struct timespec *now)
{
	clock_gettime(CLOCK_MONOTONIC, now);
	if (seat->last_click_view == view &&
	    seat->last_click_region == (int)region &&
	    elapsed_ms(&seat->last_click_time, now) < DOUBLE_CLICK_MS)
		return (2);
	return (1);
}

static void
manage_store_click(struct cow_seat *seat, struct cow_view *view,
    enum cow_dec_region region, struct timespec *now, int clicks)
{
	if (clicks == 2) {
		seat->last_click_view = NULL;
	} else {
		seat->last_click_view = view;
		seat->last_click_region = region;
		seat->last_click_time = *now;
	}
}

static bool
manage_titlebar_button_press(struct cow_seat *seat, struct cow_view *view,
    int lx, int ly)
{
	int hit_btn = cow_titlebar_button_at(view, lx, ly);
	if (hit_btn < 0)
		return (false);

	cow_seat_clear_decoration_press(seat);
	view->pressed_button	    = hit_btn;
	view->dec.dirty		    = true;
	seat->titlebar_press	    = COW_TITLEBAR_PRESS_BUTTON;
	seat->pressed_button_idx    = hit_btn;
	seat->pressed_view	    = view;
	cow_pointer_cursor_hold_start();
	river_seat_v1_op_start_pointer(seat->river_seat);
	seat->op_in_progress = true;
	seat->op_released = false;
	return (true);
}

static void
manage_titlebar_button_raw_press(struct cow_seat *seat)
{
	struct cow_view *view = seat->pending_titlebar_button_press_view;
	if (view == NULL)
		return;

	int idx = seat->pending_titlebar_button_press_idx;
	bool released = seat->pending_titlebar_button_release;
	seat->pending_titlebar_button_press_view = NULL;
	seat->pending_titlebar_button_release = false;
	if (view->closed)
		return;

	cow_seat_clear_decoration_press(seat);
	view->pressed_button = idx;
	view->dec.dirty = true;
	seat->titlebar_press = COW_TITLEBAR_PRESS_BUTTON;
	seat->pressed_button_idx = idx;
	seat->pressed_view = view;
	seat->op_released = released;
}

static enum cow_dec_region
decoration_outer_region(struct cow_view *view, int lx, int ly, int cw, int ch)
{
	int content_x, content_y, frame_w, frame_h;
	cow_titlebar_frame_geometry(view, cw, ch, &frame_w, &frame_h,
	    &content_x, &content_y);
	uint32_t edges = cow_decoration_edges_at(view, lx + content_x,
	    ly + content_y, cw, ch);
	if (edges == COW_EDGE_TOP)
		return (COW_DEC_REGION_N);
	if (edges == COW_EDGE_BOTTOM)
		return (COW_DEC_REGION_S);
	if (edges == COW_EDGE_LEFT)
		return (COW_DEC_REGION_W);
	if (edges == COW_EDGE_RIGHT)
		return (COW_DEC_REGION_E);
	return (COW_DEC_REGION_NONE);
}

static void
manage_decoration_click(struct cow_seat *seat, struct cow_view *view)
{
	int lx = seat->pointer_x - view->x;
	int ly = seat->pointer_y - view->y;
	int cw, ch;
	cow_view_effective_content_size(view, &cw, &ch);

	enum cow_dec_region region = COW_DEC_REGION_NONE;
	uint32_t edges = decoration_corner_edges(view, lx, ly, cw, ch);
	if (edges != 0) {
		region = corner_region_from_edges(edges);
	} else if (cow_titlebar_hit_for(view, cw, ch, lx, ly)) {
		if (manage_titlebar_button_press(seat, view, lx, ly)) {
			seat->last_click_view = NULL;
			return;
		} else if (cow_titlebar_is_vertical(
			       view->cfg.titlebar.position) ||
		    !manage_container_tab_click(seat, view, lx, cw)) {
			region = COW_DEC_REGION_TITLEBAR;
		} else if (cow_container_is_split(view)) {
			view   = cow_view_pane(view)->active->view;
			region = COW_DEC_REGION_TITLEBAR;
		} else {
			seat->last_click_view = NULL;
			return;
		}
	} else {
		region = decoration_outer_region(view, lx, ly, cw, ch);
	}
	if (region == COW_DEC_REGION_NONE)
		return;

	struct timespec now;
	int clicks = manage_click_count(seat, view, region, &now);
	struct cow_view *target = view;
	if (clicks == 2 && region >= COW_DEC_REGION_N &&
	    region <= COW_DEC_REGION_SW &&
	    cow_dec_binding_lookup(COW_DEC_REGION_PANE_BORDER, 2) != NULL)
		target = cow_container_border_title_target(view,
		    seat->pointer_x, seat->pointer_y);
	cow_dec_binding_dispatch(seat, target, region, clicks);
	if (region == COW_DEC_REGION_TITLEBAR && cow_container_is_split(view) &&
	    !view->container_placeholder && seat->op_in_progress &&
	    seat->op.type == COW_POINTER_OP_MOVE && seat->op.view == view &&
	    !seat->container_drag && seat->op.group_count == 0) {
		seat->pane_drag = seat->container_drag = true;
		cow_wm.drag_drop_source		       = view;
		cow_pan_cancel();
	}
	manage_store_click(seat, view, region, &now, clicks);
}

static void
manage_interacted_view(struct cow_seat *seat)
{
	if (seat->interacted_view == NULL || seat->interacted_view->closed)
		return;

	struct cow_view *view = seat->interacted_view;
	cow_seat_focus_view(seat, view);
	cow_wm.current_interaction_seat = seat;
	if (seat->consumed_titlebar_button_interaction == view) {
		seat->consumed_titlebar_button_interaction = NULL;
	} else if (seat->has_pointer_position && view->use_ssd &&
	    !seat->op_in_progress) {
		manage_decoration_click(seat, view);
	}
	/* Complete a raw button click after focusing and raising its window. */
	manage_titlebar_button_release(seat);
	seat->interacted_view = NULL;
	cow_wm.current_interaction_seat = NULL;
}

static void
manage_sloppy_focus(struct cow_seat *seat)
{
	if (seat->sloppy_focus_view != NULL && !seat->sloppy_focus_view->closed &&
	    seat->sloppy_focus_view != seat->focused_view) {
		struct cow_view *view = seat->sloppy_focus_view;
		river_seat_v1_focus_window(seat->river_seat,
		    view->river_window);
		seat->previous_focused_view = seat->focused_view;
		seat->focused_view = view;
		cow_wm_set_focused_view(view);
		if (view->output != NULL)
			cow_output_set_current(view->output);
		cow_update_request(COW_UPDATE_STATUS);
	}
	seat->sloppy_focus_view = NULL;
}

static void
manage_process_seat(struct cow_seat *seat)
{
	manage_titlebar_button_pending(seat);
	manage_titlebar_button_raw_press(seat);
	/* A triggered binding may itself cause pointer focus, and therefore
	 * binding context, to change.  Run it before context reconciliation so
	 * disabling the old context cannot discard the pending action.
	 */
	cow_seat_process_bindings(seat);
	cow_seat_enable_all_bindings(seat);
	if (seat->pending_icon_interaction != NULL) {
		struct cow_view *view = seat->pending_icon_interaction;
		seat->pending_icon_interaction = NULL;
		cow_pointer_op_start_icon_move(seat, view);
	}
	manage_titlebar_button_release(seat);
	manage_update_container_drag(seat);
	cow_pointer_op_process(seat);
	if (seat->container_drag && seat->op_in_progress &&
	    cow_wm.drag_ghost_surface == NULL &&
	    cow_wm.drag_drop_source != NULL)
		drag_ghost_create(cow_wm.drag_drop_source);
	manage_interacted_view(seat);
	manage_sloppy_focus(seat);
	manage_cursor_hide(seat);
}

static void
manage_cursor_hide(struct cow_seat *seat)
{
	if (cow_wm.wl_cursor_hidden) {
		if (!seat->cursor_hide_op_active && !seat->op_in_progress) {
			river_seat_v1_op_start_pointer(seat->river_seat);
			seat->cursor_hide_op_active = true;
			seat->op_dx = 0;
			seat->op_dy = 0;
			seat->op_released = false;
		}
		return;
	}

	if (seat->cursor_hide_op_active) {
		river_seat_v1_op_end(seat->river_seat);
		seat->cursor_hide_op_active = false;
		seat->op_dx = 0;
		seat->op_dy = 0;
		seat->op_released = false;
	}
}

static void
manage_apply_deferred_views(void)
{
	struct cow_view *view;
	wl_list_for_each(view, &cow_wm.views, wm_link)
	{
		manage_apply_deferred_close(view);
		manage_apply_deferred_tiled_edges(view);
		manage_apply_deferred_maximized_hint(view);
		manage_apply_deferred_resize(view);
		manage_apply_fullscreen_visibility(view);
		manage_apply_deferred_client_state(view);
	}
}

static void
manage_apply_fullscreen_visibility(struct cow_view *view)
{
	if (view->closed || view->river_window == NULL)
		return;

	bool visible = view->output != NULL && cow_view_visible(view,
	    view->output);
	if (view->fullscreen && visible && !view->fullscreen_applied) {
		river_window_v1_fullscreen(view->river_window,
		    view->output->river_output);
		view->fullscreen_applied = true;
	} else if ((!view->fullscreen || !visible) && view->fullscreen_applied) {
		river_window_v1_exit_fullscreen(view->river_window);
		view->fullscreen_applied = false;
	}
}

static void
manage_apply_deferred_close(struct cow_view *view)
{
	if (!view->pending_close || view->closed)
		return;

	river_window_v1_close(view->river_window);
	view->pending_close = false;
}

static void
manage_apply_deferred_tiled_edges(struct cow_view *view)
{
	if (!view->pending_tiled_edges || view->closed)
		return;

	if (view->river_window != NULL)
		river_window_v1_set_tiled(view->river_window,
		    view->pending_tiled_edges_value);
	view->tiled_edges = view->pending_tiled_edges_value;
	view->pending_tiled_edges = false;
}

static void
manage_apply_deferred_maximized_hint(struct cow_view *view)
{
	if (!view->pending_maximized_hint || view->closed)
		return;

	if (view->river_window != NULL) {
		if (view->pending_maximized_hint_value)
			river_window_v1_inform_maximized(view->river_window);
		else
			river_window_v1_inform_unmaximized(view->river_window);
	}
	view->pending_maximized_hint = false;
}

static void
manage_apply_deferred_resize(struct cow_view *view)
{
	if (!view->pending_resize || view->closed)
		return;

	bool shrinking = (view->pending_width < view->content_width ||
	    view->pending_height < view->content_height);
	bool anchored = view->deferred_anchor_right ||
	    view->deferred_anchor_bottom;
	manage_apply_deferred_position(view, shrinking || anchored);
	river_window_v1_propose_dimensions(view->river_window,
	    view->pending_width, view->pending_height);
	view->pending_resize = false;
	view->dec.dirty	     = true;
}

static void
manage_apply_deferred_position(struct cow_view *view, bool shrinking)
{
	if (shrinking && view->has_dimensions) {
		view->deferred_position = true;
		view->deferred_x	= view->pending_x;
		view->deferred_y	= view->pending_y;
		return;
	}

	view->x = view->pending_x;
	view->y = view->pending_y;
	view->deferred_anchor_right = false;
	view->deferred_anchor_bottom = false;
	if (view->output == NULL || view->desk == NULL)
		return;

	view->vx = (view->x - view->output->x) + view->desk->page_vx;
	view->vy = (view->y - view->output->y) + view->desk->page_vy;
}

static void
manage_apply_deferred_client_state(struct cow_view *view)
{
	if (view->closed || view->river_window == NULL)
		return;

	manage_apply_dimension_bounds(view);
	if (view->pending_fullscreen_state) {
		if (view->pending_fullscreen)
			river_window_v1_inform_fullscreen(view->river_window);
		else
			river_window_v1_inform_not_fullscreen(view->river_window);
		view->pending_fullscreen_state = false;
	}
}

static void
manage_apply_dimension_bounds(struct cow_view *view)
{
	int width = 0, height = 0;

	if (view->output != NULL) {
		if (view->fullscreen || !view->use_ssd) {
			width  = view->output->width;
			height = view->output->height;
		} else {
			struct cow_box *ua = &view->output->usable_area;
			int frame_w, frame_h, content_x, content_y;
			cow_titlebar_frame_geometry(view, view->content_width,
			    view->content_height, &frame_w, &frame_h,
			    &content_x, &content_y);
			width = (view->maximized_h_ignore_reserved ?
			    view->output->width : ua->width) -
			    (frame_w - view->content_width);
			height = (view->maximized_v_ignore_reserved ?
			    view->output->height : ua->height) -
			    (frame_h - view->content_height);
		}
		if (width < 0)
			width = 0;
		if (height < 0)
			height = 0;
	}

	if (view->dimension_bound_width == width &&
	    view->dimension_bound_height == height)
		return;

	river_window_v1_set_dimension_bounds(view->river_window, width, height);
	view->dimension_bound_width  = width;
	view->dimension_bound_height = height;
}

static void
manage_apply_focus(void)
{
	cow_menu_apply_focus_restore();
	manage_apply_pending_focus();
	manage_apply_needs_focus();
	manage_apply_seat_refocus();
}

static void
manage_apply_pending_focus(void)
{
	if (cow_wm.pending_focus == NULL || cow_wm.pending_focus->closed)
		return;

	struct cow_seat *seat;
	wl_list_for_each(seat, &cow_wm.seats, wm_link)
	{
		cow_seat_focus_view(seat, cow_wm.pending_focus);
		break;
	}
	cow_wm.pending_focus = NULL;
}

static void
manage_apply_needs_focus(void)
{

	struct cow_view *view;
	wl_list_for_each(view, &cow_wm.views, wm_link)
	{
		if (view->needs_focus && !view->closed) {
			struct cow_seat *seat = cow_seat_primary();
			if (seat != NULL)
				cow_seat_focus_view(seat, view);
			view->needs_focus = false;
		}
	}
}

static void
manage_apply_seat_refocus(void)
{
	struct cow_seat *seat;

	wl_list_for_each(seat, &cow_wm.seats, wm_link)
	{
		if (!seat->needs_refocus)
			continue;

		seat->needs_refocus = false;
		struct cow_view *target = session_unlock_focus_target(seat);
		if (target == NULL)
			continue;
		cow_seat_focus_view(seat, target);
	}
}

static struct cow_view *
manage_focus_replacement_for_close(struct cow_seat *seat,
    struct cow_view *closing)
{
	if (closing->parent != NULL && closing->parent != closing &&
	    cow_view_focus_replacement_candidate(closing->parent))
		return (closing->parent);

	if (seat->previous_focused_view != NULL &&
	    seat->previous_focused_view != closing &&
	    cow_view_focus_replacement_candidate(seat->previous_focused_view))
		return (seat->previous_focused_view);

	if (seat->pointer_entered_view != NULL &&
	    seat->pointer_entered_view != closing &&
	    cow_view_focus_replacement_candidate(seat->pointer_entered_view))
		return (seat->pointer_entered_view);

	return cow_view_walk(closing, true, cow_view_focus_replacement_candidate);
}

static void
manage_prepare_focus_replacement(struct cow_view *closing)
{
	struct cow_seat *seat;

	wl_list_for_each(seat, &cow_wm.seats, wm_link)
	{
		if (seat->focused_view != closing)
			continue;

		struct cow_view *replacement =
		    manage_focus_replacement_for_close(seat, closing);
		if (replacement == NULL)
			continue;

		cow_seat_focus_view_no_raise(seat, replacement);
	}
}

static void
manage_apply_warp(void)
{
	if (!cow_wm.pending_warp)
		return;

	struct cow_seat *seat;
	wl_list_for_each(seat, &cow_wm.seats, wm_link)
	{
		river_seat_v1_pointer_warp(seat->river_seat, cow_wm.warp_x,
		    cow_wm.warp_y);
		break;
	}
	cow_wm.pending_warp = false;
}

static void
manage_run_deferred_exec(void)
{
	if (!cow_wm.exec_pending)
		return;

	cow_wm.exec_pending = false;
	manage_mark_views_dirty();

	cow_exec_run_deferred();
	pan_frames_update_all();
	if (!cow_wm.restarted)
		cow_rule_fire(COW_RULE_ON_START, NULL, NULL, NULL);
}

static void
manage_mark_views_dirty(void)
{
	struct cow_view *view;
	wl_list_for_each(view, &cow_wm.views, wm_link)
	{
		if (!view->closed)
			view->dec.dirty = true;
	}
}

static void
drag_ghost_create(__unused struct cow_view *src)
{
	if (cow_wm.compositor == NULL || cow_wm.manager == NULL ||
	    cow_wm.drag_ghost_surface != NULL)
		return;

	int sz = GHOST_RADIUS * 2;

	struct wl_surface *surf = wl_compositor_create_surface(
	    cow_wm.compositor);
	if (surf == NULL)
		return;

	struct river_shell_surface_v1 *shell =
	    river_window_manager_v1_get_shell_surface(cow_wm.manager, surf);
	if (shell == NULL) {
		wl_surface_destroy(surf);
		return;
	}

	struct cow_shm_buffer *buf = cow_shm_buffer_create(sz, sz);
	if (buf == NULL) {
		river_shell_surface_v1_destroy(shell);
		wl_surface_destroy(surf);
		return;
	}

	drag_ghost_draw(buf);

	cow_wm.drag_ghost_surface   = surf;
	cow_wm.drag_ghost_shell	    = shell;
	cow_wm.drag_ghost_node	    = river_shell_surface_v1_get_node(shell);
	cow_wm.drag_ghost_buffer    = buf;
	cow_wm.drag_ghost_committed = false;
	drag_ghost_position_from_pointer();
}

static void
drag_ghost_draw(struct cow_shm_buffer *buf)
{
	int sz = GHOST_RADIUS * 2;
	cairo_surface_t *cs =
	    cairo_image_surface_create_for_data((unsigned char *)buf->data,
		CAIRO_FORMAT_ARGB32, sz, sz, buf->stride);
	cairo_t *cr = cairo_create(cs);

	cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
	cairo_paint(cr);
	cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

	/* Filled circle: semi-transparent orange */
	cairo_arc(cr, GHOST_RADIUS, GHOST_RADIUS, GHOST_RADIUS - 2, 0,
	    2 * M_PI);
	cairo_set_source_rgba(cr, 1.0, 0.53, 0.0, 0.4);
	cairo_fill_preserve(cr);

	/* Border: opaque orange, 2px */
	cairo_set_source_rgba(cr, 1.0, 0.53, 0.0, 0.85);
	cairo_set_line_width(cr, 2.0);
	cairo_stroke(cr);

	cairo_destroy(cr);
	cairo_surface_destroy(cs);
}

static void
drag_ghost_position_from_pointer(void)
{
	struct cow_seat *gs;
	wl_list_for_each(gs, &cow_wm.seats, wm_link)
	{
		if (gs->has_pointer_position) {
			cow_wm.drag_ghost_x = gs->pointer_x;
			cow_wm.drag_ghost_y = gs->pointer_y;
			break;
		}
	}
}

void
cow_drag_ghost_destroy(void)
{
	if (cow_wm.drag_ghost_node != NULL) {
		river_node_v1_destroy(cow_wm.drag_ghost_node);
		cow_wm.drag_ghost_node = NULL;
	}
	if (cow_wm.drag_ghost_shell != NULL) {
		river_shell_surface_v1_destroy(cow_wm.drag_ghost_shell);
		cow_wm.drag_ghost_shell = NULL;
	}
	if (cow_wm.drag_ghost_surface != NULL) {
		wl_surface_destroy(cow_wm.drag_ghost_surface);
		cow_wm.drag_ghost_surface = NULL;
	}
	cow_shm_buffer_destroy(cow_wm.drag_ghost_buffer);
	cow_wm.drag_ghost_buffer    = NULL;
	cow_wm.drag_ghost_committed = false;
}

static void
handle_wm_render_start(__unused void *data,
    struct river_window_manager_v1   *manager)
{
	cow_diagnostics_render_begin();
	cow_wm.sequence = COW_WM_SEQUENCE_RENDER;

	render_outputs();
	render_views();
	cow_stack_apply();
	cow_container_render_windows();
	cow_geometry_window_render();
	cow_draw_exec_render();
	render_drag_ghost();
	cow_chord_hint_update();
	cow_notification_render_all();
	cow_menu_render_all();

	river_window_manager_v1_render_finish(manager);
	cow_wm.sequence = COW_WM_SEQUENCE_NONE;
	cow_diagnostics_render_end();
}

static void
render_outputs(void)
{
	struct cow_output *out2;
	wl_list_for_each(out2, &cow_wm.outputs, wm_link)
		cow_output_render_background(out2);
}

static void
render_views(void)
{
	struct cow_view *view;
	wl_list_for_each(view, &cow_wm.views, wm_link)
	{
		if (!view->closed) {
			cow_view_apply_render(view);
			if (cow_view_is_iconified(view))
				cow_view_icon_render(view);
		}
	}
}

static void
render_drag_ghost(void)
{
	if (cow_wm.drag_ghost_node == NULL)
		return;

	river_node_v1_set_position(cow_wm.drag_ghost_node,
	    cow_wm.drag_ghost_x - GHOST_RADIUS,
	    cow_wm.drag_ghost_y - GHOST_RADIUS);
	river_node_v1_place_top(cow_wm.drag_ghost_node);
	if (cow_wm.drag_ghost_committed)
		return;

	wl_surface_attach(cow_wm.drag_ghost_surface,
	    cow_wm.drag_ghost_buffer->buffer, 0, 0);
	wl_surface_damage_buffer(cow_wm.drag_ghost_surface, 0, 0,
	    GHOST_RADIUS * 2, GHOST_RADIUS * 2);
	river_shell_surface_v1_sync_next_commit(cow_wm.drag_ghost_shell);
	wl_surface_commit(cow_wm.drag_ghost_surface);
	cow_wm.drag_ghost_committed = true;
}

static void
handle_wm_session_locked(__unused void	    *data,
    __unused struct river_window_manager_v1 *manager)
{
	cow_wm.session_locked = true;
}

static void
handle_wm_session_unlocked(__unused void    *data,
    __unused struct river_window_manager_v1 *manager)
{
	cow_wm.session_locked = false;

	struct cow_seat *seat;
	wl_list_for_each(seat, &cow_wm.seats, wm_link)
	{
		struct cow_view *target = session_unlock_focus_target(seat);
		if (target != NULL) {
			river_seat_v1_focus_window(seat->river_seat,
			    target->river_window);
			if (seat->focused_view != target)
				seat->previous_focused_view = seat->focused_view;
			seat->focused_view  = target;
			cow_wm_set_focused_view(target);
			if (target->output != NULL)
				cow_output_set_current(target->output);
		}
	}
	cow_update_request(COW_UPDATE_MANAGE | COW_UPDATE_STATUS);
}

static struct cow_view *
session_unlock_focus_target(struct cow_seat *seat)
{
	if (cow_config != NULL && seat->pointer_entered_view != NULL &&
	    !seat->pointer_entered_view->closed)
		return (seat->pointer_entered_view);
	if (seat->focused_view != NULL && !seat->focused_view->closed)
		return (seat->focused_view);
	if (wl_list_empty(&cow_wm.views))
		return (NULL);

	struct cow_view *view;
	wl_list_for_each(view, &cow_wm.views, wm_link)
	{
		if (!view->closed)
			return (view);
	}
	return (NULL);
}

static void
handle_wm_window(__unused void		    *data,
    __unused struct river_window_manager_v1 *manager,
    struct river_window_v1		    *window)
{
	cow_view_create(window);
}

static void
handle_wm_output(__unused void		    *data,
    __unused struct river_window_manager_v1 *manager,
    struct river_output_v1		    *output)
{
	cow_output_create(output);
	/* Update pan frames for newly added output */
	{
		struct cow_output *o = cow_output_find(output);
		if (o != NULL)
			pan_frames_update_output(o);
	}
}

static void
handle_wm_seat(__unused void		    *data,
    __unused struct river_window_manager_v1 *manager,
    struct river_seat_v1		    *seat)
{
	cow_seat_create(seat);
	cow_wm_cursor_theme_configure();
}

bool
cow_wm_init(void)
{
	struct wl_list saved_menus = cow_wm.menus;
	struct wl_list saved_containers = cow_wm.containers;

	memset(&cow_wm, 0, sizeof(cow_wm));
	cow_update_reset();

	const char *restart_pid = getenv(COW_RESTART_ENV);
	if (restart_pid != NULL) {
		char *end = NULL;
		long  pid = strtol(restart_pid, &end, 10);
		cow_wm.restarted = end != restart_pid && *end == '\0' &&
		    pid == (long)getpid();
		unsetenv(COW_RESTART_ENV);
	}

	cow_wm.raise_seq_counter	    = UINT64_MAX / 2;
	cow_wm.outputs_changed_deadline_ms = -1;

	cow_wm.menus = saved_menus;
	wl_list_init(&cow_wm.open_menus);
	wl_list_init(&cow_wm.menu_graveyard);

	wl_list_init(&cow_wm.views);
	wl_list_init(&cow_wm.outputs);
	wl_list_init(&cow_wm.seats);
	cow_keyboard_layout_init();
	cow_wm.containers = saved_containers;
	wl_list_init(&cow_wm.new_views);
	wl_list_init(&cow_wm.closed_views);
	wl_list_init(&cow_wm.wl_outputs);
	cow_wm.menu_context_view = NULL;
	cow_wm.drag_drop_source	 = NULL;
	cow_wm.drag_drop_target	 = NULL;

	cow_wm.display = wl_display_connect(NULL);
	if (cow_wm.display == NULL) {
		cow_log_error("failed to connect to Wayland display");
		return false;
	}

	cow_wm.registry = wl_display_get_registry(cow_wm.display);
	wl_registry_add_listener(cow_wm.registry, &registry_listener, NULL);

	/* First roundtrip: discover globals */
	wl_display_roundtrip(cow_wm.display);

	if (cow_wm.manager == NULL) {
		cow_log_error("river_window_manager_v1 not available "
		    "(is river running?)");
		goto fail;
	}

	if (cow_wm.xkb_bindings == NULL) {
		cow_log_error("river_xkb_bindings_v1 not available");
		goto fail;
	}

	if (cow_wm.compositor == NULL) {
		cow_log_error("wl_compositor not available");
		goto fail;
	}

	if (cow_wm.shm == NULL) {
		cow_log_error("wl_shm not available");
		goto fail;
	}

	/* Second roundtrip: process initial events from bound globals */
	wl_display_roundtrip(cow_wm.display);

	/*
	 * Apply reserved area to all outputs after all initial Wayland
	 * events have settled.  output_handle_dimensions/position skip this
	 * during startup (exec_pending=true); this is the single startup
	 * application.
	 */
	{
		struct cow_output *co;
		wl_list_for_each(co, &cow_wm.outputs, wm_link)
		    (void)cow_output_recompute_usable_area(co);
	}

	/*
	 * Initialise pan frames and raw pointer input after globals are bound.
	 */
	pan_frames_init();
	cow_pointer_input_init();
	cow_wm_ensure_cursor_shape_device();
	cow_wm_cursor_hide_configure();

	cow_wm.running	    = true;
	cow_wm.exec_pending = true;

	if (!cow_event_init()) {
		cow_log_error("failed to initialise event loop");
		goto fail;
	}

	/* Initialize IPC command socket */
	if (!cow_ipc_init())
		cow_log_warn("IPC initialisation failed");

	/* Initialize status FIFO */
	cow_status_init();
	cow_status_emit();

	return true;

fail:
	if (cow_wm.registry != NULL) {
		wl_registry_destroy(cow_wm.registry);
		cow_wm.registry = NULL;
	}
	if (cow_wm.display != NULL) {
		wl_display_disconnect(cow_wm.display);
		cow_wm.display = NULL;
	}
	return false;
}

void
cow_wm_fini(void)
{
	xfree(cow_wm.reported_focus_id);
	cow_wm.reported_focus_id = NULL;
	cow_notification_clear();
	cow_menu_destroy_all();
	cow_state_save();
	cow_draw_exec_cancel();
	cow_geometry_window_hide();

	struct cow_view *view, *view_tmp;
	wl_list_for_each_safe(view, view_tmp, &cow_wm.views, wm_link)
		cow_view_destroy(view);
	struct cow_container *container, *next_container;
	wl_list_for_each_safe(container, next_container, &cow_wm.containers,
	    wm_link)
	{
		cow_container_destroy(container);
	}
	cow_container_window_fini();

	cow_status_fini();
	cow_keyboard_layout_fini();
	cow_virtual_keyboard_fini();
	cow_ipc_fini();
	xfree(cow_ipc.config_path);
	cow_ipc.config_path = NULL;
	cow_event_fini();
	cow_rule_fini();
	cow_style_fini();

	if (cow_wm.layer_shell != NULL)
		river_layer_shell_v1_destroy(cow_wm.layer_shell);
	if (cow_wm.xkb_bindings != NULL)
		river_xkb_bindings_v1_destroy(cow_wm.xkb_bindings);
	if (cow_wm.manager != NULL) {
		river_window_manager_v1_stop(cow_wm.manager);
		wl_display_roundtrip(cow_wm.display);
		river_window_manager_v1_destroy(cow_wm.manager);
	}
	pan_frames_destroy_all();
	if (cow_wm.idle_notification != NULL)
		ext_idle_notification_v1_destroy(cow_wm.idle_notification);
	if (cow_wm.idle_notifier != NULL)
		ext_idle_notifier_v1_destroy(cow_wm.idle_notifier);

	if (cow_wm.wl_cursor_shape_dev != NULL)
		wp_cursor_shape_device_v1_destroy(
		    cow_wm.wl_cursor_shape_dev);
	if (cow_wm.wl_cursor_surface != NULL)
		wl_surface_destroy(cow_wm.wl_cursor_surface);
	if (cow_wm.wl_cursor_theme != NULL)
		wl_cursor_theme_destroy(cow_wm.wl_cursor_theme);
	if (cow_wm.wl_pointer_raw != NULL)
		wl_pointer_destroy(cow_wm.wl_pointer_raw);
	if (cow_wm.wl_seat_raw != NULL)
		wl_seat_destroy(cow_wm.wl_seat_raw);
	if (cow_wm.wl_pointer_constraints != NULL)
		zwp_pointer_constraints_v1_destroy(
		    cow_wm.wl_pointer_constraints);
	if (cow_wm.wl_cursor_shape_mgr != NULL)
		wp_cursor_shape_manager_v1_destroy(
		    cow_wm.wl_cursor_shape_mgr);
	if (cow_wm.wl_layer_shell != NULL)
		zwlr_layer_shell_v1_destroy(cow_wm.wl_layer_shell);
	if (cow_wm.compositor != NULL)
		wl_compositor_destroy(cow_wm.compositor);
	if (cow_wm.viewporter != NULL)
		wp_viewporter_destroy(cow_wm.viewporter);
	if (cow_wm.shm != NULL)
		wl_shm_destroy(cow_wm.shm);
	if (cow_wm.registry != NULL)
		wl_registry_destroy(cow_wm.registry);
	if (cow_wm.display != NULL)
		wl_display_disconnect(cow_wm.display);
}

static void cow_wm_after_event(struct event *);
static void cow_wm_schedule_tick(struct event *);
static void cow_wm_send_manage_dirty(void);

struct cow_wm_wayland_events {
	struct event *write_ev;
	bool	      write_pending;
};

static bool cow_wm_flush_display(struct cow_wm_wayland_events *);

static void
cow_wm_signal_cb(evutil_socket_t sig, __unused short events,
    __unused void *data)
{
	if (sig == SIGUSR1)
		cow_wm_restart_requested = true;
	else if (sig == SIGUSR2) {
		cow_log_toggle("signal=SIGUSR2");
		return;
	} else
		cow_wm_shutdown_requested = true;
	cow_wm.running = false;
}

static void
cow_wm_wayland_cb(__unused evutil_socket_t fd, __unused short events,
    __unused void *data)
{
	if (wl_display_dispatch(cow_wm.display) < 0)
		cow_wm.running = false;
	wl_display_dispatch_pending(cow_wm.display);
}

static void
cow_wm_wayland_write_cb(__unused evutil_socket_t fd, __unused short events,
    void *data)
{
	struct cow_wm_wayland_events *wayland_events = data;

	wayland_events->write_pending = false;
	if (!cow_wm_flush_display(wayland_events))
		cow_wm.running = false;
}

static void
cow_wm_tick_cb(__unused evutil_socket_t fd, __unused short events,
    __unused void *data)
{
	cow_update_request(COW_UPDATE_MANAGE);
}

static bool
cow_wm_flush_display(struct cow_wm_wayland_events *wayland_events)
{
	if (wl_display_flush(cow_wm.display) >= 0) {
		if (wayland_events->write_pending) {
			event_del(wayland_events->write_ev);
			wayland_events->write_pending = false;
		}
		return true;
	}
	if (errno == EAGAIN) {
		if (!wayland_events->write_pending &&
		    event_add(wayland_events->write_ev, NULL) < 0)
			return false;
		wayland_events->write_pending = true;
		return true;
	}

	int err = wl_display_get_error(cow_wm.display);
	if (err != 0) {
		uint32_t		   id	 = 0;
		const struct wl_interface *iface = NULL;
		int proto_err = wl_display_get_protocol_error(cow_wm.display,
		    &iface, &id);
		cow_log_error("Wayland protocol error %d on %s@%u", proto_err,
		    iface ? iface->name : "unknown", id);
	}
	return false;
}

static int
manage_next_button_click_ms(void)
{
	long now = cow_mono_ms();
	long next = -1;
	struct cow_seat *seat;

	wl_list_for_each(seat, &cow_wm.seats, wm_link)
	{
		if (seat->pending_button_click_view == NULL)
			continue;
		long delta = seat->pending_button_click_deadline_ms - now;
		if (next < 0 || delta < next)
			next = delta;
	}
	return next < 0 ? -1 : (int)next;
}

static void
cow_wm_schedule_tick(struct event *tick_ev)
{
	int tick_ms = -1;

	if (cow_wm.pan_lock_output != NULL)
		tick_ms = 50;

	int repeat_ms = cow_seat_next_repeat_ms();
	if (repeat_ms >= 0) {
		int wait = repeat_ms < 1 ? 1 : repeat_ms;
		if (tick_ms < 0 || wait < tick_ms)
			tick_ms = wait;
	}
	int menu_repeat_ms = cow_menu_next_repeat_ms();
	if (menu_repeat_ms >= 0) {
		int wait = menu_repeat_ms < 1 ? 1 : menu_repeat_ms;
		if (tick_ms < 0 || wait < tick_ms)
			tick_ms = wait;
	}
	int notification_ms = cow_notification_next_ms();
	if (notification_ms >= 0) {
		int wait = notification_ms < 1 ? 1 : notification_ms;
		if (tick_ms < 0 || wait < tick_ms)
			tick_ms = wait;
	}

	if (cow_menu_is_open() && (tick_ms < 0 || tick_ms > 33))
		tick_ms = 33;

	int button_ms = manage_next_button_click_ms();
	if (button_ms >= 0) {
		int wait = button_ms < 1 ? 1 : button_ms;
		if (tick_ms < 0 || wait < tick_ms)
			tick_ms = wait;
	}

	if (cow_wm.outputs_changed_deadline_ms >= 0) {
		long delta = cow_wm.outputs_changed_deadline_ms -
		    cow_mono_ms();
		int wait = delta < 1 ? 1 : (int)delta;
		if (tick_ms < 0 || wait < tick_ms)
			tick_ms = wait;
	}

	if (tick_ms < 0) {
		event_del(tick_ev);
		return;
	}

	struct timeval tv = {
		.tv_sec	 = tick_ms / 1000,
		.tv_usec = (tick_ms % 1000) * 1000,
	};
	event_add(tick_ev, &tv);
}

void
cow_wm_schedule_outputs_changed(void)
{
	cow_wm.output_state_dirty = true;
	cow_wm.outputs_changed_deadline_ms = cow_mono_ms() +
	    COW_OUTPUTS_CHANGED_DEBOUNCE_MS;
}

static void
cow_wm_fire_outputs_changed(void)
{
	if (cow_wm.outputs_changed_deadline_ms < 0 ||
	    cow_mono_ms() < cow_wm.outputs_changed_deadline_ms)
		return;

	bool ready = !wl_list_empty(&cow_wm.outputs);
	struct cow_output *po;
	if (ready) {
		wl_list_for_each(po, &cow_wm.outputs, wm_link)
		{
			if (po->name == NULL) {
				ready = false;
				break;
			}
		}
	}
	if (!ready) {
		cow_wm.outputs_changed_deadline_ms =
		    cow_mono_ms() + COW_OUTPUTS_CHANGED_DEBOUNCE_MS;
		return;
	}

	cow_wm.outputs_changed_deadline_ms = -1;
	if (cow_wm.output_state_dirty) {
		struct cow_output *output;

		cow_wm.output_state_dirty = false;
		wl_list_for_each(output, &cow_wm.outputs, wm_link)
			cow_style_reflow_anchors(output);
	}
	cow_rule_fire(COW_RULE_ON_OUTPUTS_CHANGED, NULL, NULL, NULL);
	cow_update_request(COW_UPDATE_STATUS);
}

void
cow_wm_set_focused_view(struct cow_view *view)
{
	if (cow_wm.focused_view == view)
		return;
	cow_wm.focused_view = view;
	cow_focus_history_changed(view);
	cow_update_request(COW_UPDATE_STATUS);
}

void
cow_wm_dispatch_focus_change(void)
{
	/* Wait for a requested replacement so event.current and the DSL's
	 * effective focus describe the same window.
	 */
	if (cow_wm.pending_focus != NULL && !cow_wm.pending_focus->closed)
		return;
	struct cow_view *view = cow_wm.focused_view;
	if (view != NULL && view->closed)
		view = NULL;
	const char *current = view != NULL ? view->identifier : NULL;
	const char *previous = cow_wm.reported_focus_id;
	if ((previous == NULL && current == NULL) ||
	    (previous != NULL && current != NULL && !strcmp(previous, current)))
		return;

	cJSON *event = cJSON_CreateObject();
	cJSON_AddItemToObject(event, "previous", previous != NULL ?
	    cJSON_CreateString(previous) : cJSON_CreateNull());
	cJSON_AddItemToObject(event, "current", current != NULL ?
	    cJSON_CreateString(current) : cJSON_CreateNull());
	char *json = cJSON_PrintUnformatted(event);
	cJSON_Delete(event);
	/* Retain an ID, not a view: the old window may already be destroyed.
	 * Advance before running rules so a new focus request is deferred
	 * until the next event-loop turn rather than dispatched recursively.
	 */
	xstrset(&cow_wm.reported_focus_id, current);
	cow_rule_fire_json(COW_RULE_ON_FOCUS_CHANGE, view,
	    view != NULL ? view->desk : NULL,
	    view != NULL ? view->output : NULL, json);
	xfree(json);
}

static void
cow_wm_title_cb(__unused evutil_socket_t fd, __unused short events,
    __unused void *data)
{
	cow_view_flush_titles();
}

static void
cow_wm_after_event(struct event *title_ev)
{
	cow_wm_fire_outputs_changed();
	cow_wm_dispatch_focus_change();
	cow_flush_binding_updates();

	uint32_t updates = cow_update_take();
	if ((updates & COW_UPDATE_TITLE) != 0 &&
	    !evtimer_pending(title_ev, NULL)) {
		/* Bound title-only refreshes to 20 Hz, without postponing the
		 * deadline on each keystroke or delaying interactive operations.
		 */
		const struct timeval interval = { .tv_usec = 50000 };
		evtimer_add(title_ev, &interval);
	}
	if ((updates & COW_UPDATE_STATUS) != 0)
		cow_status_emit();
	/* Wayland preserves request order, so manage_dirty naturally follows
	 * requests already queued for River; no synchronising roundtrip is
	 * required here.
	 */
	if ((updates & COW_UPDATE_MANAGE) != 0)
		cow_wm_send_manage_dirty();

	if (cow_wm_shutdown_requested || cow_wm_restart_requested)
		cow_wm.running = false;
}

void
cow_wm_run(void)
{
	struct event_base *base = cow_event_base();
	struct event	  *wl_ev = NULL, *tick_ev = NULL, *sigint_ev = NULL;
	struct event	  *sigterm_ev = NULL, *sigusr1_ev = NULL;
	struct event	  *sigusr2_ev = NULL;
	struct event	  *title_ev = NULL;
	struct cow_wm_wayland_events wayland_events = { 0 };

	wl_ev = event_new(base, wl_display_get_fd(cow_wm.display),
	    EV_READ | EV_PERSIST, cow_wm_wayland_cb, NULL);
	wayland_events.write_ev = event_new(base,
	    wl_display_get_fd(cow_wm.display), EV_WRITE,
	    cow_wm_wayland_write_cb, &wayland_events);
	tick_ev = evtimer_new(base, cow_wm_tick_cb, NULL);
	title_ev = evtimer_new(base, cow_wm_title_cb, NULL);
	sigint_ev = evsignal_new(base, SIGINT, cow_wm_signal_cb, NULL);
	sigterm_ev = evsignal_new(base, SIGTERM, cow_wm_signal_cb, NULL);
	sigusr1_ev = evsignal_new(base, SIGUSR1, cow_wm_signal_cb, NULL);
	sigusr2_ev = evsignal_new(base, SIGUSR2, cow_wm_signal_cb, NULL);

	if (wl_ev == NULL || wayland_events.write_ev == NULL || tick_ev == NULL ||
	    sigint_ev == NULL || title_ev == NULL ||
	    sigterm_ev == NULL || sigusr1_ev == NULL || sigusr2_ev == NULL)
		goto out;

	event_add(wl_ev, NULL);
	event_add(sigint_ev, NULL);
	event_add(sigterm_ev, NULL);
	event_add(sigusr1_ev, NULL);
	event_add(sigusr2_ev, NULL);

	while (cow_wm.running) {
		if (!cow_wm_flush_display(&wayland_events))
			break;
		wl_display_dispatch_pending(cow_wm.display);
		cow_wm_schedule_tick(tick_ev);

		if (event_base_loop(base, EVLOOP_ONCE) < 0)
			break;

		cow_wm_after_event(title_ev);
	}

out:
	if (title_ev != NULL)
		event_free(title_ev);
	if (wl_ev != NULL)
		event_free(wl_ev);
	if (wayland_events.write_ev != NULL)
		event_free(wayland_events.write_ev);
	if (tick_ev != NULL)
		event_free(tick_ev);
	if (sigint_ev != NULL)
		event_free(sigint_ev);
	if (sigterm_ev != NULL)
		event_free(sigterm_ev);
	if (sigusr1_ev != NULL)
		event_free(sigusr1_ev);
	if (sigusr2_ev != NULL)
		event_free(sigusr2_ev);
}

static void
cow_wm_send_manage_dirty(void)
{
	if (cow_wm.manager != NULL) {
		cow_log_trace("requesting immediate manage cycle");
		river_window_manager_v1_manage_dirty(cow_wm.manager);
	}
}

void
cow_wm_ensure_cursor_shape_device(void)
{
	if (cow_wm.wl_cursor_shape_dev != NULL ||
	    cow_wm.wl_cursor_shape_mgr == NULL || cow_wm.wl_pointer_raw == NULL)
		return;

	cow_wm.wl_cursor_shape_dev = wp_cursor_shape_manager_v1_get_pointer(
	    cow_wm.wl_cursor_shape_mgr, cow_wm.wl_pointer_raw);
	cow_wm.wl_cursor_shape = 0;
}

static const char *const *
cursor_shape_names(uint32_t shape)
{
	static const char *const default_names[] = {
		"default", "left_ptr", NULL
	};
	static const char *const grab_names[] = {
		"grab", "openhand", "hand1", NULL
	};
	static const char *const n_names[] = {
		"n-resize", "top_side", "ns-resize", NULL
	};
	static const char *const s_names[] = {
		"s-resize", "bottom_side", "ns-resize", NULL
	};
	static const char *const e_names[] = {
		"e-resize", "right_side", "ew-resize", NULL
	};
	static const char *const w_names[] = {
		"w-resize", "left_side", "ew-resize", NULL
	};
	static const char *const ne_names[] = {
		"ne-resize", "top_right_corner", "nesw-resize", NULL
	};
	static const char *const nw_names[] = {
		"nw-resize", "top_left_corner", "nwse-resize", NULL
	};
	static const char *const se_names[] = {
		"se-resize", "bottom_right_corner", "nwse-resize", NULL
	};
	static const char *const sw_names[] = {
		"sw-resize", "bottom_left_corner", "nesw-resize", NULL
	};

	switch (shape) {
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_GRAB:
		return (grab_names);
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_N_RESIZE:
		return (n_names);
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_S_RESIZE:
		return (s_names);
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_E_RESIZE:
		return (e_names);
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_W_RESIZE:
		return (w_names);
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NE_RESIZE:
		return (ne_names);
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NW_RESIZE:
		return (nw_names);
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_SE_RESIZE:
		return (se_names);
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_SW_RESIZE:
		return (sw_names);
	default:
		return (default_names);
	}
}

static void
cow_wm_ensure_cursor_theme(void)
{
	if (cow_wm.wl_cursor_theme != NULL || cow_wm.shm == NULL ||
	    cow_wm.compositor == NULL)
		return;

	const char *theme = cow_config != NULL ? cow_config->cursor_theme :
	    COW_CFG_DEFAULT_CURSOR_THEME;
	int size = cow_config != NULL ? cow_config->cursor_size :
	    COW_CFG_DEFAULT_CURSOR_SIZE;

	cow_wm.wl_cursor_theme = wl_cursor_theme_load(theme, size, cow_wm.shm);
	if (cow_wm.wl_cursor_theme != NULL &&
	    cow_wm.wl_cursor_surface == NULL)
		cow_wm.wl_cursor_surface =
		    wl_compositor_create_surface(cow_wm.compositor);
}

static void
cow_wm_set_cursor_surface(uint32_t shape)
{
	if (cow_wm.wl_pointer_raw == NULL || cow_wm.wl_cursor_serial == 0)
		return;

	cow_wm_ensure_cursor_theme();
	if (cow_wm.wl_cursor_theme == NULL ||
	    cow_wm.wl_cursor_surface == NULL)
		return;

	const char *const *names = cursor_shape_names(shape);
	struct wl_cursor  *cursor = NULL;
	for (size_t i = 0; names[i] != NULL; i++) {
		cursor = wl_cursor_theme_get_cursor(cow_wm.wl_cursor_theme,
		    names[i]);
		if (cursor != NULL)
			break;
	}
	if (cursor == NULL || cursor->image_count == 0)
		return;

	struct wl_cursor_image *img = cursor->images[0];
	struct wl_buffer       *buf = wl_cursor_image_get_buffer(img);
	if (buf == NULL)
		return;

	wl_pointer_set_cursor(cow_wm.wl_pointer_raw, cow_wm.wl_cursor_serial,
	    cow_wm.wl_cursor_surface, img->hotspot_x, img->hotspot_y);
	wl_surface_attach(cow_wm.wl_cursor_surface, buf, 0, 0);
	wl_surface_damage_buffer(cow_wm.wl_cursor_surface, 0, 0, img->width,
	    img->height);
	wl_surface_commit(cow_wm.wl_cursor_surface);
}

static void
cow_wm_hide_cursor_surface(void)
{
	if (cow_wm.wl_pointer_raw == NULL)
		return;

	wl_pointer_set_cursor(cow_wm.wl_pointer_raw, cow_wm.wl_cursor_serial,
	    NULL, 0, 0);
}

static void
cow_wm_set_cursor_shape_internal(uint32_t shape, uint32_t serial, bool force)
{
	cow_wm_ensure_cursor_shape_device();
	uint32_t old_serial = cow_wm.wl_cursor_serial;
	if (serial != 0)
		cow_wm.wl_cursor_serial = serial;
	if (cow_wm.wl_cursor_hidden) {
		cow_wm.wl_cursor_shape = shape;
		cow_wm_hide_cursor_surface();
		return;
	}
	if (!force && cow_wm.wl_cursor_shape == shape &&
	    (serial == 0 || old_serial == cow_wm.wl_cursor_serial))
		return;

	if (cow_wm.wl_cursor_shape_dev != NULL && cow_wm.wl_cursor_serial != 0)
		wp_cursor_shape_device_v1_set_shape(cow_wm.wl_cursor_shape_dev,
		    cow_wm.wl_cursor_serial, shape);
	cow_wm_set_cursor_surface(shape);
	cow_wm.wl_cursor_shape = shape;
}

void
cow_wm_cursor_theme_configure(void)
{
	if (cow_config == NULL || cow_config->cursor_theme == NULL)
		return;

	char size[32];
	snprintf(size, sizeof(size), "%d", cow_config->cursor_size);
	setenv("XCURSOR_THEME", cow_config->cursor_theme, 1);
	setenv("XCURSOR_SIZE", size, 1);

	if (cow_wm.manager != NULL) {
		struct cow_seat *seat;
		wl_list_for_each(seat, &cow_wm.seats, wm_link)
		{
			if (!seat->removed &&
			    river_seat_v1_get_version(seat->river_seat) >= 2)
				river_seat_v1_set_xcursor_theme(
				    seat->river_seat, cow_config->cursor_theme,
				    (uint32_t)cow_config->cursor_size);
		}
	}

	if (cow_wm.wl_cursor_theme != NULL) {
		wl_cursor_theme_destroy(cow_wm.wl_cursor_theme);
		cow_wm.wl_cursor_theme = NULL;
	}

	if (cow_wm.wl_pointer_has_focus && cow_wm.wl_cursor_serial != 0) {
		uint32_t shape = cow_wm.wl_cursor_shape != 0 ?
		    cow_wm.wl_cursor_shape :
		    WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT;
		cow_wm_set_cursor_shape_internal(shape, 0, true);
	}
}

void
cow_wm_note_cursor_focus(uint32_t serial, bool focused)
{
	cow_wm.wl_pointer_has_focus = focused;
	if (serial != 0)
		cow_wm.wl_cursor_serial = serial;
	if (focused && cow_wm.wl_cursor_hidden)
		cow_wm_hide_cursor_surface();
}

void
cow_wm_set_cursor_shape(uint32_t shape, uint32_t serial)
{
	cow_wm_set_cursor_shape_internal(shape, serial, false);
}

void
cow_wm_reset_cursor_shape(uint32_t serial)
{
	cow_wm_set_cursor_shape_internal(
	    WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT, serial, true);
}

void
cow_wm_set_cursor_hidden(bool hidden)
{
	if (cow_wm.wl_cursor_hidden == hidden)
		return;

	cow_wm.wl_cursor_hidden = hidden;
	if (hidden) {
		cow_wm_hide_cursor_surface();
	} else {
		uint32_t shape = cow_wm.wl_cursor_shape != 0 ?
		    cow_wm.wl_cursor_shape :
		    WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT;
		cow_wm_set_cursor_shape_internal(shape, 0, true);
	}
	cow_update_request(COW_UPDATE_MANAGE);
}

bool
cow_wm_cursor_hidden(void)
{
	return cow_wm.wl_cursor_hidden;
}

void
cow_wm_cursor_hide_configure(void)
{
	if (cow_wm.idle_notification != NULL) {
		ext_idle_notification_v1_destroy(cow_wm.idle_notification);
		cow_wm.idle_notification = NULL;
	}

	cow_wm_set_cursor_hidden(false);
	if (cow_config == NULL || cow_config->cursor_hide_timeout == 0 ||
	    cow_wm.idle_notifier == NULL || cow_wm.wl_seat_raw == NULL)
		return;

	if (cow_wm.idle_notifier_version >= 2) {
		cow_wm.idle_notification =
		    ext_idle_notifier_v1_get_input_idle_notification(
			cow_wm.idle_notifier,
			(uint32_t)cow_config->cursor_hide_timeout,
			cow_wm.wl_seat_raw);
	} else {
		cow_wm.idle_notification =
		    ext_idle_notifier_v1_get_idle_notification(
			cow_wm.idle_notifier,
			(uint32_t)cow_config->cursor_hide_timeout,
			cow_wm.wl_seat_raw);
	}
	ext_idle_notification_v1_add_listener(cow_wm.idle_notification,
	    &idle_notification_listener, NULL);
}

void
cow_wm_request_pointer_warp(int x, int y)
{
	cow_wm.pending_warp = true;
	cow_wm.warp_x	    = x;
	cow_wm.warp_y	    = y;
}

void
cow_wm_restart(char **argv)
{
	/* Run on-restart rules synchronously before anything else, so the
	 * user can kill processes, clean up, etc. before the new process
	 * starts.  Replaces the older single-shot cow_config->hook_restart
	 * mechanism with the unified rule subsystem. */
	cow_rule_fire(COW_RULE_ON_RESTART, NULL, NULL, NULL);

	/* Save state so the new process can restore it. */
	cow_state_save();

	/* Release IPC and status sockets so the new process can bind them.
	 * Do NOT send stop/destroy to River -- the new process takes over
	 * the existing window manager session immediately. */
	cow_status_fini();
	cow_ipc_fini();

	char restart_pid[32];
	snprintf(restart_pid, sizeof restart_pid, "%ld", (long)getpid());
	setenv(COW_RESTART_ENV, restart_pid, 1);

	cow_log_info("restarting (%s)", argv[0]);
	execvp(argv[0], argv);
	cow_log_fatal("restart failed: %s", strerror(errno));
	_exit(EXIT_FAILURE);
}
