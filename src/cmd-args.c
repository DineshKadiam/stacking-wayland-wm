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

#include <errno.h>
#include <limits.h>
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
cmd_parse_page_axis(struct cmd_q *cmdq, const char *cmd_name,
    const char *field, const char *value, int current, int count,
    long long *out)
{
	size_t	  len = strlen(value);
	bool	  relative = len > 1 && value[len - 1] == 'p' &&
	    (value[0] == '+' || value[0] == '-');
	char	  buf[64];
	long long v, page;

	if (relative) {
		if (len >= sizeof(buf)) {
			cmdq_error(cmdq, "%s: %s value too long", cmd_name,
			    field);
			return false;
		}
		memcpy(buf, value, len - 1);
		buf[len - 1] = '\0';
		if (!cmd_parse_int(cmdq, cmd_name, field, buf, INT_MIN,
			INT_MAX, &v))
			return false;
		page = current + v;
	} else {
		if (!cmd_parse_int(cmdq, cmd_name, field, value, -count,
			count - 1, &v))
			return false;
		page = v < 0 ? count + v : v;
	}
	if (page < 0 || page >= count) {
		cmdq_error(cmdq, "%s: %s out of range: %lld", cmd_name,
		    field, page);
		return false;
	}
	*out = page;
	return true;
}

bool
cmd_parse_page_spec(struct cmd_q *cmdq, const char *cmd_name,
    const char *spec, int current_col, int current_row, int cols, int rows,
    long long *col, long long *row)
{
	const char *comma = strchr(spec, ',');
	if (comma == NULL) {
		cmdq_error(cmdq, "%s: expected col,row (got '%s')", cmd_name,
		    spec);
		return false;
	}

	size_t col_len = (size_t)(comma - spec);
	if (col_len >= 64 || strlen(comma + 1) >= 64) {
		cmdq_error(cmdq, "%s: page value too long", cmd_name);
		return false;
	}
	char col_buf[64];
	memcpy(col_buf, spec, col_len);
	col_buf[col_len] = '\0';

	if (!cmd_parse_page_axis(cmdq, cmd_name, "col", col_buf,
		current_col, cols, col) ||
	    !cmd_parse_page_axis(cmdq, cmd_name, "row", comma + 1,
		current_row, rows, row))
		return false;
	return true;
}

bool
cmd_parse_length(struct cmd_q *cmdq, const char *cmd_name, const char *field,
    const char *val, long long lo, long long hi, struct cmd_length *out)
{
	if (val == NULL) {
		cmdq_error(cmdq, "%s: %s: missing value", cmd_name, field);
		return false;
	}

	char  *end;
	errno = 0;
	double value = strtod(val, &end);
	enum cmd_length_unit unit;

	if (end != val && strcmp(end, "vw") == 0)
		unit = CMD_LENGTH_VW;
	else if (end != val && strcmp(end, "vh") == 0)
		unit = CMD_LENGTH_VH;
	else {
		long long pixels;
		if (!cmd_parse_int(cmdq, cmd_name, field, val, lo, hi,
			&pixels))
			return false;
		out->value = (double)pixels;
		out->unit  = CMD_LENGTH_PIXELS;
		return true;
	}

	if (errno == ERANGE || value != value || value < (double)lo ||
	    value > (double)hi) {
		cmdq_error(cmdq, "%s: %s: bad value: %s", cmd_name, field,
		    val);
		return false;
	}

	out->value = value;
	out->unit  = unit;
	return true;
}

bool
cmd_length_resolve(struct cmd_q *cmdq, const char *cmd_name,
    const char *field, const struct cmd_length *length,
    const struct cow_output *output, int *result)
{
	if (length->unit == CMD_LENGTH_PIXELS) {
		*result = (int)length->value;
		return true;
	}
	if (output == NULL) {
		cmdq_error(cmdq, "%s: %s: window has no output", cmd_name,
		    field);
		return false;
	}

	int base = length->unit == CMD_LENGTH_VW ? output->usable_area.width :
						      output->usable_area.height;
	long double scaled = (long double)length->value * base / 100.0L;
	if (scaled < INT_MIN || scaled > INT_MAX) {
		cmdq_error(cmdq, "%s: %s: resolved value is out of range",
		    cmd_name, field);
		return false;
	}

	/* Round to the nearest physical pixel. */
	*result = (int)(scaled < 0 ? scaled - 0.5L : scaled + 0.5L);
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
	view->dec.dirty = true;
	cow_wm_request_manage_dirty();
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
