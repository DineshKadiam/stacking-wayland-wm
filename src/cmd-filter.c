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

#include <string.h>

#include "cow/cmd.h"
#include "cow/cmd-filter.h"
#include "cow/dsl.h"
#include "cow/memory.h"

static void
cmd_filter_error_free(struct cmd_filter *filter)
{
	if (filter->err == NULL)
		return;
	xfree(filter->err->file);
	xfree(filter->err->message);
	xfree(filter->err);
	filter->err = NULL;
}

bool
cmd_filter_parse(struct cmd_filter *filter, struct cmd_q *cmdq,
    const char *command, const char *flag, const char *expr)
{
	memset(filter, 0, sizeof(*filter));
	filter->command = command;
	filter->flag = flag;
	if (expr == NULL)
		return true;

	filter->err = xcalloc(1, sizeof(*filter->err));
	filter->ast = dsl_parse(expr, NULL, filter->err);
	if (filter->ast != NULL) {
		cmd_filter_error_free(filter);
		return true;
	}

	cmdq_error(cmdq, "%s: %s parse error at %d:%d: %s", command, flag,
	    filter->err->line, filter->err->col,
	    filter->err->message != NULL ? filter->err->message : "(unknown)");
	cmd_filter_error_free(filter);
	return false;
}

bool
cmd_filter_empty(const struct cmd_filter *filter)
{
	return filter == NULL || filter->ast == NULL;
}

bool
cmd_filter_view(struct cmd_filter *filter, const struct cow_view *view,
    bool *matches)
{
	if (matches == NULL)
		return false;
	*matches = true;
	if (cmd_filter_empty(filter))
		return true;

	cmd_filter_error_free(filter);
	filter->err = xcalloc(1, sizeof(*filter->err));
	if (!dsl_eval_view_expr(filter->ast, view, matches, filter->err)) {
		filter->failed = true;
		return false;
	}

	cmd_filter_error_free(filter);
	return true;
}

bool
cmd_filter_finish(struct cmd_filter *filter, struct cmd_q *cmdq)
{
	if (filter == NULL || !filter->failed)
		return true;

	cmdq_error(cmdq, "%s: %s eval error at %d:%d: %s", filter->command,
	    filter->flag, filter->err != NULL ? filter->err->line : 0,
	    filter->err != NULL ? filter->err->col : 0,
	    filter->err != NULL && filter->err->message != NULL ?
	    filter->err->message : "(unknown)");
	filter->failed = false;
	cmd_filter_error_free(filter);
	return false;
}

void
cmd_filter_free(struct cmd_filter *filter)
{
	if (filter == NULL)
		return;
	dsl_free(filter->ast);
	filter->ast = NULL;
	filter->failed = false;
	cmd_filter_error_free(filter);
}
