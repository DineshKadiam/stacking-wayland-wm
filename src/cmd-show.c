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
 * Command: show -- query window manager state.
 *
 * usage: show windows
 *        show desk
 *        show output
 *        show config KEY
 *        show config.KEY
 */

#include <cJSON.h>
#include <string.h>

#include "cow/cmd.h"
#include "cow/config.h"
#include "cow/ipc.h"
#include "cow/output.h"
#include "cow/view.h"
#include "cow/wm.h"

static enum cmd_retval cmd_show_exec(struct cmd *, struct cmd_q *);
static void	       do_show_windows(int fd);
static void	       do_show_desk(int fd);
static void	       do_show_output(int fd);
static void	       do_show_config(const char *ckey, int fd, struct cmd_q *);

struct cmd_entry cmd_show_entry = {
	"show", "",
	1, 2,
	"show topic",
	cmd_show_exec
};

static enum cmd_retval
cmd_show_exec(struct cmd *self, struct cmd_q *cmdq)
{
	struct args *args  = self->args;
	int	     fd	   = cmdq->client_fd;
	const char  *topic = args->argv[0];

	if (strcmp(topic, "windows") == 0) {
		do_show_windows(fd);
		return (CMD_RETURN_NORMAL);
	}

	if (strcmp(topic, "desk") == 0) {
		do_show_desk(fd);
		return (CMD_RETURN_NORMAL);
	}

	if (strcmp(topic, "output") == 0) {
		do_show_output(fd);
		return (CMD_RETURN_NORMAL);
	}

	const char *ckey = NULL;
	if (strncmp(topic, "config.", 7) == 0)
		ckey = topic + 7;
	else if (strcmp(topic, "config") == 0 && args->argc >= 2)
		ckey = args->argv[1];

	if (ckey != NULL) {
		do_show_config(ckey, fd, cmdq);
		return (CMD_RETURN_NORMAL);
	}

	cow_ipc_respond_error(fd, "show: unknown topic");
	cmdq_error(cmdq, "show: unknown topic: %s", topic);
	return (CMD_RETURN_ERROR);
}

static void
do_show_windows(int fd)
{
	cJSON		*arr = cJSON_CreateArray();
	struct cow_view *view;

	wl_list_for_each(view, &cow_wm.views, wm_link)
	{
		if (view->closed)
			continue;
		cJSON *w = cJSON_CreateObject();
		cJSON_AddStringToObject(w, "id",
		    view->identifier ? view->identifier : "");
		cJSON_AddStringToObject(w, "app_id",
		    view->app_id ? view->app_id : "");
		cJSON_AddStringToObject(w, "title",
		    view->title ? view->title : "");
		cJSON_AddNumberToObject(w, "desk",
		    view->desk ? view->desk->nr : -1);
		cJSON_AddNumberToObject(w, "x", view->x);
		cJSON_AddNumberToObject(w, "y", view->y);
		cJSON_AddNumberToObject(w, "vx", view->vx);
		cJSON_AddNumberToObject(w, "vy", view->vy);
		cJSON_AddNumberToObject(w, "width", view->content_width);
		cJSON_AddNumberToObject(w, "height", view->content_height);
		cJSON_AddBoolToObject(w, "focused",
		    view == cow_wm.focused_view);
		cJSON_AddBoolToObject(w, "maximized", view->maximized);
		cJSON_AddBoolToObject(w, "shaded", view->sd != COW_SHADE_NONE);
		cJSON_AddBoolToObject(w, "iconified",
		    cow_view_is_iconified(view));
		cJSON_AddItemToArray(arr, w);
	}
	cow_ipc_respond_data(fd, arr);
}

static void
do_show_desk(int fd)
{
	struct cow_output *out = cow_wm.current_output;

	if (out != NULL) {
		cJSON *d = cJSON_CreateObject();
		cJSON_AddNumberToObject(d, "current", out->current_desk->nr);
		cow_ipc_respond_data(fd, d);
	} else {
		cow_ipc_respond_data(fd, cJSON_CreateNull());
	}
}

static void
do_show_output(int fd)
{
	cJSON		  *arr = cJSON_CreateArray();
	struct cow_output *out;

	wl_list_for_each(out, &cow_wm.outputs, wm_link)
	{
		cJSON *o = cJSON_CreateObject();
		cJSON_AddStringToObject(o, "name", out->name ? out->name : "");
		cJSON_AddNumberToObject(o, "x", out->x);
		cJSON_AddNumberToObject(o, "y", out->y);
		cJSON_AddNumberToObject(o, "width", out->width);
		cJSON_AddNumberToObject(o, "height", out->height);
		cJSON *ua = cJSON_CreateObject();
		cJSON_AddNumberToObject(ua, "x", out->usable_area.x);
		cJSON_AddNumberToObject(ua, "y", out->usable_area.y);
		cJSON_AddNumberToObject(ua, "width", out->usable_area.width);
		cJSON_AddNumberToObject(ua, "height", out->usable_area.height);
		cJSON_AddItemToObject(o, "usable", ua);
		cJSON_AddItemToArray(arr, o);
	}
	cow_ipc_respond_data(fd, arr);
}

static void
do_show_config(const char *ckey, int fd, struct cmd_q *cmdq)
{
	cJSON *val = NULL;

	if (strcmp(ckey, "border") == 0)
		val = cJSON_CreateNumber(cow_config->border);
	else if (strcmp(ckey, "step") == 0)
		val = cJSON_CreateNumber(cow_config->step);
	else if (strcmp(ckey, "corner_length") == 0)
		val = cJSON_CreateNumber(cow_config->corner_length);
	else if (strcmp(ckey, "border_style") == 0)
		val = cJSON_CreateString(
		    cow_config->border_style == COW_BORDER_STYLE_FVWM ? "fvwm" :
			cow_config->border_style == COW_BORDER_STYLE_MWM ?
									"mwm" :
									"none");
	else if (strcmp(ckey, "focus") == 0)
		val = cJSON_CreateString(
		    cow_config->focus_mode == COW_FOCUS_MODE_SLOPPY ? "sloppy" :
								      "click");
	else {
		cow_ipc_respond_error(fd, "show: unknown config key");
		cmdq_error(cmdq, "show: unknown config key: %s", ckey);
		return;
	}

	cow_ipc_respond_data(fd, val);
}
