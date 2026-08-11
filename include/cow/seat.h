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
#if !defined(COW_SEAT_H)
#define COW_SEAT_H

#include <stdbool.h>
#include <time.h>
#include <wayland-client.h>

#include "cow/pointer_op.h"

struct river_seat_v1;
struct river_xkb_bindings_seat_v1;
struct cow_view;

/*
 * A seat tracked by the window manager.
 * Wraps a river_seat_v1 object.
 */
struct cow_seat {
	struct river_seat_v1		  *river_seat;
	struct river_xkb_bindings_seat_v1 *xkb_seat;
	struct wl_list			   wm_link; /* cow_wm.seats */

	/* Registered bindings */
	struct wl_list keybindings;	 /* cow_keybinding.link */
	struct wl_list pointer_bindings; /* cow_pointer_binding.link */
	bool	       bindings_enabled;
	uint32_t	binding_context;
	struct cow_view *binding_context_view;
	struct cow_view *binding_action_view;
	bool		binding_context_dirty;
	bool		binding_context_suspended;
	bool		binding_context_deferred;
	uint32_t	binding_context_deferred_value;
	struct cow_view *binding_context_deferred_view;
	uint32_t       watched_modifiers;
	uint32_t       active_modifiers;
	bool	       modifier_watch_dirty;

	/* Corresponding wl_seat global name */
	uint32_t wl_seat_name;

	/* Current pointer state */
	int  pointer_x, pointer_y;
	bool has_pointer_position;

	/* The window the pointer is currently over */
	struct cow_view *pointer_entered_view;

	/* The window that was last interacted with */
	struct cow_view *interacted_view;
	struct cow_view *pending_icon_interaction;
	struct cow_view *consumed_icon_interaction;

	/* The currently focused view (keyboard focus) */
	struct cow_view *focused_view;
	struct cow_view *previous_focused_view;
	bool		 needs_refocus;

	/* Pending sloppy focus (set on pointer_enter, applied in manage) */
	struct cow_view *sloppy_focus_view;

	/* Double-click detection for decoration bindings. */
	struct cow_view *last_click_view;
	int		 last_click_region;
	struct timespec	 last_click_time;
	bool pending_shade;   /* set when double-click detected in decoration */
	int  shade_direction; /* COW_SHADE_* direction from double-click */

	/* A titlebar-button single click awaiting the double-click deadline. */
	struct cow_view *pending_button_click_view;
	int		 pending_button_click_idx;
	long		 pending_button_click_deadline_ms;

	/* Pointer operation state */
	struct cow_pointer_op op;
	bool		      op_in_progress;
	bool		      cursor_hide_op_active;
	int		      op_dx, op_dy;
	bool		      op_released;

	/* Desk assign mode (ensure_next_key_eaten) */
	bool awaiting_desk_assign;

	/* Titlebar interaction state */
	enum {
		COW_TITLEBAR_PRESS_NONE,
		COW_TITLEBAR_PRESS_TITLE,
		COW_TITLEBAR_PRESS_BUTTON,
	} titlebar_press;
	int		 pressed_button_idx;
	struct cow_view *pressed_view;	     /* view with pressed decoration */

	bool removed;

	/*
	 * Menu click flag.  Set by the raw wl_pointer.button hook in
	 * pan_frames.c when a button press arrives while menus are
	 * open; consumed and acted on in cow_menu_process_input().
	 */
	bool menu_click_pending;

	/*
	 * Set when the current window-move was started with -C (container
	 * drag mode).  When op_released, if drag_drop_target is set, the
	 * two views are grouped into a container instead of completing a
	 * normal move.  Cleared on every op release.
	 */
	bool container_drag;

	/* Per-seat layer-shell state (opaque to consumers). */
	struct cow_layer_shell_seat *layer_shell_state;
};

/*
 * Create a new seat wrapper for the given river_seat_v1.
 */
struct cow_seat *cow_seat_create(struct river_seat_v1 *);

/*
 * Destroy a seat wrapper.
 */
void cow_seat_destroy(struct cow_seat *);

/*
 * Focus a window from this seat.
 * Must be called during a manage sequence.
 */
void cow_seat_focus_view(struct cow_seat *, struct cow_view *);
void cow_seat_focus_view_no_raise(struct cow_seat *, struct cow_view *);

/*
 * Find a seat by its river_seat_v1 pointer.
 */
struct cow_seat *cow_seat_find(struct river_seat_v1 *);

/*
 * Return the first (primary) seat, or NULL if none.
 */
struct cow_seat *cow_seat_primary(void);

/*
 * Get the current pointer position from the first seat that has one.
 * Returns true if a position was found.
 */
bool cow_seat_get_pointer(int *, int *);

/*
 * Return the first seat with a known pointer position, or NULL.
 */
struct cow_seat *cow_seat_first_with_pointer(void);

/*
 * Clear all back-references to 'view' on this seat (focused, pressed,
 * interacted, sloppy_focus, last_click, pointer_entered, op view).
 * If the view was the seat's keyboard focus, also send a clear-focus
 * to river so its Window.destroy assert (no seat focuses this window)
 * can pass.
 */
void cow_seat_forget_view(struct cow_seat *, struct cow_view *);

#endif
