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
 * Command: desk -- virtual desktop operations.
 */

#include <stdio.h>

#include "cow/compat.h"

#include "cow/cmd.h"
#include "cow/desk_ops.h"
#include "cow/ipc.h"
#include "cow/memory.h"
#include "cow/menu_choice.h"
#include "cow/output.h"
#include "cow/seat.h"
#include "cow/view.h"
#include "cow/update.h"
#include "cow/wm.h"

static enum cmd_retval cmd_desk_exec(struct cmd *, struct cmd_q *);
static enum cmd_retval choose_desk(struct cow_output *, struct cmd_q *);
static void	       do_collect(struct cow_output *, uint8_t);

#define DESK_MENU "\037cow-desk"

struct cmd_entry cmd_desk_entry = {
	"desk",
	"t:d:c:np",
	0,
	0,
	"[-t target output] [-d 0-9] [-c 0-9] [-n] [-p]",
	cmd_desk_exec,
	0,
};

static enum cmd_retval
cmd_desk_exec(struct cmd *self, struct cmd_q *cmdq)
{
	struct args	  *args	  = self->args;
	bool chooser = args_get(args, 'd') == NULL &&
	    args_get(args, 'c') == NULL && !args_has(args, 'n') &&
	    !args_has(args, 'p');
	if (chooser && cow_ipc.startup_mode)
		return (CMD_RETURN_NORMAL);

	struct cow_output *output = cmd_ctx_target_output(cmdq);
	char		  *desk_no;

	if (output == NULL) {
		cmdq_error(cmdq, "desk: no current output");
		return (CMD_RETURN_ERROR);
	}

	if (args_has(args, 'n')) {
		struct cow_desk *next = cow_desk_next(output,
		    output->current_desk);
		if (next != NULL)
			cow_desk_switch(output, next->nr);
		return (CMD_RETURN_NORMAL);
	}

	if (args_has(args, 'p')) {
		struct cow_desk *prev = cow_desk_prev(output,
		    output->current_desk);
		if (prev != NULL)
			cow_desk_switch(output, prev->nr);
		return (CMD_RETURN_NORMAL);
	}

	if ((desk_no = args_get(args, 'd')) == NULL)
		desk_no = args_get(args, 'c');
	if (desk_no == NULL)
		return (choose_desk(output, cmdq));

	long long nr;
	if (!cmd_parse_int(cmdq, "desk", "desk", desk_no, 0,
		cow_desk_count(output) - 1, &nr))
		return (CMD_RETURN_ERROR);
	if (cow_desk_by_nr(output, (uint8_t)nr) == NULL) {
		cmdq_error(cmdq, "desk: no such desk: %lld", nr);
		return (CMD_RETURN_ERROR);
	}

	if (args_has(args, 'd')) {
		cow_desk_switch(output, (uint8_t)nr);
		return (CMD_RETURN_NORMAL);
	}
	if (args_has(args, 'c')) {
		do_collect(output, (uint8_t)nr);
		return (CMD_RETURN_NORMAL);
	}

	cmdq_error(cmdq, "desk: unknown command: %s", args->argv[0]);
	return (CMD_RETURN_ERROR);
}

static enum cmd_retval
choose_desk(struct cow_output *output, struct cmd_q *cmdq)
{
	size_t count = cow_desk_count(output);
	struct cow_menu_choice *choices = xcalloc(count, sizeof(*choices));
	struct cow_desk *desk;
	size_t i = 0;
	wl_list_for_each(desk, &output->desk_set.desks, set_link)
	{
		char *label;
		xasprintf(&label, "%u: %s%s", desk->nr, desk->name,
		    desk == output->current_desk ? " (active)" : "");
		char nr[4];
		snprintf(nr, sizeof(nr), "%u", desk->nr);
		const char *argv[] = { "desk", "-t", output->name, "-d", nr };
		if (!cow_menu_choice_init(&choices[i], label, 5, argv)) {
			xfree(label);
			cow_menu_choices_free(choices, count);
			cmdq_error(cmdq, "desk: cannot build desk menu");
			return CMD_RETURN_ERROR;
		}
		i++;
		xfree(label);
	}

	char *title;
	xasprintf(&title, "Desk on %s (type to search)", output->name);
	enum cow_menu_choice_result result = cow_menu_choose(DESK_MENU, title,
	    choices, count, output, NULL);
	xfree(title);
	enum cmd_retval retval = CMD_RETURN_NORMAL;
	if (result == COW_MENU_CHOICE_EMPTY) {
		cmdq_error(cmdq, "desk: no desks available");
		retval = CMD_RETURN_ERROR;
	} else if (result == COW_MENU_CHOICE_SINGLE) {
		cow_desk_switch(output, cow_desk_first(output)->nr);
	} else if (result == COW_MENU_CHOICE_ERROR) {
		cmdq_error(cmdq, "desk: cannot create desk menu");
		retval = CMD_RETURN_ERROR;
	}
	cow_menu_choices_free(choices, count);
	return (retval);
}

void
do_collect(struct cow_output *output, uint8_t nr)
{
	/* Can't collect the current desk */
	if (nr == (int)output->current_desk->nr)
		return;

	bool was_collected = (output->collected_desks & (1u << nr)) != 0;

	/* Toggle the collected bit for this desk */
	output->collected_desks ^= (1u << nr);

	bool now_collected = (output->collected_desks & (1u << nr)) != 0;

	/* Save/restore positions for views on the toggled desk */
	struct cow_view *v;
	wl_list_for_each(v, &cow_wm.views, wm_link)
	{
		if (v->output != output || v->closed)
			continue;
		if (v->desk != cow_desk_by_nr(output, nr))
			continue;

		if (!was_collected && now_collected) {
			/* Collecting: save full geometry */
			v->collected_geo_saved	  = true;
			v->collected_saved_x	  = v->x;
			v->collected_saved_y	  = v->y;
			v->collected_saved_width  = v->content_width;
			v->collected_saved_height = v->content_height;
		} else if (was_collected && !now_collected) {
			/* Uncollecting: restore saved geometry */
			if (v->collected_geo_saved) {
				v->x = v->collected_saved_x;
				v->y = v->collected_saved_y;
				cow_view_set_pending_geometry(v,
				    v->collected_saved_x, v->collected_saved_y,
				    v->collected_saved_width,
				    v->collected_saved_height);
				v->collected_geo_saved = false;
			}
		}
	}

	/* Mark views on this output dirty for repaint. */
	wl_list_for_each(v, &cow_wm.views, wm_link)
	{
		if (v->output == output && !v->closed)
			v->dec.dirty = true;
	}

	cow_update_request(COW_UPDATE_MANAGE | COW_UPDATE_STATUS);
}
