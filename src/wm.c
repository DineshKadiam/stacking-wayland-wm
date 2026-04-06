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
#include "cow/wm.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <wayland-client.h>

#include "cow/wm.h"
#include "cow/cmd.h"
#include "cow/command.h"
#include "cow/config.h"
#include "cow/ipc.h"
#include "cow/keybinding.h"
#include "cow/memory.h"
#include "cow/output.h"
#include "cow/pointer_op.h"
#include "cow/seat.h"
#include "cow/state.h"
#include "cow/status.h"
#include "cow/view.h"

#include "river-layer-shell-v1-client-protocol.h"
#include "river-window-management-v1-client-protocol.h"
#include "river-xkb-bindings-v1-client-protocol.h"

struct cow_wm cow_wm;

/* Forward declarations for protocol listeners */
static void handle_wm_unavailable(void *, struct river_window_manager_v1 *);
static void handle_wm_finished(void *, struct river_window_manager_v1 *);
static void handle_wm_manage_start(void *, struct river_window_manager_v1 *);
static void handle_wm_render_start(void *, struct river_window_manager_v1 *);
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
}

static const struct wl_registry_listener registry_listener = {
	.global	       = registry_global,
	.global_remove = registry_global_remove,
};

/*
 * Window manager event handlers.
 */

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

static void
handle_wm_manage_start(__unused void *data,
    struct river_window_manager_v1   *manager)
{
	cow_wm.sequence = COW_WM_SEQUENCE_MANAGE;

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

	struct cow_output *out;
	wl_list_for_each(out, &cow_wm.outputs, wm_link)
	{
		cow_output_prepare_background(out);
	}

	wl_list_for_each(view, &cow_wm.views, wm_link)
	{
		if (!view->configured && !view->closed)
			cow_view_configure(view);
	}

	struct cow_seat *s;
	wl_list_for_each(s, &cow_wm.seats, wm_link)
	{
		cow_seat_enable_all_bindings(s);
		cow_seat_process_bindings(s);

		if (s->op_released &&
		    s->titlebar_press == COW_TITLEBAR_PRESS_BUTTON) {
			struct cow_view *pv = s->pressed_view;
			if (pv != NULL && !pv->closed) {
				bool still_on_button = false;
				if (s->has_pointer_position) {
					int tt	  = pv->dec.title_thickness;
					int bw	  = pv->dec.border_width;
					int lx	  = s->pointer_x - pv->x;
					int ly	  = s->pointer_y - pv->y;
					int tb_w  = pv->content_width;
					int btn_s = tt;
					int bi	  = s->pressed_button_idx;
					if (ly >= -(tt + bw) && ly < 0) {
						int hit = -1;
						if (lx >= 0 && lx < btn_s)
							hit = 0;
						else if (lx >=
							tb_w - 2 * btn_s &&
						    lx < tb_w - btn_s)
							hit = 1;
						else if (lx >= tb_w - btn_s &&
						    lx < tb_w)
							hit = 2;
						still_on_button = (hit == bi);
					}
				}
				if (still_on_button && cow_config != NULL) {
					int bi = s->pressed_button_idx;
					if (bi >= 0 && bi < 3) {
						enum cow_button_action act =
						    cow_config->titlebar
							.buttons[bi]
							.action;
						cow_wm.focused_view = pv;
						if (pv->output != NULL)
							cow_wm.current_output =
							    pv->output;
						if (act ==
						    COW_BUTTON_ACTION_CLOSE)
							river_window_v1_close(
							    pv->river_window);
						else if (act ==
						    COW_BUTTON_ACTION_MAXIMIZE) {
							if (pv->maximized)
								cow_view_unmaximize(
								    pv);
							else
								cow_view_maximize(
								    pv);
						} else if (act ==
						    COW_BUTTON_ACTION_ICONIFY) {
							cow_view_iconify(pv);
						}
					}
				}
				/* Always restore button frame to raised after
				 * release */
				pv->pressed_button = -1;
				pv->dec.dirty	   = true;
			}
			river_seat_v1_op_end(s->river_seat);
			s->op_in_progress = false;
			s->op_released	  = false;
			s->titlebar_press = COW_TITLEBAR_PRESS_NONE;
			s->pressed_view	  = NULL;
		}

		/* Process pointer operations (move/resize) */
		cow_pointer_op_process(s);

		/* Handle focus + decoration click dispatch */
		if (s->interacted_view != NULL && !s->interacted_view->closed) {
			struct cow_view *iv = s->interacted_view;
			cow_seat_focus_view(s, iv);

			if (s->has_pointer_position && iv->use_ssd &&
			    !s->op_in_progress) {
				int bw = iv->dec.border_width;
				int tt = iv->dec.title_thickness;
				int lx = s->pointer_x - iv->x;
				int ly = s->pointer_y - iv->y;
				int cl = cow_config ?
				    cow_config->corner_length :
				    20;
				if (tt > 0 && cl < bw + tt)
					cl = bw + tt;

				int hit_cw = iv->sd != COW_SHADE_NONE ?
				    iv->shade_saved_width :
				    iv->content_width;
				int hit_ch = iv->sd != COW_SHADE_NONE ?
				    iv->shade_saved_height :
				    iv->content_height;

				/* Double-click on titlebar -> shade/unshade
				 * (north) */
				bool in_titlebar = (ly >= -tt && ly < 0 &&
						       tt > 0) ||
				    (iv->sd != COW_SHADE_NONE);
				if (in_titlebar) {
					struct timespec now;
					clock_gettime(CLOCK_MONOTONIC, &now);
					if (s->last_click_view == iv &&
					    elapsed_ms(&s->last_click_time,
						&now) < DOUBLE_CLICK_MS) {
						cow_view_shade(iv,
						    COW_SHADE_NORTH);
						s->last_click_view = NULL;
						s->interacted_view = NULL;
						continue;
					}
					s->last_click_view = iv;
					s->last_click_time = now;
				}

				/* Corner resize detection.
				 *
				 * The corner zone is an L-shaped region at
				 * each corner of the frame, extending `cl`
				 * pixels along each edge.  The pointer must
				 * be in the border area (not content) AND
				 * within `cl` of a corner along either axis.
				 *
				 * This makes corners much easier to grab
				 * than requiring the pointer to be within
				 * `cl` of both edges simultaneously.
				 */
				int	 frame_w      = hit_cw + 2 * bw;
				int	 frame_h      = hit_ch + tt + 2 * bw;
				int	 fx	      = lx + bw;
				int	 fy	      = ly + bw + tt;
				bool	 in_corner    = false;
				uint32_t corner_edges = 0;

				/* Check if pointer is in the border area
				 * (outside the content rectangle) */
				bool in_border = (fx >= 0 && fx < frame_w &&
				    fy >= 0 && fy < frame_h &&
				    (fx < bw || fx >= frame_w - bw ||
					fy < bw + tt || fy >= frame_h - bw));

				if (in_border) {
					bool near_top	= (fy < cl);
					bool near_bot	= (fy >= frame_h - cl);
					bool near_left	= (fx < cl);
					bool near_right = (fx >= frame_w - cl);

					/* Don't treat titlebar button
					 * areas as corners.  Buttons
					 * occupy the leftmost and two
					 * rightmost titlebar-height
					 * squares. */
					bool in_tb_btn = false;
					if (tt > 0 && ly >= -tt && ly < 0) {
						if (lx >= 0 && lx < tt)
							in_tb_btn = true;
						if (lx >= hit_cw - 2 * tt &&
						    lx < hit_cw)
							in_tb_btn = true;
					}

					if (near_top && near_left &&
					    !in_tb_btn) {
						corner_edges = COW_EDGE_TOP |
						    COW_EDGE_LEFT;
						in_corner = true;
					} else if (near_top && near_right &&
					    !in_tb_btn) {
						corner_edges = COW_EDGE_TOP |
						    COW_EDGE_RIGHT;
						in_corner = true;
					} else if (near_bot && near_left) {
						corner_edges = COW_EDGE_BOTTOM |
						    COW_EDGE_LEFT;
						in_corner = true;
					} else if (near_bot && near_right) {
						corner_edges = COW_EDGE_BOTTOM |
						    COW_EDGE_RIGHT;
						in_corner = true;
					}
				}

				if (in_corner && corner_edges != 0) {
					cow_pointer_op_start_resize(s, iv,
					    corner_edges);
				} else if (ly >= -tt && ly < 0 && tt > 0) {
					/* Titlebar (below north border, above
					 * content) */
					int tb_w    = hit_cw;
					int btn_s   = tt;
					int hit_btn = -1;
					if (lx >= 0 && lx < btn_s)
						hit_btn = 0;
					else if (lx >= tb_w - 2 * btn_s &&
					    lx < tb_w - btn_s)
						hit_btn = 1;
					else if (lx >= tb_w - btn_s &&
					    lx < tb_w)
						hit_btn = 2;

					if (hit_btn >= 0) {
						iv->pressed_button = hit_btn;
						iv->dec.dirty	   = true;
						s->titlebar_press =
						    COW_TITLEBAR_PRESS_BUTTON;
						s->pressed_button_idx = hit_btn;
						s->pressed_view	      = iv;
						river_seat_v1_op_start_pointer(
						    s->river_seat);
						s->op_in_progress = true;
						s->op_released	  = false;
					} else {
						iv->title_pressed = true;
						iv->dec.dirty	  = true;
						s->titlebar_press =
						    COW_TITLEBAR_PRESS_TITLE;
						s->pressed_view = iv;
						cow_pointer_op_start_move(s,
						    iv);
					}
				} else if (ly < -tt || ly >= hit_ch || lx < 0 ||
				    lx >= hit_cw) {
					/* Border edge (not in content, not in
					 * titlebar, not corner) */
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
						cow_pointer_op_start_resize(s,
						    iv, edges);
				}
			}
			s->interacted_view = NULL;
		}

		/* Sloppy focus */
		if (s->sloppy_focus_view != NULL &&
		    !s->sloppy_focus_view->closed &&
		    s->sloppy_focus_view != s->focused_view) {
			struct cow_view *sv = s->sloppy_focus_view;
			river_seat_v1_focus_window(s->river_seat,
			    sv->river_window);
			s->focused_view	    = sv;
			cow_wm.focused_view = sv;
			if (sv->output != NULL)
				cow_wm.current_output = sv->output;
			cow_status_emit();
		}
		s->sloppy_focus_view = NULL;
	}

	/* Restore saved state on the first manage cycle.  Must happen before
	 * the pending_resize loop below so restored geometry is applied in
	 * the same cycle via propose_dimensions. */
	if (cow_wm.exec_pending && !cow_wm.state_restored) {
		cow_wm.state_restored = true;
		cow_state_restore();
	}

	/* Apply deferred geometry changes from IPC commands and
	 * keybinding-invoked commands.  propose_dimensions is only
	 * valid during the manage callback. */
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
		}
		/* Note: clip box changes are rendering state and are
		 * applied in handle_wm_render_start, not here. */
	}

	/* Apply deferred focus from IPC/keybinding commands. */
	if (cow_wm.pending_focus != NULL && !cow_wm.pending_focus->closed) {
		struct cow_seat *fs;
		wl_list_for_each(fs, &cow_wm.seats, wm_link)
		{
			cow_seat_focus_view(fs, cow_wm.pending_focus);
			break;
		}
		cow_wm.pending_focus = NULL;
	}

	/* Focus newly created windows. */
	wl_list_for_each(view, &cow_wm.views, wm_link)
	{
		if (view->needs_focus && !view->closed) {
			struct cow_seat *nfs = cow_seat_primary();
			if (nfs != NULL)
				cow_seat_focus_view(nfs, view);
			view->needs_focus = false;
		}
	}

	/* Apply deferred pointer warp. */
	if (cow_wm.pending_warp) {
		struct cow_seat *ws;
		wl_list_for_each(ws, &cow_wm.seats, wm_link)
		{
			river_seat_v1_pointer_warp(ws->river_seat,
			    cow_wm.warp_x, cow_wm.warp_y);
			break;
		}
		cow_wm.pending_warp = false;
	}

	river_window_manager_v1_manage_finish(manager);
	cow_wm.sequence = COW_WM_SEQUENCE_NONE;

	/* Run deferred exec commands on the first manage cycle. */
	if (cow_wm.exec_pending) {
		cow_wm.exec_pending = false;
		cow_rule_run_deferred();

		/* Re-resolve styles for all existing views now that
		 * deferred rules are in place. */
		struct cow_view *rv;
		wl_list_for_each(rv, &cow_wm.views, wm_link)
		{
			if (!rv->closed) {
				cow_view_resolve_style(rv);
				rv->dec.dirty = true;
			}
		}

		cow_exec_run_deferred();
	}
}

static void
handle_wm_render_start(__unused void *data,
    struct river_window_manager_v1   *manager)
{
	cow_wm.sequence = COW_WM_SEQUENCE_RENDER;

	/* Render background surfaces for all outputs */
	struct cow_output *out2;
	wl_list_for_each(out2, &cow_wm.outputs, wm_link)
	{
		cow_output_render_background(out2);
	}

	/* Apply rendering state for all views */
	struct cow_view *view;
	wl_list_for_each(view, &cow_wm.views, wm_link)
	{
		if (!view->closed) {
			cow_view_apply_render(view);
			/* Render icon for iconified views */
			if (cow_view_is_iconified(view))
				cow_view_icon_render(view);
		}
	}

	river_window_manager_v1_render_finish(manager);
	cow_wm.sequence = COW_WM_SEQUENCE_NONE;
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
}

static void
handle_wm_seat(__unused void		    *data,
    __unused struct river_window_manager_v1 *manager,
    struct river_seat_v1		    *seat)
{
	cow_seat_create(seat);
}

bool
cow_wm_init(void)
{
	memset(&cow_wm, 0, sizeof(cow_wm));

	wl_list_init(&cow_wm.views);
	wl_list_init(&cow_wm.outputs);
	wl_list_init(&cow_wm.seats);
	wl_list_init(&cow_wm.new_views);
	wl_list_init(&cow_wm.closed_views);
	wl_list_init(&cow_wm.wl_outputs);

	cow_wm.display = wl_display_connect(NULL);
	if (cow_wm.display == NULL) {
		fprintf(stderr, "cow: failed to connect to Wayland display\n");
		return false;
	}

	cow_wm.registry = wl_display_get_registry(cow_wm.display);
	wl_registry_add_listener(cow_wm.registry, &registry_listener, NULL);

	/* First roundtrip: discover globals */
	wl_display_roundtrip(cow_wm.display);

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

	/* Second roundtrip: process initial events from bound globals */
	wl_display_roundtrip(cow_wm.display);

	cow_wm.running	    = true;
	cow_wm.exec_pending = true;

	/* Initialize IPC command socket */
	if (!cow_ipc_init())
		fprintf(stderr, "cow: warning: IPC init failed\n");

	/* Initialize status FIFO */
	cow_status_init();

	return true;
}

void
cow_wm_fini(void)
{
	cow_state_save();
	cow_status_fini();
	cow_ipc_fini();

	if (cow_wm.layer_shell != NULL)
		river_layer_shell_v1_destroy(cow_wm.layer_shell);
	if (cow_wm.xkb_bindings != NULL)
		river_xkb_bindings_v1_destroy(cow_wm.xkb_bindings);
	if (cow_wm.manager != NULL) {
		river_window_manager_v1_stop(cow_wm.manager);
		wl_display_roundtrip(cow_wm.display);
		river_window_manager_v1_destroy(cow_wm.manager);
	}
	if (cow_wm.compositor != NULL)
		wl_compositor_destroy(cow_wm.compositor);
	if (cow_wm.shm != NULL)
		wl_shm_destroy(cow_wm.shm);
	if (cow_wm.registry != NULL)
		wl_registry_destroy(cow_wm.registry);
	if (cow_wm.display != NULL)
		wl_display_disconnect(cow_wm.display);
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

		int ret = poll(fds, total_fds, -1);
		if (ret < 0) {
			if (errno == EINTR) {
				wl_display_cancel_read(cow_wm.display);
				if (cow_wm_restart_requested)
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

		if (cow_wm_restart_requested)
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
cow_wm_signal_handler(__unused int sig)
{
	cow_state_save();
	_exit(0);
}

volatile sig_atomic_t cow_wm_restart_requested = 0;

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
	 * starts. */
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
