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

#include "cow/cmd.h"
#include "cow/ipc.h"
#include "cow/memory.h"
#include "cow/util.h"

static enum cmd_retval cmd_include_exec(struct cmd *, struct cmd_q *);
static char	       *cmd_include_path(const char *, struct cmd_q *);

struct cmd_entry cmd_include_entry = {
	"include",
	"",
	1,
	1,
	"include file",
	cmd_include_exec,
	0,
};

static enum cmd_retval
cmd_include_exec(struct cmd *self, struct cmd_q *cmdq)
{
	struct args *args = self->args;
	char	    *path = cmd_include_path(args->argv[0], cmdq);

	if (path == NULL)
		return (CMD_RETURN_ERROR);

	bool ok = cow_ipc_include_config(path, cmdq);
	if (!ok)
		cmdq_error(cmdq, "include: cannot open %s", path);
	xfree(path);

	return ok ? CMD_RETURN_NORMAL : CMD_RETURN_ERROR;
}

static char *
cmd_include_path(const char *path, struct cmd_q *cmdq)
{
	const char *base = NULL;
	bool home_path =
	    (path[0] == '~' && (path[1] == '\0' || path[1] == '/')) ||
	    (strncmp(path, "$HOME", 5) == 0 &&
		(path[5] == '\0' || path[5] == '/')) ||
	    (strncmp(path, "${HOME}", 7) == 0 &&
		(path[7] == '\0' || path[7] == '/'));
	if (path[0] != '/' && !home_path) {
		base = getenv("XDG_RUNTIME_DIR");
		if (base == NULL || base[0] == '\0') {
			cmdq_error(cmdq, "include: XDG_RUNTIME_DIR not set");
			return (NULL);
		}
	}
	char *expanded = cow_path_resolve(base, path);
	if (expanded == NULL)
		cmdq_error(cmdq, "include: cannot expand path: %s", path);
	return (expanded);
}
