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
 *        show rules
 *        show config KEY
 *        show config.KEY
 */

#include <cJSON.h>
#include <string.h>

#include "cow/cmd.h"
#include "cow/config.h"
#include "cow/ipc.h"
#include "cow/output.h"
#include "cow/rule.h"
#include "cow/status.h"
#include "cow/titlebar.h"
#include "cow/view.h"
#include "cow/wm.h"

static enum cmd_retval cmd_show_exec(struct cmd *, struct cmd_q *);
static void	       do_show_windows(int fd);
static void	       do_show_desk(int fd);
static void	       do_show_output(int fd);
static void	       do_show_rules(int fd);
static void	       do_show_config(const char *ckey, int fd, struct cmd_q *);

struct cmd_entry cmd_show_entry = {
	"show",
	"",
	1,
	2,
	"show topic",
	cmd_show_exec,
	0,
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

	if (strcmp(topic, "rules") == 0) {
		do_show_rules(fd);
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
		cow_view_json_add_identity(w, view);
		cow_view_json_add_placement(w, view);
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
		cJSON_AddNumberToObject(w, "raise_seq",
		    (double)view->raise_seq);
		cJSON_AddNumberToObject(w, "layer", (double)view->layer);

		/* Container membership.  Emit the first member's id as
		 * the container_id (stable per-container key) and
		 * container_active (true when this view is the visible
		 * member).  Lets cowrearrange and similar consumers
		 * dedupe a container's members down to a single layout
		 * entity. */
		if (view->container_member != NULL) {
			struct cow_container *ctr =
			    view->container_member->container;
			struct cow_container_member *first =
			    wl_container_of(ctr->members.next, first, link);
			if (first->view != NULL &&
			    first->view->identifier != NULL)
				cJSON_AddStringToObject(w, "container_id",
				    first->view->identifier);
			cJSON_AddBoolToObject(w, "container_active",
			    ctr->active == view->container_member);
		}

		/* Decoration / squeeze diagnostics. */
		cJSON_AddBoolToObject(w, "use_ssd", view->use_ssd);
		cJSON_AddNumberToObject(w, "title_thickness",
		    view->dec.title_thickness);
		cJSON_AddNumberToObject(w, "border_width",
		    view->dec.border_width);
		struct cow_titlebar_squeeze sq;
		cow_titlebar_effective_squeeze(view, &sq);
		const char *mode = sq.mode == COW_SQUEEZE_NATURAL ? "natural" :
		    sq.mode == COW_SQUEEZE_FIXED		  ? "fixed" :
								    "none";
		const char *just = sq.justify == COW_JUST_CENTRE ? "centre" :
		    sq.justify == COW_JUST_RIGHT		 ? "right" :
								   "left";
		cJSON	   *sqo	 = cJSON_CreateObject();
		cJSON_AddStringToObject(sqo, "mode", mode);
		cJSON_AddStringToObject(sqo, "justify", just);
		cJSON_AddNumberToObject(sqo, "width", sq.width);
		cJSON_AddNumberToObject(sqo, "offset", sq.offset);
		cJSON_AddNumberToObject(sqo, "min_width", sq.min_width);
		cJSON_AddItemToObject(w, "squeeze", sqo);

		struct cow_box tbr  = cow_titlebar_squeezed_rect(view);
		cJSON	      *tbrj = cJSON_CreateObject();
		cow_box_json_add(tbrj, &tbr);
		cJSON_AddItemToObject(w, "squeezed_rect", tbrj);

		cJSON_AddItemToArray(arr, w);
	}
	cow_ipc_respond_data(fd, arr);
}

static void
do_show_desk(int fd)
{
	struct cow_output *out = cow_wm.current_output;

	if (out == NULL) {
		cow_ipc_respond_data(fd, cJSON_CreateNull());
		return;
	}

	cJSON *d = cJSON_CreateObject();
	cJSON_AddNumberToObject(d, "current", out->current_desk->nr);
	if (out->current_desk->name != NULL)
		cJSON_AddStringToObject(d, "current_name",
		    out->current_desk->name);
	cJSON *desks = cJSON_AddArrayToObject(d, "desks");
	struct cow_desk *dk;
	wl_list_for_each(dk, &out->desk_set.desks, set_link)
	{
		cJSON *entry = cJSON_CreateObject();
		cJSON_AddNumberToObject(entry, "nr", dk->nr);
		cJSON_AddStringToObject(entry, "name",
		    dk->name != NULL ? dk->name : "");
		cJSON_AddBoolToObject(entry, "active",
		    dk == out->current_desk);
		cJSON_AddItemToArray(desks, entry);
	}
	cow_ipc_respond_data(fd, d);
}

static void
do_show_output(int fd)
{
	cJSON		  *arr = cJSON_CreateArray();
	struct cow_output *out;

	wl_list_for_each(out, &cow_wm.outputs, wm_link)
	{
		cJSON *o = cJSON_CreateObject();
		cow_status_add_output_geometry(o, out);
		cJSON_AddItemToArray(arr, o);
	}
	cow_ipc_respond_data(fd, arr);
}

static void
do_show_rules(int fd)
{
	cJSON		     *arr  = cJSON_CreateArray();
	const struct wl_list *head = cow_rules_head();
	struct cow_rule	     *rule;

	wl_list_for_each(rule, head, link)
	{
		cJSON *r = cJSON_CreateObject();
		cJSON_AddStringToObject(r, "name",
		    rule->name ? rule->name : "");
		cJSON_AddStringToObject(r, "type",
		    cow_rule_type_name(rule->type));
		cJSON_AddBoolToObject(r, "global", rule->is_global);
		if (rule->output_name != NULL)
			cJSON_AddStringToObject(r, "output", rule->output_name);
		if (rule->desk_name != NULL)
			cJSON_AddStringToObject(r, "desk", rule->desk_name);
		else if (rule->desk_nr >= 0)
			cJSON_AddNumberToObject(r, "desk", rule->desk_nr);
		if (rule->match_spec != NULL)
			cJSON_AddStringToObject(r, "match", rule->match_spec);

		/*
		 * Render the rule body (parsed command list) back to a
		 * source-form string.  cmd_list_print uses " ; " between
		 * commands, matching what the user wrote in their config.
		 */
		if (rule->cmdlist != NULL) {
			char buf[1024];
			buf[0] = '\0';
			cmd_list_print(rule->cmdlist, buf, sizeof(buf));
			cJSON_AddStringToObject(r, "body", buf);
		} else {
			cJSON_AddStringToObject(r, "body", "");
		}

		cJSON_AddItemToArray(arr, r);
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
	else if (strcmp(ckey, "geometry_window") == 0) {
		const char *mode = "none";
		if (cow_config->geometry_window == COW_GEOMETRY_WINDOW_BOTH)
			mode = "both";
		else if (cow_config->geometry_window ==
		    COW_GEOMETRY_WINDOW_MOVE)
			mode = "move";
		else if (cow_config->geometry_window ==
		    COW_GEOMETRY_WINDOW_RESIZE)
			mode = "resize";
		val = cJSON_CreateString(mode);
	}
	else if (strcmp(ckey, "font") == 0)
		val = cJSON_CreateString(
		    cow_config->font_name ? cow_config->font_name : "");
	else if (strcmp(ckey, "preserve_desk_on_output_loss") == 0)
		val = cJSON_CreateBool(cow_config->preserve_desk_on_output_loss);
	else if (strcmp(ckey, "desks.count") == 0) {
		struct cow_output *out = cmd_ctx_target_output(cmdq);
		if (out == NULL && !wl_list_empty(&cow_wm.outputs))
			out = wl_container_of(cow_wm.outputs.next, out,
			    wm_link);
		val = cJSON_CreateNumber(out != NULL ? cow_desk_count(out) :
		    cow_config->desk_count);
	}
	else {
		cow_ipc_respond_error(fd, "show: unknown config key");
		cmdq_error(cmdq, "show: unknown config key: %s", ckey);
		return;
	}

	cow_ipc_respond_data(fd, val);
}
