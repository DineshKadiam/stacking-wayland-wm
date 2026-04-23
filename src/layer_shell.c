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
#include "cow/layer_shell.h"

#include <stdio.h>

#include "cow/compat.h"
#include "cow/memory.h"
#include "cow/output.h"
#include "cow/seat.h"
#include "cow/wm.h"

#include "river-layer-shell-v1-client-protocol.h"

/*
 * river_layer_shell_output_v1 event handlers
 */

static void
ls_output_handle_non_exclusive_area(__unused void *data,
    __unused struct river_layer_shell_output_v1 *ls_output, __unused int32_t x,
    __unused int32_t y, __unused int32_t width, __unused int32_t height)
{
	/* River's non_exclusive_area reflects panel exclusive zones.
	 * cow uses its own reserved area (set reserved.*) for usable_area,
	 * applied independently from raw output dimensions.
	 */
}

static const struct river_layer_shell_output_v1_listener ls_output_listener = {
	.non_exclusive_area = ls_output_handle_non_exclusive_area,
};

/*
 * river_layer_shell_seat_v1 event handlers
 */

static void
ls_seat_handle_focus_exclusive(void	      *data,
    __unused struct river_layer_shell_seat_v1 *ls_seat)
{
	struct cow_layer_shell_seat *lss = data;
	lss->has_exclusive_focus	 = true;
}

static void
ls_seat_handle_focus_non_exclusive(void	      *data,
    __unused struct river_layer_shell_seat_v1 *ls_seat)
{
	struct cow_layer_shell_seat *lss = data;
	lss->has_exclusive_focus	 = false;
}

static void
ls_seat_handle_focus_none(void		      *data,
    __unused struct river_layer_shell_seat_v1 *ls_seat)
{
	struct cow_layer_shell_seat *lss = data;
	lss->has_exclusive_focus	 = false;
}

static const struct river_layer_shell_seat_v1_listener ls_seat_listener = {
	.focus_exclusive     = ls_seat_handle_focus_exclusive,
	.focus_non_exclusive = ls_seat_handle_focus_non_exclusive,
	.focus_none	     = ls_seat_handle_focus_none,
};

void
cow_layer_shell_init_output(struct cow_output *output)
{
	if (cow_wm.layer_shell == NULL || output->layer_shell_state != NULL)
		return;

	struct cow_layer_shell_output *lso = xcalloc(1, sizeof(*lso));
	lso->output			   = output;
	lso->river_ls_output		   = river_layer_shell_v1_get_output(
		  cow_wm.layer_shell, output->river_output);

	river_layer_shell_output_v1_add_listener(lso->river_ls_output,
	    &ls_output_listener, lso);

	output->layer_shell_state = lso;
}

void
cow_layer_shell_fini_output(struct cow_output *output)
{
	struct cow_layer_shell_output *lso = output->layer_shell_state;
	if (lso == NULL)
		return;

	if (lso->river_ls_output != NULL)
		river_layer_shell_output_v1_destroy(lso->river_ls_output);
	xfree(lso);
	output->layer_shell_state = NULL;
}

void
cow_layer_shell_init_seat(struct cow_seat *seat)
{
	if (cow_wm.layer_shell == NULL || seat->layer_shell_state != NULL)
		return;

	struct cow_layer_shell_seat *lss = xcalloc(1, sizeof(*lss));
	lss->seat			 = seat;
	lss->has_exclusive_focus	 = false;
	lss->river_ls_seat = river_layer_shell_v1_get_seat(cow_wm.layer_shell,
	    seat->river_seat);

	river_layer_shell_seat_v1_add_listener(lss->river_ls_seat,
	    &ls_seat_listener, lss);

	seat->layer_shell_state = lss;
}

void
cow_layer_shell_fini_seat(struct cow_seat *seat)
{
	struct cow_layer_shell_seat *lss = seat->layer_shell_state;
	if (lss == NULL)
		return;

	if (lss->river_ls_seat != NULL)
		river_layer_shell_seat_v1_destroy(lss->river_ls_seat);
	xfree(lss);
	seat->layer_shell_state = NULL;
}
