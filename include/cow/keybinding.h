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
#if !defined(COW_KEYBINDING_H)
#define COW_KEYBINDING_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

struct river_xkb_binding_v1;
struct river_pointer_binding_v1;
struct cow_seat;
struct cow_view;

/*
 * Action callback.
 */
typedef void (*cow_action_fn)(void *);

/*
 * A key binding registered with river.
 *
 * Repeat state:
 *   repeatable   -- the user registered this with `bind -r`.
 *   held         -- river reported `pressed` and we have not yet
 *                   seen `released` or `stop_repeat`.
 *   last_fire_ms -- monotonic ms of the most recent fire of this
 *                   binding; used by cow_seat_process_bindings to
 *                   decide whether the next repeat is due.
 *   first_repeat -- true between the first fire (on press) and the
 *                   first repeat; gates the initial repeat_delay_ms
 *                   dwell vs. the steady-state repeat_interval_ms.
 */
struct cow_keybinding {
	struct wl_list link; /* cow_seat.keybindings */

	struct river_xkb_binding_v1 *river_binding;

	xkb_keysym_t keysym;
	uint32_t     modifiers;

	cow_action_fn action;
	void	     *action_arg;

	struct cow_seat *seat;
	bool		 enabled;
	bool pending; /* pressed event received, waiting for manage */

	bool repeatable;
	bool held;
	bool first_repeat;
	long last_fire_ms;
};

/*
 * A pointer binding registered with river.
 */
struct cow_pointer_binding {
	struct wl_list link; /* cow_seat.pointer_bindings */

	struct river_pointer_binding_v1 *river_binding;

	uint32_t button;
	uint32_t modifiers;

	cow_action_fn action;
	void	     *action_arg;

	struct cow_seat *seat;
	bool		 enabled;
	bool		 pending;
};

/*
 * Create a key binding for the given seat.  When `repeatable` is
 * true the binding's action will be re-fired while the triggering
 * key is held, paced by cow_config->repeat_delay_ms /
 * repeat_interval_ms.
 */
struct cow_keybinding *cow_keybinding_create(struct cow_seat *, xkb_keysym_t,
    uint32_t, cow_action_fn, void *, bool repeatable);

/*
 * Create a pointer binding for the given seat.
 */
struct cow_pointer_binding *cow_pointer_binding_create(struct cow_seat *,
    uint32_t, uint32_t, cow_action_fn, void *);

/*
 * Enable all keybindings and pointer bindings for a seat.
 * Must be called during a manage sequence.
 */
void cow_seat_enable_all_bindings(struct cow_seat *);

/*
 * Process pending binding events during a manage sequence.
 */
void cow_seat_process_bindings(struct cow_seat *);

/*
 * Register the default set of keybindings for a seat.
 */
void cow_seat_register_default_bindings(struct cow_seat *);

/*
 * Resolve an action name string to a function pointer + argument.
 * Returns true if the action was found.
 */
bool cow_resolve_action(const char *, cow_action_fn *, void **);

/*
 * Destroy all keybindings and pointer bindings for a seat.
 */
void cow_seat_destroy_bindings(struct cow_seat *);

/*
 * Teardown and re-register bindings on all seats from config.
 */
void cow_rebind_all_seats(void);

/*
 * Return ms-from-now until the next held repeatable binding is due
 * to re-fire, across all seats.  Returns -1 if no repeatable
 * binding is currently held (in which case the main loop can
 * poll() without a timeout from this source).  Used by the main
 * loop to shorten its poll timeout so the next repeat fires
 * promptly instead of waiting for an unrelated river event.
 */
int cow_seat_next_repeat_ms(void);

#endif
