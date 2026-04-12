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
 * Command: page -- navigate the virtual page grid.
 *
 * usage: page [-t output] [-n] [-p] [-d dir] [-c col row]
 *
 *   -t output   target a specific output by name
 *   -n          next page (right, wrapping to the next row at the end)
 *   -p          previous page (left, wrapping upward at the start)
 *   -d dir      move in direction: left | right | up | down
 *   -c col row  go to the specific page at (col, row)
 */

#include <string.h>

#include "cow/compat.h"
#include "cow/cmd.h"
#include "cow/config.h"
#include "cow/desk_ops.h"
#include "cow/output.h"
#include "cow/wm.h"

static enum cmd_retval cmd_page_exec(struct cmd *, struct cmd_q *);

struct cmd_entry cmd_page_entry = { "page", "t:d:c:np", 0, 0,
	"page [-t output] [-n] [-p] [-d left|right|up|down] [-c col row]",
	cmd_page_exec };

static enum cmd_retval
cmd_page_exec(struct cmd *self, struct cmd_q *cmdq)
{
	struct args	  *args = self->args;
	struct cow_output *output;

	/* Resolve target output (-t name or current) */
	const char *out_name = args_get(args, 't');
	if (out_name != NULL) {
		output = NULL;
		struct cow_output *o;
		wl_list_for_each(o, &cow_wm.outputs, wm_link)
		{
			if (o->name != NULL && strcmp(o->name, out_name) == 0) {
				output = o;
				break;
			}
		}
		if (output == NULL) {
			cmdq_error(cmdq, "page: unknown output: %s", out_name);
			return CMD_RETURN_ERROR;
		}
	} else {
		output = cow_wm.current_output;
	}

	if (output == NULL) {
		cmdq_error(cmdq, "page: no output");
		return CMD_RETURN_ERROR;
	}

	if (cow_config == NULL ||
	    (cow_config->page_cols <= 1 && cow_config->page_rows <= 1)) {
		/* Single-page grid — nothing to navigate */
		return CMD_RETURN_NORMAL;
	}

	int cols = cow_config->page_cols;
	int rows = cow_config->page_rows;
	int col	 = cow_page_col(output);
	int row	 = cow_page_row(output);

	if (args_has(args, 'n')) {
		/* Next page: right along the row, wrapping to next row */
		col++;
		if (col >= cols) {
			col = 0;
			row = (row + 1) % rows;
		}
	} else if (args_has(args, 'p')) {
		/* Previous page: left along the row, wrapping up */
		col--;
		if (col < 0) {
			col = cols - 1;
			row = (row - 1 + rows) % rows;
		}
	} else if (args_get(args, 'd') != NULL) {
		const char *dir = args_get(args, 'd');
		if (strcmp(dir, "right") == 0)
			col = (col + 1) % cols;
		else if (strcmp(dir, "left") == 0)
			col = (col - 1 + cols) % cols;
		else if (strcmp(dir, "down") == 0)
			row = (row + 1) % rows;
		else if (strcmp(dir, "up") == 0)
			row = (row - 1 + rows) % rows;
		else {
			cmdq_error(cmdq,
			    "page: unknown direction: %s (left|right|up|down)",
			    dir);
			return CMD_RETURN_ERROR;
		}
	} else if (args_get(args, 'c') != NULL) {
		/* Absolute page: -c col row (two positional args follow) */
		const char *col_s = args_get(args, 'c');
		if (args->argc < 1) {
			cmdq_error(cmdq,
			    "page: -c requires col and row arguments");
			return CMD_RETURN_ERROR;
		}
		const char *row_s = args->argv[0];
		const char *errstr;
		col = (int)strtonum(col_s, 0, cols - 1, &errstr);
		if (errstr != NULL) {
			cmdq_error(cmdq, "page: bad col value: %s", col_s);
			return CMD_RETURN_ERROR;
		}
		row = (int)strtonum(row_s, 0, rows - 1, &errstr);
		if (errstr != NULL) {
			cmdq_error(cmdq, "page: bad row value: %s", row_s);
			return CMD_RETURN_ERROR;
		}
	} else {
		cmdq_error(cmdq, cmd_page_entry.usage);
		return CMD_RETURN_ERROR;
	}

	cow_page_switch(output, col, row);
	return CMD_RETURN_NORMAL;
}
