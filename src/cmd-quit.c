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
/*
 * Command: quit -- exit the window manager session or reload config.
 *
 * usage: quit          (exit session)
 *        quit -r       (reload configuration)
 */

#include <stdio.h>

#include "cow/cmd.h"
#include "cow/config.h"
#include "cow/ipc.h"
#include "cow/keybinding.h"
#include "cow/output.h"
#include "cow/seat.h"
#include "cow/view.h"
#include "cow/wm.h"

#include "river-window-management-v1-client-protocol.h"

static enum cmd_retval cmd_quit_exec(struct cmd *, struct cmd_q *);
static void	       do_reload(struct cmd_q *);

struct cmd_entry cmd_quit_entry = {
	"quit",
	"r",
	0,
	0,
	"quit [-r]",
	cmd_quit_exec,
};

static enum cmd_retval
cmd_quit_exec(struct cmd *self, struct cmd_q *cmdq)
{
	struct args *args = self->args;

	if (args_has(args, 'r')) {
		do_reload(cmdq);
		return (CMD_RETURN_NORMAL);
	}

	fprintf(stderr, "cow: exit session\n");
	if (cow_wm.manager != NULL)
		river_window_manager_v1_exit_session(cow_wm.manager);
	cow_wm.running = false;
	return (CMD_RETURN_NORMAL);
}

static void
do_reload(struct cmd_q *cmdq)
{
	/* Use the path the config was originally loaded from, falling back
	 * to the default search path. */
	const char *path = cow_ipc.config_path;
	if (path == NULL)
		path = cow_config_default_path();
	if (path == NULL) {
		cmdq_error(cmdq, "quit -r: no config file found");
		return;
	}

	struct cow_config *old = cow_config;
	cow_config	       = cow_config_create();
	cow_config_destroy(old);

	if (!cow_ipc_load_config(path)) {
		cmdq_error(cmdq, "quit -r: config load failed");
		return;
	}

	struct cow_seat *seat;
	wl_list_for_each(seat, &cow_wm.seats, wm_link)
	{
		cow_seat_destroy_bindings(seat);
		cow_seat_register_default_bindings(seat);
	}

	struct cow_view *view;
	wl_list_for_each(view, &cow_wm.views, wm_link)
	{
		if (!view->closed) {
			view->dec.dirty	       = true;
			view->dec.border_width = cow_config->border;
		}
	}

	/* Recompute usable areas for all outputs from the new config. */
	struct cow_output *output;
	wl_list_for_each(output, &cow_wm.outputs, wm_link)
	{
		output->usable_area.x	   = output->x;
		output->usable_area.y	   = output->y;
		output->usable_area.width  = output->width;
		output->usable_area.height = output->height;
		cow_output_apply_reserved_area(output);
		output->bg_dirty = true;
	}

	cow_wm_request_manage_dirty();
	fprintf(stderr, "cow: configuration reloaded from %s\n", path);
}
