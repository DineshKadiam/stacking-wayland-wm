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

#include <limits.h>
#include <string.h>

#include <wayland-client.h>

#include "cow/cmd.h"
#include "cow/compat.h"
#include "cow/output.h"
#include "cow/view.h"
#include "cow/wm.h"

static enum cmd_retval cmd_icon_reset_exec(struct cmd *, struct cmd_q *);
static bool	      icon_target_is_window(const char *, struct cmd_q *);
static bool	      icon_target_is_output(const char *, struct cmd_q *);

struct cmd_entry cmd_icon_reset_entry = {
	"icon-reset",
	"t:",
	0,
	0,
	"icon-reset [-t target]",
	cmd_icon_reset_exec,
	0,
};

static void
icon_reset_view(struct cow_view *view)
{
	if (view == NULL || view->closed)
		return;
	view->icon_manual_position = false;
}

static bool
icon_target_is_window(const char *target, struct cmd_q *cmdq)
{
	if (target == NULL)
		return false;
	if (strchr(target, '.') != NULL)
		return true;
	if (strchr(target, ':') != NULL)
		return false;
	return !icon_target_is_output(target, cmdq);
}

static bool
icon_target_is_output(const char *target, struct cmd_q *cmdq)
{
	struct cow_output *output = cmdq->target.output;

	if (target == NULL || output == NULL)
		return false;
	if (strchr(target, ':') != NULL || strchr(target, '.') != NULL)
		return false;
	if (output->name != NULL && strcmp(target, output->name) == 0)
		return true;
	if (target[0] == '@' && target[1] >= '0' && target[1] <= '9') {
		const char *errstr;
		long long   nr = strtonum(target + 1, 1, INT_MAX, &errstr);

		return (errstr == NULL && nr == output->number);
	}
	return false;
}

static void
icon_reset_output(struct cow_output *output)
{
	struct cow_view *view;

	wl_list_for_each(view, &cow_wm.views, wm_link)
	{
		if (view->output == output)
			icon_reset_view(view);
	}
	cow_view_layout_icons_on_output(output);
}

static void
icon_reset_desk(struct cow_desk *desk)
{
	struct cow_view *view;

	wl_list_for_each(view, &cow_wm.views, wm_link)
	{
		if (view->desk == desk)
			icon_reset_view(view);
	}
	if (desk->output != NULL)
		cow_view_layout_icons_on_output(desk->output);
}

static enum cmd_retval
cmd_icon_reset_exec(struct cmd *self, struct cmd_q *cmdq)
{
	const char *target = args_get(self->args, 't');

	if (target != NULL && icon_target_is_window(target, cmdq) &&
	    cmdq->target.window != NULL) {
		struct cow_output *output = cmdq->target.window->output;

		icon_reset_view(cmdq->target.window);
		if (output != NULL)
			cow_view_layout_icons_on_output(output);
		cow_wm_request_manage_dirty();
		return (CMD_RETURN_NORMAL);
	}
	if (target != NULL && cmdq->target.desk != NULL) {
		if (icon_target_is_output(target, cmdq))
			icon_reset_output(cmdq->target.output);
		else
			icon_reset_desk(cmdq->target.desk);
		cow_wm_request_manage_dirty();
		return (CMD_RETURN_NORMAL);
	}

	struct cow_view *view;
	wl_list_for_each(view, &cow_wm.views, wm_link)
		icon_reset_view(view);

	struct cow_output *output;
	wl_list_for_each(output, &cow_wm.outputs, wm_link)
		cow_view_layout_icons_on_output(output);

	cow_wm_request_manage_dirty();
	return (CMD_RETURN_NORMAL);
}
