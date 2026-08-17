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
 * Command: script -- evaluate a CoW DSL block.
 *
 *   script { ... }
 *
 * The brace-delimited body is captured by cmd-string's brace
 * continuation as a single argv element (with the outer braces
 * included).  We parse it on each invocation and evaluate against
 * the calling cmd_q; the body may emit any number of cow commands
 * which are appended to the queue and drain in due course.
 */

#include <stdlib.h>
#include <string.h>

#include "cow/cmd.h"
#include "cow/dsl.h"
#include "cow/memory.h"

static enum cmd_retval cmd_script_exec(struct cmd *, struct cmd_q *);

struct cmd_entry cmd_script_entry = {
	"script",
	"",
	1,
	-1,
	"script { ... }",
	cmd_script_exec,
	CMD_OWN_TS,
};

static enum cmd_retval
cmd_script_exec(struct cmd *self, struct cmd_q *cmdq)
{
	struct args *args = self->args;
	char	    *body = cmd_argv_to_string(args->argc, args->argv, 0);

	struct dsl_error err = { 0 };
	struct dsl_ast	*ast = dsl_parse(body, NULL, &err);
	xfree(body);

	if (ast == NULL) {
		cmdq_error(cmdq, "script: parse error at %d:%d: %s", err.line,
		    err.col, err.message != NULL ? err.message : "(unknown)");
		dsl_error_fini(&err);
		return (CMD_RETURN_ERROR);
	}

	struct dsl_error eerr = { 0 };
	bool		 ok   = dsl_eval(ast, cmdq, &eerr);
	if (!ok) {
		cmdq_error(cmdq, "script: eval error at %d:%d: %s", eerr.line,
		    eerr.col,
		    eerr.message != NULL ? eerr.message : "(unknown)");
		dsl_error_fini(&eerr);
		dsl_free(ast);
		return (CMD_RETURN_ERROR);
	}

	dsl_error_fini(&eerr);
	dsl_free(ast);
	return (CMD_RETURN_NORMAL);
}
