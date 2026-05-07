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
 * Shared argument-parsing and target-resolution helpers.  Existing
 * commands open-coded the same strtonum / direction / target-or-error
 * patterns over and over; these functions consolidate the boilerplate.
 */

#include <stdlib.h>
#include <string.h>

#include "cow/cmd.h"
#include "cow/desk_ops.h"
#include "cow/ipc.h"
#include "cow/output.h"
#include "cow/view.h"
#include "cow/wm.h"

#include <cJSON.h>

struct cow_view *
cmd_require_source_window(struct cmd_q *cmdq, const char *cmd_name)
{
	struct cow_view *view = cmdq->source.window;
	if (view == NULL || view->closed) {
		cmdq_error(cmdq, "%s: no source window", cmd_name);
		return NULL;
	}
	return view;
}

struct cow_output *
cmd_require_target_output(struct cmd_q *cmdq, const char *cmd_name)
{
	struct cow_output *out = cmd_ctx_target_output(cmdq);
	if (out == NULL) {
		cmdq_error(cmdq, "%s: no target output", cmd_name);
		return NULL;
	}
	return out;
}

struct cow_desk *
cmd_require_target_desk(struct cmd_q *cmdq, const char *cmd_name)
{
	struct cow_desk *d = cmd_ctx_target_desk(cmdq);
	if (d == NULL) {
		cmdq_error(cmdq, "%s: no target desk", cmd_name);
		return NULL;
	}
	return d;
}

bool
cmd_parse_int(struct cmd_q *cmdq, const char *cmd_name, const char *field,
    const char *val, long long lo, long long hi, long long *out)
{
	if (val == NULL) {
		cmdq_error(cmdq, "%s: %s: missing value", cmd_name, field);
		return false;
	}
	const char *errstr = NULL;
	long long   v	   = strtonum(val, lo, hi, &errstr);
	if (errstr != NULL) {
		cmdq_error(cmdq, "%s: %s: bad value: %s", cmd_name, field, val);
		return false;
	}
	*out = v;
	return true;
}

bool
cmd_args_int(struct cmd_q *cmdq, const char *cmd_name, struct args *args,
    u_char flag, long long lo, long long hi, long long *out)
{
	if (!args_has(args, flag))
		return false;
	const char *val = args_get(args, flag);
	return cmd_parse_int(cmdq, cmd_name, (const char[]) { '-', flag, '\0' },
	    val, lo, hi, out);
}

enum cow_direction
cmd_args_direction(struct cmd_q *cmdq, const char *cmd_name, struct args *args,
    u_char flag, bool required)
{
	if (!args_has(args, flag)) {
		if (required) {
			cmdq_error(cmdq, "%s: missing direction (-%c)",
			    cmd_name, flag);
		}
		return COW_DIR_NONE;
	}
	const char	  *val = args_get(args, flag);
	enum cow_direction d   = cow_parse_direction(val);
	if (d == COW_DIR_NONE) {
		cmdq_error(cmdq, "%s: unknown direction: %s", cmd_name,
		    val ? val : "(null)");
	}
	return d;
}

void
cmd_view_dirty(struct cow_view *view)
{
	if (view == NULL)
		return;
	view->dec.dirty		  = true;
	cow_wm.needs_manage_dirty = true;
}

void
cmdq_respond_data(struct cmd_q *cmdq, struct cJSON *data)
{
	if (cmdq == NULL || cmdq->client_fd < 0) {
		if (data != NULL)
			cJSON_Delete(data);
		return;
	}
	cow_ipc_respond_data(cmdq->client_fd, data);
}

void
cmdq_respond_error(struct cmd_q *cmdq, const char *msg)
{
	if (cmdq == NULL || cmdq->client_fd < 0)
		return;
	cow_ipc_respond_error(cmdq->client_fd, msg);
}
