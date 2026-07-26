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
#if !defined(COW_WM_H)
#define COW_WM_H

#include <signal.h>
#include <stdbool.h>
#include <wayland-client.h>

#define DOUBLE_CLICK_MS 300

/*
 * How long after the last output add/remove event before
 * on-outputs-changed fires.  Generous enough that wlroots has time
 * to propagate every new wl_output global to other clients before a
 * rule body such as `exec wlr-randr ...` runs.
 */
#define COW_OUTPUTS_CHANGED_DEBOUNCE_MS 250

/*
 * Forward declarations for river protocol objects.
 * The actual types are generated from the protocol XML by wayland-scanner.
 */
struct river_window_manager_v1;
struct zwlr_layer_shell_v1;
struct zwp_pointer_constraints_v1;
struct wp_cursor_shape_manager_v1;
struct wp_cursor_shape_device_v1;
struct wl_cursor_theme;
struct river_xkb_bindings_v1;
struct river_layer_shell_v1;
struct river_window_v1;
struct river_output_v1;
struct river_seat_v1;
struct river_node_v1;
struct river_decoration_v1;
struct river_xkb_binding_v1;
struct river_xkb_bindings_seat_v1;
struct river_pointer_binding_v1;
struct river_shell_surface_v1;

struct cow_view;
struct cow_output;
struct cow_seat;

/*
 * Sequence state: tracks whether we're in a manage or render sequence.
 */
enum cow_wm_sequence {
	COW_WM_SEQUENCE_NONE,
	COW_WM_SEQUENCE_MANAGE,
	COW_WM_SEQUENCE_RENDER,
};

/*
 * The global window manager state.
 * Replaces the old cow_server struct.
 */
struct cow_wm {
	struct wl_display    *display;
	struct wl_registry   *registry;
	struct wl_compositor *compositor;
	struct wl_shm	     *shm;

	/* Raw Wayland seat/pointer/constraints for pan frame input events */
	struct zwlr_layer_shell_v1	  *wl_layer_shell;
	struct wl_seat			  *wl_seat_raw;
	bool				   wl_seat_has_pointer;
	bool				   wl_seat_has_keyboard;
	struct wl_pointer		  *wl_pointer_raw;
	struct zwp_pointer_constraints_v1 *wl_pointer_constraints;
	struct wp_cursor_shape_manager_v1 *wl_cursor_shape_mgr;
	struct wp_cursor_shape_device_v1  *wl_cursor_shape_dev;
	struct wl_surface		  *wl_cursor_surface;
	struct wl_cursor_theme		  *wl_cursor_theme;
	uint32_t			   wl_cursor_shape;
	uint32_t			   wl_cursor_serial;

	/* Currently armed pan edge (at most one across all outputs). */
	struct cow_output *pan_lock_output;
	int		   pan_lock_edge;
	/* Global post-fire cooldown so the post-warp cursor doesn't
	 * immediately re-arm an adjacent edge. */
	long pan_global_cooldown_ms;

	/* River protocol globals */
	struct river_window_manager_v1 *manager;
	struct river_xkb_bindings_v1   *xkb_bindings;
	struct river_layer_shell_v1    *layer_shell;

	/* Sequence state */
	enum cow_wm_sequence sequence;
	bool		     running;
	bool		     session_locked;
	bool		     restarted;
	bool state_restored; /* true after first manage restores saved state */

	/* Lists of tracked objects */
	struct wl_list views;	   /* cow_view.wm_link */
	struct wl_list outputs;	   /* cow_output.wm_link */
	struct wl_list seats;	   /* cow_seat.wm_link */
	struct wl_list containers; /* cow_container.wm_link */

	/* The "current" output and focused view */
	struct cow_output *current_output;
	struct cow_view	  *focused_view;

	/*
	 * Seat whose pointer interaction is currently being dispatched
	 * (decoration click etc.).  Set by wm.c's decoration-click
	 * dispatch right before firing a bind-decoration binding,
	 * cleared after.  Lets `window-move`, `window-resize`, and any
	 * other cmd that starts a pointer op pick the originating seat
	 * for multi-seat setups instead of defaulting to the primary
	 * one.  NULL outside a decoration dispatch.
	 */
	struct cow_seat *current_interaction_seat;

	/* Monotonic counter assigned to a view's raise_seq on each raise. */
	uint64_t raise_seq_counter;

	/* Pending state changes accumulated between manage sequences */
	struct wl_list new_views;    /* views created this sequence */
	struct wl_list closed_views; /* views closed this sequence */

	/* Deferred manage_dirty request from IPC context */
	bool needs_manage_dirty;

	/* Debounced on-outputs-changed firing.  Each output add/remove
	 * sets the deadline to now + COW_OUTPUTS_CHANGED_DEBOUNCE_MS.
	 * The main loop fires the rule once and clears the deadline
	 * after it expires, so a flurry of changes (e.g. a VT-switch
	 * round-trip with seven monitors) results in exactly one fire
	 * after the topology has settled and wlroots has had time to
	 * propagate wl_output globals to other clients (such as
	 * wlr-randr). */
	long outputs_changed_deadline_ms;

	/* Deferred exec commands pending from config loading */
	bool exec_pending;

	/* Deferred focus request from IPC context */
	struct cow_view *pending_focus;

	/* Deferred pointer warp from IPC context */
	bool pending_warp;
	int  warp_x, warp_y;

	/*
	 * Container drag-and-drop state.
	 * drag_drop_source: the view currently being dragged with -C.
	 * drag_drop_target: the view whose titlebar is highlighted as the
	 *                   drop target.  NULL when no drag is in progress.
	 */
	struct cow_view *drag_drop_source;
	struct cow_view *drag_drop_target;

	/*
	 * Drag ghost: a small circle that follows the pointer during a
	 * container-forming drag (-C), giving visual feedback without
	 * moving the source window.
	 */
	struct wl_surface	      *drag_ghost_surface;
	struct river_shell_surface_v1 *drag_ghost_shell;
	struct river_node_v1	      *drag_ghost_node;
	struct cow_shm_buffer	      *drag_ghost_buffer;
	bool			       drag_ghost_committed;

	int drag_ghost_x;
	int drag_ghost_y;

	/* Geometry overlay shown during interactive move/resize. */
	struct wl_surface	      *geometry_surface;
	struct river_shell_surface_v1 *geometry_shell;
	struct river_node_v1	      *geometry_node;
	struct cow_shm_buffer	      *geometry_buffer;
	char			       geometry_text[64];
	int			       geometry_x;
	int			       geometry_y;
	bool			       geometry_dirty;

	/* Bound wl_output objects (for matching to cow_outputs) */
	struct wl_list wl_outputs; /* cow_wl_output_entry.link */
};

/*
 * Temporary record for a bound wl_output global.
 * Used to match registry wl_output objects to cow_output structs.
 */
struct cow_wl_output_entry {
	struct wl_output *wl_output;
	uint32_t	  global_name;
	char	      *name; /* human-readable name from wl_output.name event */
	struct wl_list link;
};

extern struct cow_wm cow_wm;

/*
 * Initialize the WM state and connect to the Wayland display.
 * Returns false on failure.
 */
bool cow_wm_init(void);

/*
 * Run the main event loop.
 */
void cow_wm_run(void);

/*
 * Clean up and disconnect.
 */
void cow_wm_fini(void);

/*
 * Issue a manage_dirty protocol request to the compositor immediately.
 * Most callers should use cow_wm_request_manage_dirty() instead, which
 * defers the request to the next run-loop pass; cow_wm_manage_dirty_now
 * is for code that has just finished a roundtrip and wants the next
 * manage cycle to start without waiting for the run loop.
 */
void cow_wm_manage_dirty_now(void);
void cow_drag_ghost_destroy(void);

/*
 * Set cow_wm.needs_manage_dirty so the run loop will issue a
 * manage_dirty on its next pass.  Use this rather than poking the flag
 * directly so dirty triggers stay greppable and uniform.
 */
void cow_wm_request_manage_dirty(void);

/*
 * Manage the default pointer shape used by River when no client surface
 * owns pointer focus, such as while hovering server-side decorations.
 */
void cow_wm_ensure_cursor_shape_device(void);
void cow_wm_set_cursor_shape(uint32_t, uint32_t);

/*
 * Request that the pointer be warped to (x, y) at the next manage
 * sequence boundary.  Centralises the three places that previously
 * set pending_warp / warp_x / warp_y by hand.
 */
void cow_wm_request_pointer_warp(int, int);

/*
 * Signal handler for SIGTERM/SIGINT -- triggers clean shutdown.
 */
void cow_wm_signal_handler(int);

/*
 * Signal handler for SIGUSR1 -- triggers a clean restart via execv().
 */
void cow_wm_restart_handler(int);

/*
 * Set by SIGTERM/SIGINT handler; checked in the event loop.  When true,
 * the run loop exits and main() proceeds with normal teardown via
 * cow_wm_fini.  Async-signal-safe to write from a signal handler.
 */
extern volatile sig_atomic_t cow_wm_shutdown_requested;

/*
 * Set by SIGUSR1 handler; checked in the event loop.
 */
extern volatile sig_atomic_t cow_wm_restart_requested;

/*
 * Save state, release sockets, and execvp() the new cow process.
 * Does NOT tear down the River session.
 */
void cow_wm_restart(char **);

#endif
