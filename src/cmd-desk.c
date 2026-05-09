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

#include "cow/compat.h"

#include "cow/cmd.h"
#include "cow/output.h"
#include "cow/seat.h"
#include "cow/desk_ops.h"
#include "cow/status.h"
#include "cow/view.h"
#include "cow/wm.h"

static enum cmd_retval cmd_desk_exec(struct cmd *, struct cmd_q *);
static void	       do_collect(struct cow_output *, uint8_t);

struct cmd_entry cmd_desk_entry = {
	"desk",
	"t:d:c:np",
	0,
	0,
	"[-t target output] [-d 0-9] [-c 0-9] [-n] [-p]",
	cmd_desk_exec,
};

static enum cmd_retval
cmd_desk_exec(struct cmd *self, struct cmd_q *cmdq)
{
	struct args	  *args	  = self->args;
	struct cow_output *output = cmd_ctx_target_output(cmdq);
	char		  *desk_no;

	if (output == NULL) {
		cmdq_error(cmdq, "desk: no current output");
		return (CMD_RETURN_ERROR);
	}

	if (args_has(args, 'n')) {
		uint8_t nr = (output->current_desk->nr + 1) % COW_NR_OF_DESKS;
		cow_desk_switch(output, nr);

		return (CMD_RETURN_NORMAL);
	}

	if (args_has(args, 'p')) {
		uint8_t nr = output->current_desk->nr;
		nr	   = (nr == 0) ? COW_NR_OF_DESKS - 1 : nr - 1;
		cow_desk_switch(output, nr);

		return (CMD_RETURN_NORMAL);
	}

	if ((desk_no = args_get(args, 'd')) == NULL)
		desk_no = args_get(args, 'c');
	if (desk_no == NULL)
		return (CMD_RETURN_NORMAL);

	/* Now parse the desk -- it's either going to be for collect or desk
	 * switching.
	 */
	long long nr;
	if (!cmd_parse_int(cmdq, "desk", "desk", desk_no, 0,
		COW_NR_OF_DESKS - 1, &nr))
		return (CMD_RETURN_ERROR);

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
		if (v->desk != &output->desks[nr])
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

	/* Re-resolve styles for all views on this output */
	wl_list_for_each(v, &cow_wm.views, wm_link)
	{
		if (v->output == output && !v->closed) {
			cow_view_resolve_style(v);
			v->dec.dirty = true;
		}
	}

	cow_status_emit();
	cow_wm_request_manage_dirty();
}
