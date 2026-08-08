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
#include "cow/cmd-filter.h"
#include "cow/compat.h"
#include "cow/output.h"
#include "cow/view.h"
#include "cow/wm.h"

static enum cmd_retval cmd_icon_reset_exec(struct cmd *, struct cmd_q *);
static bool	      icon_reset_view_filtered(struct cow_view *,
    struct cmd_filter *, bool *);
static bool	      icon_target_is_window(const char *, struct cmd_q *);
static bool	      icon_target_is_output(const char *, struct cmd_q *);

struct cmd_entry cmd_icon_reset_entry = {
	"icon-reset",
	"F:t:",
	0,
	0,
	"icon-reset [-F expr] [-t target]",
	cmd_icon_reset_exec,
	0,
};

static void
icon_reset_view(struct cow_view *view)
{
	if (view == NULL || view->closed)
		return;
	view->icon_manual_position = false;
	view->icon_auto_position_valid = false;
}

static bool
icon_reset_view_filtered(struct cow_view *view, struct cmd_filter *filter,
    bool *changed)
{
	if (view == NULL || view->closed)
		return true;

	bool matches = false;
	if (!cmd_filter_view(filter, view, &matches))
		return false;
	if (!matches)
		return true;

	icon_reset_view(view);
	if (changed != NULL)
		*changed = true;
	return true;
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

static bool
icon_reset_output(struct cow_output *output, struct cmd_filter *filter,
    bool *changed)
{
	struct cow_view *view;

	wl_list_for_each(view, &cow_wm.views, wm_link)
	{
		if (view->output == output &&
		    !icon_reset_view_filtered(view, filter, changed))
			return false;
	}
	cow_view_layout_icons_on_output(output);
	return true;
}

static bool
icon_reset_desk(struct cow_desk *desk, struct cmd_filter *filter,
    bool *changed)
{
	struct cow_view *view;

	wl_list_for_each(view, &cow_wm.views, wm_link)
	{
		if (view->desk == desk &&
		    !icon_reset_view_filtered(view, filter, changed))
			return false;
	}
	if (desk->output != NULL)
		cow_view_layout_icons_on_output(desk->output);
	return true;
}

static enum cmd_retval
cmd_icon_reset_exec(struct cmd *self, struct cmd_q *cmdq)
{
	const char *target = args_get(self->args, 't');
	const char *filter_val = args_get(self->args, 'F');
	struct cmd_filter filter;
	bool changed = false;

	if (!cmd_filter_parse(&filter, cmdq, "icon-reset", "-F",
		filter_val))
		return (CMD_RETURN_ERROR);

	if (target != NULL && icon_target_is_window(target, cmdq) &&
	    cmdq->target.window != NULL) {
		struct cow_output *output = cmdq->target.window->output;

		if (!icon_reset_view_filtered(cmdq->target.window, &filter,
			&changed) ||
		    !cmd_filter_finish(&filter, cmdq)) {
			cmd_filter_free(&filter);
			return (CMD_RETURN_ERROR);
		}
		cmd_filter_free(&filter);
		if (changed && output != NULL)
			cow_view_layout_icons_on_output(output);
		if (changed)
			cow_wm_request_manage_dirty();
		return (CMD_RETURN_NORMAL);
	}
	if (target != NULL && cmdq->target.desk != NULL) {
		bool ok = icon_target_is_output(target, cmdq) ?
		    icon_reset_output(cmdq->target.output, &filter, &changed) :
		    icon_reset_desk(cmdq->target.desk, &filter, &changed);
		if (!ok || !cmd_filter_finish(&filter, cmdq)) {
			cmd_filter_free(&filter);
			return (CMD_RETURN_ERROR);
		}
		cmd_filter_free(&filter);
		if (changed)
			cow_wm_request_manage_dirty();
		return (CMD_RETURN_NORMAL);
	}

	struct cow_view *view;
	wl_list_for_each(view, &cow_wm.views, wm_link)
	{
		if (!icon_reset_view_filtered(view, &filter, &changed)) {
			cmd_filter_finish(&filter, cmdq);
			cmd_filter_free(&filter);
			return (CMD_RETURN_ERROR);
		}
	}
	if (!cmd_filter_finish(&filter, cmdq)) {
		cmd_filter_free(&filter);
		return (CMD_RETURN_ERROR);
	}
	cmd_filter_free(&filter);

	if (changed) {
		struct cow_output *output;
		wl_list_for_each(output, &cow_wm.outputs, wm_link)
			cow_view_layout_icons_on_output(output);
		cow_wm_request_manage_dirty();
	}
	return (CMD_RETURN_NORMAL);
}
