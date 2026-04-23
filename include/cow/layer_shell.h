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
#if !defined(COW_LAYER_SHELL_H)
#define COW_LAYER_SHELL_H

#include <stdbool.h>

struct cow_output;
struct cow_seat;

/*
 * Per-output layer shell state.
 * Tracks the non-exclusive area (usable area after layer shell
 * exclusive zones are subtracted).
 */
struct cow_layer_shell_output {
	struct river_layer_shell_output_v1 *river_ls_output;
	struct cow_output		   *output;
};

/*
 * Per-seat layer shell state.
 * Tracks whether a layer shell surface has exclusive focus.
 */
struct cow_layer_shell_seat {
	struct river_layer_shell_seat_v1 *river_ls_seat;
	struct cow_seat			 *seat;
	bool				  has_exclusive_focus;
};

/*
 * Initialize layer shell bindings for an output.
 * Binds river_layer_shell_v1.get_output to receive non_exclusive_area events.
 */
void cow_layer_shell_init_output(struct cow_output *output);

/*
 * Initialize layer shell bindings for a seat.
 * Binds river_layer_shell_v1.get_seat to receive focus events.
 */
void cow_layer_shell_init_seat(struct cow_seat *seat);

#endif
