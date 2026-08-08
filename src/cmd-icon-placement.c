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

#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "cow/cmd.h"
#include "cow/compat.h"
#include "cow/config.h"
#include "cow/icon_placement.h"
#include "cow/ipc.h"
#include "cow/output.h"
#include "cow/status.h"
#include "cow/view.h"
#include "cow/wm.h"

static enum cmd_retval cmd_icon_placement_exec(struct cmd *, struct cmd_q *);

struct cmd_entry cmd_icon_placement_entry = {
	"icon-placement",
	"+o:",
	1,
	3,
	"icon-placement [-o OUTPUT] box full|GEOMETRY | fill DIR DIR | "
	"grid X Y | compact BOOL | reset",
	cmd_icon_placement_exec,
	0,
};

static void
reflow_output(struct cow_output *output, bool invalidate)
{
	if (output == NULL)
		return;
	if (invalidate)
		cow_view_invalidate_auto_icon_positions(output);
	cow_view_layout_icons_on_output(output);
	cow_wm_request_manage_dirty();
}

static void
reflow_all(bool invalidate)
{
	struct cow_output *output;
	wl_list_for_each(output, &cow_wm.outputs, wm_link)
		reflow_output(output, invalidate);
}

static bool
parse_grid(const char *xvalue, const char *yvalue, int *x, int *y,
    struct cmd_q *cmdq)
{
	const char *errstr;
	long long n = strtonum(xvalue, 1, INT_MAX, &errstr);
	if (errstr != NULL) {
		cmdq_error(cmdq, "icon-placement: invalid X grid: %s", xvalue);
		return false;
	}
	*x = (int)n;
	n = strtonum(yvalue, 1, INT_MAX, &errstr);
	if (errstr != NULL) {
		cmdq_error(cmdq, "icon-placement: invalid Y grid: %s", yvalue);
		return false;
	}
	*y = (int)n;
	return true;
}

static enum cmd_retval
cmd_icon_placement_exec(struct cmd *self, struct cmd_q *cmdq)
{
	struct args *args = self->args;
	const char *output_spec = args_get(args, 'o');
	const char *operation = args->argv[0];
	struct cow_output *output = NULL;

	if (output_spec != NULL && !cow_ipc.startup_mode) {
		output = cow_output_find_by_spec(output_spec);
		if (output == NULL) {
			cmdq_error(cmdq, "icon-placement: unknown output: %s",
			    output_spec);
			return CMD_RETURN_ERROR;
		}
	}

	if (strcmp(operation, "reset") == 0) {
		if (args->argc != 1) {
			cmdq_error(cmdq, "usage: %s", self->entry->usage);
			return CMD_RETURN_ERROR;
		}
		if (output_spec == NULL)
			cow_icon_placement_default(&cow_config->icon_placement);
		else {
			cow_icon_placement_override_remove(cow_config, output_spec);
			if (output != NULL) {
				if (output->name != NULL)
					cow_icon_placement_override_remove(cow_config,
					    output->name);
				char number[32];
				snprintf(number, sizeof(number), "@%d",
					output->number);
				cow_icon_placement_override_remove(cow_config,
				    number);
			}
		}
		if (!cow_ipc.startup_mode) {
			if (output_spec == NULL)
				reflow_all(true);
			else
				reflow_output(output, true);
			cow_status_emit();
		}
		return CMD_RETURN_NORMAL;
	}

	struct cow_icon_placement value = cow_config->icon_placement;
	struct cow_icon_placement_override *override = NULL;
	if (output_spec != NULL) {
		override = cow_icon_placement_override_get(cow_config,
		    output_spec, false);
		if (override != NULL)
			value = override->placement;
	}
	uint32_t field;

	if (strcmp(operation, "box") == 0) {
		if (args->argc != 2 ||
		    !cow_icon_box_parse(args->argv[1], &value.box)) {
			cmdq_error(cmdq,
			    "icon-placement: box expects full or WIDTHxHEIGHT±X±Y");
			return CMD_RETURN_ERROR;
		}
		field = COW_ICON_PLACEMENT_BOX;
	} else if (strcmp(operation, "fill") == 0) {
		if (args->argc != 3 || !cow_icon_fill_parse(args->argv[1],
			args->argv[2], &value.primary, &value.secondary)) {
			cmdq_error(cmdq, "icon-placement: fill expects one "
			    "horizontal and one vertical direction");
			return CMD_RETURN_ERROR;
		}
		field = COW_ICON_PLACEMENT_FILL;
	} else if (strcmp(operation, "grid") == 0) {
		if (args->argc != 3 || !parse_grid(args->argv[1], args->argv[2],
			&value.grid_x, &value.grid_y, cmdq))
			return CMD_RETURN_ERROR;
		field = COW_ICON_PLACEMENT_GRID;
	} else if (strcmp(operation, "compact") == 0) {
		if (args->argc != 2 ||
		    !cmd_parse_bool(args->argv[1], &value.compact)) {
			cmdq_error(cmdq,
			    "icon-placement: compact expects a boolean");
			return CMD_RETURN_ERROR;
		}
		field = COW_ICON_PLACEMENT_COMPACT;
	} else {
		cmdq_error(cmdq, "icon-placement: unknown operation: %s",
		    operation);
		return CMD_RETURN_ERROR;
	}

	if (override != NULL) {
		override->placement = value;
		override->fields |= field;
	} else if (output_spec != NULL) {
		override = cow_icon_placement_override_get(cow_config,
		    output_spec, true);
		override->placement = value;
		override->fields = field;
	} else {
		cow_config->icon_placement = value;
	}
	if (!cow_ipc.startup_mode) {
		if (output_spec == NULL)
			reflow_all(field != COW_ICON_PLACEMENT_COMPACT);
		else
			reflow_output(output,
			    field != COW_ICON_PLACEMENT_COMPACT);
		cow_status_emit();
	}
	return CMD_RETURN_NORMAL;
}
