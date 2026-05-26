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
 * continuation (added in step 7) and arrives here as a single
 * argv element including the outer braces.  cmd_script_exec()
 * parses it lazily on first invocation, caches the AST, and
 * evaluates against the calling cmd_q.  Errors are reported via
 * cmdq_error().
 *
 * Step 1: command registered, exec is a no-op so the wiring is
 * verifiable end-to-end before the parser sprouts real productions.
 */

#include "cow/attrs.h"
#include "cow/cmd.h"
#include "cow/dsl.h"

static enum cmd_retval cmd_script_exec(struct cmd *, struct cmd_q *);

struct cmd_entry cmd_script_entry = {
	"script",
	"",
	1,
	1,
	"script { ... }",
	cmd_script_exec,
	CMD_OWN_TS,
};

static enum cmd_retval
cmd_script_exec(__unused struct cmd *self, __unused struct cmd_q *cmdq)
{
	/* Step 1: no-op.  Real parse + eval lands in step 6. */
	return (CMD_RETURN_NORMAL);
}
