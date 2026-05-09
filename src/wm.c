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
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <cairo.h>
#include <math.h>

#include <wayland-client.h>

#include "cow/cmd.h"
#include "cow/command.h"
#include "cow/config.h"
#include "cow/ipc.h"
#include "cow/keybinding.h"
#include "cow/memory.h"
#include "cow/desk_ops.h"
#include "cow/output.h"
#include "cow/pan_frames.h"
#include "cow/pointer_op.h"
#include "cow/seat.h"
#include "cow/state.h"
#include "cow/status.h"
#include "cow/titlebar.h"
#include "cow/shm.h"
#include "cow/util.h"
#include "cow/view.h"
#include "cow/wm.h"

#include "pointer-constraints-unstable-v1-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "river-layer-shell-v1-client-protocol.h"
#include "river-window-management-v1-client-protocol.h"
#include "river-xkb-bindings-v1-client-protocol.h"

#define GHOST_RADIUS 24

struct cow_wm cow_wm;

/* Forward declarations for protocol listeners */
static void handle_wm_unavailable(void *, struct river_window_manager_v1 *);
static void handle_wm_finished(void *, struct river_window_manager_v1 *);
static void handle_wm_manage_start(void *, struct river_window_manager_v1 *);
static void handle_wm_render_start(void *, struct river_window_manager_v1 *);
static void drag_ghost_create(struct cow_view *);

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

	xfree(entry->name);
	entry->name = xstrdup(name);

	/* Try to match to an existing cow_output */
	struct cow_output *out;
	wl_list_for_each(out, &cow_wm.outputs, wm_link)
	{
		if (out->wl_output_name == entry->global_name &&
		    out->name == NULL) {
			out->wl_output = entry->wl_output;
			out->name      = xstrdup(name);
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

static void
wl_seat_handle_capabilities(__unused void *data, __unused struct wl_seat *seat,
    uint32_t capabilities)
{
	cow_wm.wl_seat_has_pointer = (capabilities &
					 WL_SEAT_CAPABILITY_POINTER) != 0;

	if (!cow_wm.wl_seat_has_pointer && cow_wm.wl_pointer_raw != NULL) {
		/* Pointer capability removed — release the wl_pointer. */
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

/*
 * Registry handler: bind river globals.
 */
static void
registry_global(__unused void *data, struct wl_registry *registry,
    uint32_t name, const char *interface, __unused uint32_t version)
{
	if (strcmp(interface, river_window_manager_v1_interface.name) == 0) {
		cow_wm.manager = wl_registry_bind(registry, name,
		    &river_window_manager_v1_interface, 4);
		river_window_manager_v1_add_listener(cow_wm.manager,
		    &wm_listener, NULL);
	} else if (strcmp(interface, river_xkb_bindings_v1_interface.name) ==
	    0) {
		cow_wm.xkb_bindings = wl_registry_bind(registry, name,
		    &river_xkb_bindings_v1_interface, 2);
	} else if (strcmp(interface, river_layer_shell_v1_interface.name) ==
	    0) {
		cow_wm.layer_shell = wl_registry_bind(registry, name,
		    &river_layer_shell_v1_interface, 1);
	} else if (strcmp(interface, "wl_compositor") == 0) {
		cow_wm.compositor = wl_registry_bind(registry, name,
		    &wl_compositor_interface, 6);
	} else if (strcmp(interface, "wl_shm") == 0) {
		cow_wm.shm = wl_registry_bind(registry, name, &wl_shm_interface,
		    1);
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
	} else if (strcmp(interface,
		       zwp_pointer_constraints_v1_interface.name) == 0 &&
	    version >= 1) {
		cow_wm.wl_pointer_constraints = wl_registry_bind(registry, name,
		    &zwp_pointer_constraints_v1_interface, 1);
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
    __unused struct wl_registry *registry, __unused uint32_t name)
{
    return;
}

static const struct wl_registry_listener registry_listener = {
	.global	       = registry_global,
	.global_remove = registry_global_remove,
};

static void
handle_wm_unavailable(__unused void	    *data,
    __unused struct river_window_manager_v1 *manager)
{
	fprintf(stderr,
	    "cow: window management unavailable "
	    "(another WM is already running?)\n");
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

static inline void
wm_enter_sequence(enum cow_wm_sequence seq)
{
	cow_wm.sequence = seq;
}

static inline void
wm_leave_sequence(void)
{
	cow_wm.sequence = COW_WM_SEQUENCE_NONE;
}

static void
manage_reap_destroyed_objects(void)
{
	struct cow_view *view, *view_tmp;
	wl_list_for_each_safe(view, view_tmp, &cow_wm.views, wm_link)
	{
		if (view->closed)
			cow_view_destroy(view);
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
		cow_page_scroll_tick(out);
		cow_output_pan_frame_tick(out);
	}
}

static void
manage_configure_new_views(void)
{
	struct cow_view *view;
	wl_list_for_each(view, &cow_wm.views, wm_link)
	{
		if (!view->configured && !view->closed)
			cow_view_configure(view);
	}
}

static void
seat_dispatch_titlebar_button_release(struct cow_seat *s)
{
	if (!s->op_released || s->titlebar_press != COW_TITLEBAR_PRESS_BUTTON)
		return;

	struct cow_view *pv = s->pressed_view;
	if (pv != NULL && !pv->closed) {
		bool still_on_button = false;
		if (s->has_pointer_position) {
			int tt = pv->dec.title_thickness;
			int bw = pv->dec.border_width;
			int lx = s->pointer_x - pv->x;
			int ly = s->pointer_y - pv->y;
			int bi = s->pressed_button_idx;

			if (cow_titlebar_hit(pv, lx, ly)) {
				struct cow_box tbr = cow_titlebar_squeezed_rect(
				    pv);
				int btn_s = tt;
				int tbr_l = tbr.x - bw;
				int tbr_r = tbr_l + tbr.width;
				int hit	  = -1;
				if (lx >= tbr_l && lx < tbr_l + btn_s)
					hit = 0;
				else if (lx >= tbr_r - 2 * btn_s &&
				    lx < tbr_r - btn_s)
					hit = 1;
				else if (lx >= tbr_r - btn_s && lx < tbr_r)
					hit = 2;
				still_on_button = (hit == bi);
			}
		}
		if (still_on_button && cow_config != NULL) {
			int bi = s->pressed_button_idx;
			if (bi >= 0 && bi < 3) {
				enum cow_button_action act =
				    cow_config->titlebar.buttons[bi].action;
				/*
				 * Direct global writes here (rather than via
				 * cow_seat_focus_view) are deliberate: the
				 * seat already has focus on pv; we just keep
				 * the WM-wide pointers in sync without
				 * re-emitting focus_window or status.
				 */
				cow_wm.focused_view = pv;
				if (pv->output != NULL)
					cow_wm.current_output = pv->output;
				if (act == COW_BUTTON_ACTION_CLOSE)
					river_window_v1_close(pv->river_window);
				else if (act == COW_BUTTON_ACTION_MAXIMIZE) {
					if (pv->maximized)
						cow_view_unmaximize(pv);
					else
						cow_view_maximize(pv);
				} else if (act == COW_BUTTON_ACTION_ICONIFY) {
					cow_view_iconify(pv);
				}
			}
		}
		/* Always restore button frame to raised after release */
		pv->pressed_button = -1;
		pv->dec.dirty	   = true;
	}
	river_seat_v1_op_end(s->river_seat);
	s->op_in_progress = false;
	s->op_released	  = false;
	s->titlebar_press = COW_TITLEBAR_PRESS_NONE;
	s->pressed_view	  = NULL;
}

static void
seat_update_drag_drop_target(struct cow_seat *s)
{
	if (!s->container_drag || !s->op_in_progress ||
	    s->op.type != COW_POINTER_OP_MOVE || !s->has_pointer_position ||
	    s->op.view->container_member != NULL)
		return;

	/* Only scan for drop targets when the source is NOT already
	 * containerised.  A -C drag from a containerised view is an
	 * uncontainerise operation -- no highlight needed.
	 */
	struct cow_view *new_target = NULL;
	struct cow_view *sv;
	wl_list_for_each(sv, &cow_wm.views, wm_link)
	{
		if (sv->closed || sv == s->op.view)
			continue;
		if (sv->output == NULL || !cow_view_visible(sv, sv->output))
			continue;
		struct cow_box fb = cow_view_frame_box(sv);
		if (s->pointer_x >= fb.x && s->pointer_x < fb.x + fb.width &&
		    s->pointer_y >= fb.y && s->pointer_y < fb.y + fb.height) {
			new_target = sv;
			break;
		}
	}
	if (new_target != cow_wm.drag_drop_target) {
		if (cow_wm.drag_drop_target != NULL)
			cow_wm.drag_drop_target->dec.dirty = true;
		cow_wm.drag_drop_target = new_target;
		if (cow_wm.drag_drop_target != NULL)
			cow_wm.drag_drop_target->dec.dirty = true;
	}
}

static void
seat_maybe_create_drag_ghost(struct cow_seat *s)
{
	if (s->container_drag && s->op_in_progress &&
	    cow_wm.drag_ghost_surface == NULL &&
	    cow_wm.drag_drop_source != NULL)
		drag_ghost_create(cow_wm.drag_drop_source);
}

static bool
seat_dispatch_decoration_click(struct cow_seat *s)
{
	if (s->interacted_view == NULL || s->interacted_view->closed)
		return false;

	struct cow_view *iv = s->interacted_view;
	cow_seat_focus_view(s, iv);

	if (!s->has_pointer_position || !iv->use_ssd || s->op_in_progress) {
		s->interacted_view = NULL;
		return false;
	}

	int bw = iv->dec.border_width;
	int tt = iv->dec.title_thickness;
	int lx = s->pointer_x - iv->x;
	int ly = s->pointer_y - iv->y;
	int cl = cow_config ? cow_config->corner_length : 20;
	if (cl < bw)
		cl = bw;

	int hit_cw = iv->sd != COW_SHADE_NONE ? iv->shade_saved_width :
						iv->content_width;
	int hit_ch = iv->sd != COW_SHADE_NONE ? iv->shade_saved_height :
						iv->content_height;

	/* Double-click on titlebar -> shade/unshade (north). */
	bool in_titlebar = cow_titlebar_hit(iv, lx, ly) ||
	    (iv->sd != COW_SHADE_NONE);

	if (in_titlebar) {
		struct timespec now;
		clock_gettime(CLOCK_MONOTONIC, &now);
		if (s->last_click_view == iv &&
		    elapsed_ms(&s->last_click_time, &now) < DOUBLE_CLICK_MS) {
			cow_view_shade(iv, COW_SHADE_NORTH);
			s->last_click_view = NULL;
			s->interacted_view = NULL;
			return true;
		}
		s->last_click_view = iv;
		s->last_click_time = now;
	}

	/*
	 * Corner resize detection.  The corner zone is an L-shaped
	 * region at each corner of the frame, extending `cl` pixels
	 * along each edge.  The pointer must be in the border area
	 * (not content) AND within `cl` of a corner along either axis.
	 */
	int	 frame_w      = hit_cw + 2 * bw;
	int	 frame_h      = hit_ch + tt + 2 * bw;
	int	 fx	      = lx + bw;
	int	 fy	      = ly + bw + tt;
	bool	 in_corner    = false;
	uint32_t corner_edges = 0;

	bool in_border = (fx >= 0 && fx < frame_w && fy >= 0 && fy < frame_h &&
	    (fx < bw || fx >= frame_w - bw || fy < bw || fy >= frame_h - bw));

	if (in_border) {
		bool near_top	= (fy < cl);
		bool near_bot	= (fy >= frame_h - cl);
		bool near_left	= (fx < cl);
		bool near_right = (fx >= frame_w - cl);

		bool in_tb_btn = false;
		if (tt > 0 && ly >= -tt && ly < 0) {
			if (lx >= 0 && lx < tt)
				in_tb_btn = true;
			if (lx >= hit_cw - 2 * tt && lx < hit_cw)
				in_tb_btn = true;
		}

		bool in_titlebar_height = (tt > 0 && ly >= -tt && ly < 0);

		if (!in_titlebar_height) {
			if (near_top && near_left && !in_tb_btn) {
				corner_edges = COW_EDGE_TOP | COW_EDGE_LEFT;
				in_corner    = true;
			} else if (near_top && near_right && !in_tb_btn) {
				corner_edges = COW_EDGE_TOP | COW_EDGE_RIGHT;
				in_corner    = true;
			} else if (near_bot && near_left) {
				corner_edges = COW_EDGE_BOTTOM | COW_EDGE_LEFT;
				in_corner    = true;
			} else if (near_bot && near_right) {
				corner_edges = COW_EDGE_BOTTOM | COW_EDGE_RIGHT;
				in_corner    = true;
			}
		}
	}

	if (in_corner && corner_edges != 0) {
		cow_pointer_op_start_resize(s, iv, corner_edges);
	} else if (cow_titlebar_hit(iv, lx, ly)) {
		struct cow_box tbr	 = cow_titlebar_squeezed_rect(iv);
		int	       btn_s	 = tt;
		int	       tbr_left	 = tbr.x - bw;
		int	       tbr_right = tbr_left + tbr.width;
		int	       hit_btn	 = -1;

		if (lx >= tbr_left && lx < tbr_left + btn_s)
			hit_btn = 0;
		else if (lx >= tbr_right - 2 * btn_s && lx < tbr_right - btn_s)
			hit_btn = 1;
		else if (lx >= tbr_right - btn_s && lx < tbr_right)
			hit_btn = 2;

		if (hit_btn >= 0) {
			iv->pressed_button    = hit_btn;
			iv->dec.dirty	      = true;
			s->titlebar_press     = COW_TITLEBAR_PRESS_BUTTON;
			s->pressed_button_idx = hit_btn;
			s->pressed_view	      = iv;
			river_seat_v1_op_start_pointer(s->river_seat);
			s->op_in_progress = true;
			s->op_released	  = false;
		} else {
			/*
			 * Tab-click: if the view is in a container with
			 * multiple members, check if the click landed on a
			 * tab label.  Containers force squeeze NONE, so the
			 * tab strip still spans the full content width.
			 */
			struct cow_container *ctr = cow_view_container(iv);
			if (ctr != NULL && ctr->n_members > 1) {
				int strip_x = btn_s;
				int strip_w = hit_cw - 3 * btn_s;
				if (strip_w > 0 && lx >= strip_x &&
				    lx < strip_x + strip_w) {
					int tab_w = strip_w / ctr->n_members;
					if (tab_w < 1)
						tab_w = 1;
					int tidx = (lx - strip_x) / tab_w;
					if (tidx < 0)
						tidx = 0;
					if (tidx >= ctr->n_members)
						tidx = ctr->n_members - 1;
					struct cow_container_member *tm = NULL;
					int			     ti = 0;
					wl_list_for_each(tm, &ctr->members,
					    link)
					{
						if (ti++ == tidx)
							break;
					}
					if (tm != NULL && tm != ctr->active) {
						cow_container_activate(ctr, tm);
						s->interacted_view = NULL;
						return true;
					}
				}
			}
			iv->title_pressed = true;
			iv->dec.dirty	  = true;
			s->titlebar_press = COW_TITLEBAR_PRESS_TITLE;
			s->pressed_view	  = iv;
			cow_pointer_op_start_move(s, iv);
		}
	} else if (ly < -tt || ly >= hit_ch || lx < 0 || lx >= hit_cw) {
		/* Border edge (not in content, not in titlebar, not corner) */
		uint32_t edges = 0;
		if (ly < -tt)
			edges = COW_EDGE_TOP;
		else if (ly >= hit_ch)
			edges = COW_EDGE_BOTTOM;
		else if (lx < 0)
			edges = COW_EDGE_LEFT;
		else if (lx >= hit_cw)
			edges = COW_EDGE_RIGHT;
		if (edges != 0)
			cow_pointer_op_start_resize(s, iv, edges);
	}
	s->interacted_view = NULL;
	return false;
}

static void
seat_apply_sloppy_focus(struct cow_seat *s)
{
	if (s->sloppy_focus_view != NULL && !s->sloppy_focus_view->closed &&
	    s->sloppy_focus_view != s->focused_view) {
		struct cow_view *sv = s->sloppy_focus_view;
		river_seat_v1_focus_window(s->river_seat, sv->river_window);
		s->focused_view	    = sv;
		cow_wm.focused_view = sv;
		if (sv->output != NULL)
			cow_wm.current_output = sv->output;
		cow_status_emit();
	}
	s->sloppy_focus_view = NULL;
}

static void
manage_process_seat(struct cow_seat *s)
{
	cow_seat_enable_all_bindings(s);
	cow_seat_process_bindings(s);

	seat_dispatch_titlebar_button_release(s);
	cow_pointer_op_process(s);
	seat_update_drag_drop_target(s);
	seat_maybe_create_drag_ghost(s);
	(void)seat_dispatch_decoration_click(s);
	seat_apply_sloppy_focus(s);
}

/* Phase 5: restore saved state on the first manage cycle. */
static void
manage_restore_state_once(void)
{
	if (cow_wm.exec_pending && !cow_wm.state_restored)
		cow_state_restore();
}

static void
manage_apply_pending_view_geom(void)
{
	struct cow_view *view;
	wl_list_for_each(view, &cow_wm.views, wm_link)
	{
		if (view->pending_close && !view->closed) {
			river_window_v1_close(view->river_window);
			view->pending_close = false;
		}
		if (view->pending_resize && !view->closed) {
			view->x = view->pending_x;
			view->y = view->pending_y;
			river_window_v1_propose_dimensions(view->river_window,
			    view->pending_width, view->pending_height);
			view->pending_resize = false;
			view->dec.dirty	     = true;
			cow_view_sync_vxy_from_global(view);
		}
		/* Note: clip box changes are rendering state and are
		 * applied in handle_wm_render_start, not here. */
	}
}

static void
manage_apply_pending_focus(void)
{
	if (cow_wm.pending_focus != NULL && !cow_wm.pending_focus->closed) {
		struct cow_seat *fs = cow_seat_primary();
		if (fs != NULL)
			cow_seat_focus_view(fs, cow_wm.pending_focus);
		cow_wm.pending_focus = NULL;
	}

	struct cow_view *view;
	wl_list_for_each(view, &cow_wm.views, wm_link)
	{
		if (view->needs_focus && !view->closed) {
			struct cow_seat *nfs = cow_seat_primary();
			if (nfs != NULL)
				cow_seat_focus_view(nfs, view);
			view->needs_focus = false;
		}
	}
}

static void
manage_apply_pending_warp(void)
{
	if (!cow_wm.pending_warp)
		return;
	struct cow_seat *ws = cow_seat_primary();
	if (ws != NULL)
		river_seat_v1_pointer_warp(ws->river_seat, cow_wm.warp_x,
		    cow_wm.warp_y);
	cow_wm.pending_warp = false;
}

static void
manage_run_deferred_config_once(void)
{
	if (!cow_wm.exec_pending)
		return;
	cow_wm.exec_pending = false;
	cow_rule_run_deferred();

	struct cow_view *rv;
	wl_list_for_each(rv, &cow_wm.views, wm_link)
	{
		if (!rv->closed) {
			cow_view_resolve_style(rv);
			rv->dec.dirty = true;
		}
	}

	cow_exec_run_deferred();
	pan_frames_update_all();
}

static void
handle_wm_manage_start(__unused void *data,
    struct river_window_manager_v1   *manager)
{
	wm_enter_sequence(COW_WM_SEQUENCE_MANAGE);

	manage_reap_destroyed_objects();
	manage_tick_outputs();
	manage_configure_new_views();

	struct cow_seat *s;
	wl_list_for_each(s, &cow_wm.seats, wm_link) manage_process_seat(s);

	manage_restore_state_once();
	manage_apply_pending_view_geom();
	manage_apply_pending_focus();
	manage_apply_pending_warp();

	river_window_manager_v1_manage_finish(manager);
	wm_leave_sequence();

	manage_run_deferred_config_once();
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

	/* Draw a filled circle with a border using Cairo */
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

	cow_wm.drag_ghost_surface   = surf;
	cow_wm.drag_ghost_shell	    = shell;
	cow_wm.drag_ghost_node	    = river_shell_surface_v1_get_node(shell);
	cow_wm.drag_ghost_buffer    = buf;
	cow_wm.drag_ghost_committed = false;
	/* Use the current pointer position so the circle appears under
	 * the cursor immediately, not at the source window's origin. */
	cow_seat_get_pointer(&cow_wm.drag_ghost_x, &cow_wm.drag_ghost_y);
}

void
cow_drag_ghost_destroy(void)
{
	COW_DESTROY_AND_NULL(cow_wm.drag_ghost_node, river_node_v1_destroy);
	COW_DESTROY_AND_NULL(cow_wm.drag_ghost_shell,
	    river_shell_surface_v1_destroy);
	COW_DESTROY_AND_NULL(cow_wm.drag_ghost_surface, wl_surface_destroy);
	cow_shm_buffer_destroy(cow_wm.drag_ghost_buffer);
	cow_wm.drag_ghost_buffer    = NULL;
	cow_wm.drag_ghost_committed = false;
}

/* Render output backgrounds for the current cycle. */
static void
render_output_backgrounds(void)
{
	struct cow_output *out;
	wl_list_for_each(out, &cow_wm.outputs, wm_link)
	    cow_output_render_background(out);
}

/* Apply rendering state for every live view; render icons for iconified
 * ones.
 */
static void
render_views(void)
{
	struct cow_view *view;
	wl_list_for_each(view, &cow_wm.views, wm_link)
	{
		if (view->closed)
			continue;
		cow_view_apply_render(view);
		if (cow_view_is_iconified(view))
			cow_view_icon_render(view);
	}
}

/* Position + commit the drag-ghost surface if a container drag is in
 * progress. */
static void
render_drag_ghost(void)
{
	if (cow_wm.drag_ghost_node == NULL)
		return;
	river_node_v1_set_position(cow_wm.drag_ghost_node,
	    cow_wm.drag_ghost_x - GHOST_RADIUS,
	    cow_wm.drag_ghost_y - GHOST_RADIUS);
	river_node_v1_place_top(cow_wm.drag_ghost_node);
	if (!cow_wm.drag_ghost_committed) {
		wl_surface_attach(cow_wm.drag_ghost_surface,
		    cow_wm.drag_ghost_buffer->buffer, 0, 0);
		wl_surface_damage_buffer(cow_wm.drag_ghost_surface, 0, 0,
		    GHOST_RADIUS * 2, GHOST_RADIUS * 2);
		river_shell_surface_v1_sync_next_commit(
		    cow_wm.drag_ghost_shell);
		wl_surface_commit(cow_wm.drag_ghost_surface);
		cow_wm.drag_ghost_committed = true;
	}
}

static void
handle_wm_render_start(__unused void *data,
    struct river_window_manager_v1   *manager)
{
	wm_enter_sequence(COW_WM_SEQUENCE_RENDER);

	render_output_backgrounds();
	render_views();
	render_drag_ghost();

	river_window_manager_v1_render_finish(manager);
	wm_leave_sequence();
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
}

/* Reset the cow_wm global to a clean zero state and initialise its
 * intrusive lists.  Called once at the very start of cow_wm_init. */
static void
wm_state_reset(void)
{
	memset(&cow_wm, 0, sizeof(cow_wm));

	wl_list_init(&cow_wm.views);
	wl_list_init(&cow_wm.outputs);
	wl_list_init(&cow_wm.seats);
	wl_list_init(&cow_wm.containers);
	wl_list_init(&cow_wm.new_views);
	wl_list_init(&cow_wm.closed_views);
	wl_list_init(&cow_wm.wl_outputs);
}

/* Connect to the Wayland display and bind the registry.  Returns false
 * on failure (with the global cow_wm.display left NULL).
 */
static bool
wm_connect_display(void)
{
	cow_wm.display = wl_display_connect(NULL);
	if (cow_wm.display == NULL) {
		fprintf(stderr, "cow: failed to connect to Wayland display\n");
		return false;
	}
	cow_wm.registry = wl_display_get_registry(cow_wm.display);
	wl_registry_add_listener(cow_wm.registry, &registry_listener, NULL);
	return true;
}

/* Tear down the Wayland connection objects established by
 * wm_connect_display.  Used by both the cow_wm_init failure path and
 * cow_wm_fini.
 */
static void
wm_disconnect_display(void)
{
	COW_DESTROY_AND_NULL(cow_wm.registry, wl_registry_destroy);
	COW_DESTROY_AND_NULL(cow_wm.display, wl_display_disconnect);
}

/* Verify that all globals required by cow are present after registry
 * advertisement.  Logs a useful message and returns false on the first
 * missing global.
 */
static bool
wm_validate_globals(void)
{
	if (cow_wm.manager == NULL) {
		fprintf(stderr,
		    "cow: river_window_manager_v1 not available "
		    "(is river running?)\n");
		return false;
	}
	if (cow_wm.xkb_bindings == NULL) {
		fprintf(stderr, "cow: river_xkb_bindings_v1 not available\n");
		return false;
	}
	if (cow_wm.compositor == NULL) {
		fprintf(stderr, "cow: wl_compositor not available\n");
		return false;
	}
	if (cow_wm.shm == NULL) {
		fprintf(stderr, "cow: wl_shm not available\n");
		return false;
	}
	return true;
}

/*
 * Apply reserved area to all outputs after all initial Wayland events
 * have settled.  output_handle_dimensions / position skip this during
 * startup (exec_pending=true); this is the single startup application.
 */
static void
wm_apply_initial_output_state(void)
{
	struct cow_output *co;
	wl_list_for_each(co, &cow_wm.outputs, wm_link)
	    cow_output_apply_reserved_area(co);
}

bool
cow_wm_init(void)
{
	wm_state_reset();
	if (!wm_connect_display())
		return false;

	/* First roundtrip: discover globals */
	wl_display_roundtrip(cow_wm.display);

	if (!wm_validate_globals()) {
		wm_disconnect_display();
		return false;
	}

	/* Second roundtrip: process initial events from bound globals */
	wl_display_roundtrip(cow_wm.display);

	wm_apply_initial_output_state();
	pan_frames_init();

	cow_wm.running	    = true;
	cow_wm.exec_pending = true;

	if (!cow_ipc_init())
		fprintf(stderr, "cow: warning: IPC init failed\n");
	cow_status_init();

	return true;
}

/*
 * Persist saved state and release the IPC + status sockets.  Used
 * by both cow_wm_fini and cow_wm_restart so the on-disk state file
 * is identical regardless of how cow exits.
 */
static void
wm_save_and_release_sockets(void)
{
	cow_state_save();
	cow_status_fini();
	cow_ipc_fini();
}

/* Destroy the river-side protocol objects (manager, xkb bindings, layer
 * shell).  Must precede the raw-Wayland teardown below.
 */
static void
wm_destroy_river_objects(void)
{
	COW_DESTROY_AND_NULL(cow_wm.layer_shell, river_layer_shell_v1_destroy);
	COW_DESTROY_AND_NULL(cow_wm.xkb_bindings,
	    river_xkb_bindings_v1_destroy);
	if (cow_wm.manager != NULL) {
		river_window_manager_v1_stop(cow_wm.manager);
		wl_display_roundtrip(cow_wm.display);
		river_window_manager_v1_destroy(cow_wm.manager);
		cow_wm.manager = NULL;
	}
	pan_frames_destroy_all();
}

/* Destroy raw wl_seat / wl_pointer / pointer-constraints + remaining
 * wl_globals.  No protocol-error exposure here; safe to run after the
 * river side is gone.
 */
static void
wm_destroy_wl_globals(void)
{
	COW_DESTROY_AND_NULL(cow_wm.wl_pointer_raw, wl_pointer_destroy);
	COW_DESTROY_AND_NULL(cow_wm.wl_seat_raw, wl_seat_destroy);
	COW_DESTROY_AND_NULL(cow_wm.wl_pointer_constraints,
	    zwp_pointer_constraints_v1_destroy);
	COW_DESTROY_AND_NULL(cow_wm.wl_layer_shell,
	    zwlr_layer_shell_v1_destroy);
	COW_DESTROY_AND_NULL(cow_wm.compositor, wl_compositor_destroy);
	COW_DESTROY_AND_NULL(cow_wm.shm, wl_shm_destroy);
}

void
cow_wm_fini(void)
{
	wm_save_and_release_sockets();
	wm_destroy_river_objects();
	wm_destroy_wl_globals();
	wm_disconnect_display();
}

void
cow_wm_run(void)
{
	int wl_fd = wl_display_get_fd(cow_wm.display);

	while (cow_wm.running) {
		/* Flush outgoing requests */
		if (wl_display_flush(cow_wm.display) < 0 && errno != EAGAIN) {
			int err = wl_display_get_error(cow_wm.display);
			if (err != 0) {
				uint32_t		   id	 = 0;
				const struct wl_interface *iface = NULL;
				int proto_err = wl_display_get_protocol_error(
				    cow_wm.display, &iface, &id);
				fprintf(stderr,
				    "cow: protocol error %d on %s@%u\n",
				    proto_err, iface ? iface->name : "unknown",
				    id);
			}
			break;
		}

		/* Prepare to read Wayland events (non-blocking) */
		while (wl_display_prepare_read(cow_wm.display) != 0)
			wl_display_dispatch_pending(cow_wm.display);

		/* Build poll fd array: Wayland fd + IPC fds */
		struct pollfd fds[COW_IPC_MAX_CLIENTS + 2];
		int	      nfds = 0;

		fds[nfds].fd	  = wl_fd;
		fds[nfds].events  = POLLIN;
		fds[nfds].revents = 0;
		nfds++;

		int ipc_nfds  = cow_ipc_fill_pollfds(fds + nfds);
		int total_fds = nfds + ipc_nfds;

		/* If any pan frame dwell timer is armed, wake up frequently
		 * so cow_output_pan_frame_tick() fires promptly without
		 * waiting for a River manage event.
		 */
		int poll_timeout = -1;
		{
			struct cow_output *po;
			wl_list_for_each(po, &cow_wm.outputs, wm_link)
			{
				if (po->pan_edge >= 0) {
					poll_timeout = 50; /* ms */
					break;
				}
			}
		}

		int ret = poll(fds, total_fds, poll_timeout);
		if (ret < 0) {
			if (errno == EINTR) {
				wl_display_cancel_read(cow_wm.display);
				if (cow_wm_shutdown_requested ||
				    cow_wm_restart_requested)
					cow_wm.running = false;
				continue;
			}
			wl_display_cancel_read(cow_wm.display);
			break;
		}

		/* Handle Wayland events */
		if (fds[0].revents & POLLIN) {
			wl_display_read_events(cow_wm.display);
		} else {
			wl_display_cancel_read(cow_wm.display);
		}
		wl_display_dispatch_pending(cow_wm.display);

		/* Poll timeout with armed pan edge: kick a manage cycle so
		 * cow_output_pan_frame_tick() can fire the page switch. */
		if (ret == 0 && poll_timeout > 0)
			cow_wm_request_manage_dirty();

		/* Handle IPC events */
		if (ipc_nfds > 0)
			cow_ipc_process_events(fds + nfds, ipc_nfds);

		/* If any IPC commands deferred state changes, do a
		 * round-trip to ensure the previous render_finish was
		 * processed, then request a new render cycle. */
		if (cow_wm.needs_manage_dirty) {
			cow_wm.needs_manage_dirty = false;
			wl_display_roundtrip(cow_wm.display);
			cow_wm_manage_dirty();
		}

		if (cow_wm_shutdown_requested || cow_wm_restart_requested)
			cow_wm.running = false;
	}
}

void
cow_wm_manage_dirty(void)
{
	if (cow_wm.manager != NULL)
		river_window_manager_v1_manage_dirty(cow_wm.manager);
}

void
cow_wm_request_manage_dirty(void)
{
	cow_wm.needs_manage_dirty = true;
}

void
cow_wm_request_pointer_warp(int x, int y)
{
	cow_wm.pending_warp = true;
	cow_wm.warp_x	    = x;
	cow_wm.warp_y	    = y;
}

/*
 * Shutdown / restart are signalled into the run loop via two
 * sig_atomic_t flags rather than executed directly from the signal
 * handler.  The original SIGTERM path called cow_state_save() (which
 * touches the filesystem) from inside the handler -- not async-signal
 * safe.  The flag-only design keeps the handler bodies trivial and
 * lets cow_wm_run drain shutdown the same way it drains restart, with
 * normal teardown via cow_wm_fini in main().
 */
volatile sig_atomic_t cow_wm_shutdown_requested = 0;
volatile sig_atomic_t cow_wm_restart_requested	= 0;

void
cow_wm_signal_handler(__unused int sig)
{
	cow_wm_shutdown_requested = 1;
}

void
cow_wm_restart_handler(__unused int sig)
{
	cow_wm_restart_requested = 1;
}

void
cow_wm_restart(char **argv)
{
	/* Run the restart hook synchronously before anything else, so the
	 * user can kill processes, clean up, etc. before the new process
	 * starts.
	 */
	if (cow_config != NULL && cow_config->hook_restart != NULL) {
		fprintf(stderr, "cow: running restart hook: %s\n",
		    cow_config->hook_restart);
		cow_command_execute(cow_config->hook_restart);
	}

	/* Save state so the new process can restore it. */
	cow_state_save();

	/* Release IPC and status sockets so the new process can bind them.
	 * Do NOT send stop/destroy to River -- the new process takes over
	 * the existing window manager session immediately. */
	cow_status_fini();
	cow_ipc_fini();

	fprintf(stderr, "cow: restarting (%s)...\n", argv[0]);
	execvp(argv[0], argv);
	fprintf(stderr, "cow: restart failed: %s\n", strerror(errno));
	_exit(EXIT_FAILURE);
}
