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
#include "cow/log.h"
#include "cow/config.h"
#include "cow/ipc.h"
#include "cow/keybinding.h"
#include "cow/menu.h"
#include "cow/output.h"
#include "cow/rule.h"
#include "cow/style.h"
#include "cow/seat.h"
#include "cow/view.h"
#include "cow/update.h"
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
	0,
};

static enum cmd_retval
cmd_quit_exec(struct cmd *self, struct cmd_q *cmdq)
{
	struct args *args = self->args;

	if (args_has(args, 'r')) {
		do_reload(cmdq);
		return (CMD_RETURN_NORMAL);
	}

	cow_log_info("exiting session");
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

	/*
	 * Drop the previous load's global objects before replaying the
	 * config file.  Menu definitions live on cow_wm rather than inside
	 * cow_config, so they need explicit reset here too.
	 */
	cow_rule_fini();
	cow_style_fini();
	cow_decor_reset_profiles();
	cow_menu_close_all();
	cow_menu_destroy_all();
	wl_list_init(&cow_wm.menus);

	if (!cow_ipc_load_config(path)) {
		cmdq_error(cmdq, "quit -r: config load failed");
		return;
	}
	cow_wm_cursor_hide_configure();

	struct cow_seat *seat;
	wl_list_for_each(seat, &cow_wm.seats, wm_link)
	{
		cow_seat_destroy_bindings(seat);
		cow_seat_register_default_bindings(seat);
	}

	cow_decoration_register_default_bindings();

	/* Re-stamp every view's cfg snapshot from the freshly parsed
	 * cow_config so existing views pick up the new defaults; then
	 * mark their decoration dirty for re-render. */
	struct cow_view *view;
	wl_list_for_each(view, &cow_wm.views, wm_link)
	{
		if (!view->closed) {
			cow_view_cfg_apply_global(&view->cfg);
			view->policy_overrides = 0;
			view->dec.dirty	       = true;
			view->dec.border_width = view->cfg.border;
		}
	}

	/* Recompute usable areas for all outputs from the new config. */
	struct cow_output *output;
	wl_list_for_each(output, &cow_wm.outputs, wm_link)
	{
		(void)cow_output_recompute_usable_area(output);
		output->bg_dirty = true;
	}

	/* Apply rules here which could affect styling.
	 *
	 * Run on-map per view first, then on-desk per
	 * output's current desk.  The rule subsystem fans on-desk
	 * out across each view on the matching desk internally.
	 */
	wl_list_for_each(view, &cow_wm.views, wm_link)
	{
		if (view->closed)
			continue;
		cow_rule_fire(COW_RULE_ON_MAP, view, view->desk, view->output);
	}
	wl_list_for_each(output, &cow_wm.outputs, wm_link)
	{
		if (output->current_desk != NULL)
			cow_rule_fire(COW_RULE_ON_DESK, NULL,
			    output->current_desk, output);
	}

	cow_update_request(COW_UPDATE_MANAGE);
	cow_log_info("configuration reloaded from %s", path);
}
