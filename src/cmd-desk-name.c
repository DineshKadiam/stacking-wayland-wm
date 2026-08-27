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

#include <stdlib.h>
#include <string.h>

#include <cJSON.h>

#include "cow/cmd.h"
#include "cow/compat.h"
#include "cow/ipc.h"
#include "cow/memory.h"
#include "cow/output.h"
#include "cow/status.h"
#include "cow/wm.h"

static enum cmd_retval cmd_desk_name_exec(struct cmd *, struct cmd_q *);

struct cmd_entry cmd_desk_name_entry = {
	"desk-name",
	"t:dlr",
	0,
	-1,
	"[-t output] N NAME | -d N | -l | -r",
	cmd_desk_name_exec,
	0,
};

static void
do_list(struct cmd_q *cmdq)
{
	cJSON		  *arr = cJSON_CreateArray();
	struct cow_output *out;
	wl_list_for_each(out, &cow_wm.outputs, wm_link)
	{
		cJSON *o = cJSON_CreateObject();
		cJSON_AddStringToObject(o, "output",
		    out->name != NULL ? out->name : "");
		cJSON_AddBoolToObject(o, "user_named",
		    out->desk_set.user_named);
		cJSON *ds = cJSON_AddArrayToObject(o, "desks");
		struct cow_desk *d;
		wl_list_for_each(d, &out->desk_set.desks, set_link)
		{
			cJSON *e = cJSON_CreateObject();
			cJSON_AddNumberToObject(e, "nr", d->nr);
			cJSON_AddStringToObject(e, "name",
			    d->name != NULL ? d->name : "");
			cJSON_AddBoolToObject(e, "current",
			    d == out->current_desk);
			cJSON_AddItemToArray(ds, e);
		}
		cJSON_AddItemToArray(arr, o);
	}
	cmdq_respond_data(cmdq, arr);
}

static enum cmd_retval
cmd_desk_name_exec(struct cmd *self, struct cmd_q *cmdq)
{
	struct args *args = self->args;

	if (args_has(args, 'l')) {
		do_list(cmdq);
		return (CMD_RETURN_NORMAL);
	}

	struct cow_output *output = cmd_ctx_target_output(cmdq);
	if (output == NULL) {
		cmdq_error(cmdq, "desk-name: no current output");
		return (CMD_RETURN_ERROR);
	}

	if (args_has(args, 'r')) {
		cow_desk_set_reset(output);
		cow_wm_request_manage_dirty();
		cow_status_request();
		return (CMD_RETURN_NORMAL);
	}

	if (args_has(args, 'd')) {
		if (args->argc < 1) {
			cmdq_error(cmdq, "desk-name: -d needs N");
			return (CMD_RETURN_ERROR);
		}
		long long nr;
		if (!cmd_parse_int(cmdq, "desk-name", "N", args->argv[0],
		    0, UINT8_MAX, &nr))
			return (CMD_RETURN_ERROR);
		char *cause = NULL;
		if (!cow_desk_set_delete(output, (uint8_t)nr, &cause)) {
			cmdq_error(cmdq, "%s",
			    cause != NULL ? cause : "desk-name: delete failed");
			xfree(cause);
			return (CMD_RETURN_ERROR);
		}
		xfree(cause);
		cow_wm_request_manage_dirty();
		cow_status_request();
		return (CMD_RETURN_NORMAL);
	}

	if (args->argc < 2) {
		cmdq_error(cmdq, "desk-name: usage: N NAME");
		return (CMD_RETURN_ERROR);
	}
	long long nr;
	if (!cmd_parse_int(cmdq, "desk-name", "N", args->argv[0], 0,
	    UINT8_MAX, &nr))
		return (CMD_RETURN_ERROR);
	char *cause = NULL;
	if (!cow_desk_set_name(output, (uint8_t)nr, args->argv[1], &cause)) {
		cmdq_error(cmdq, "%s",
		    cause != NULL ? cause : "desk-name: failed");
		xfree(cause);
		return (CMD_RETURN_ERROR);
	}
	xfree(cause);
	cow_wm_request_manage_dirty();
	cow_status_request();
	return (CMD_RETURN_NORMAL);
}
