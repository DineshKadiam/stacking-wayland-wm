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
#include <stdio.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cow/cmd.h"
#include "cow/config.h"
#include "cow/memory.h"
#include "cow/output.h"
#include "cow/pan_frames.h"
#include "cow/pointer_op.h"
#include "cow/policy.h"
#include "cow/rule.h"
#include "cow/seat.h"
#include "cow/util.h"
#include "cow/view.h"
#include "cow/wm.h"

#define CHECK(expr) do { \
	if (!(expr)) { \
		fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); \
		abort(); \
	} \
} while (0)

static struct cow_config config;
static struct cow_seat seat;
static struct cow_view view;
static struct cow_output left, right;
static struct cow_desk left_desk, right_desk;
static int fired;

static enum cmd_retval
record_drop(struct cmd *cmd, struct cmd_q *queue)
{
	(void)cmd;
	CHECK(!seat.op_in_progress);
	CHECK(queue->default_target.window == &view);
	CHECK(queue->default_target.output == &right);
	CHECK(queue->default_target.desk == &right_desk);
	CHECK(strstr(queue->event_json, "\"output\":\"right\"") != NULL);
	CHECK(strstr(queue->event_json, "\"edge\":\"e\"") != NULL);
	fired++;
	return CMD_RETURN_NORMAL;
}

static const struct cmd_entry record_entry = {
	.name = "record", .exec = record_drop,
};

static void
add_rule(const char *name, enum cow_rule_type type, const char *output)
{
	struct cow_rule *rule = xcalloc(1, sizeof(*rule));
	cow_policy_init(&rule->policy);
	rule->policy.name = xstrdup(name);
	rule->policy.match_spec = xstrdup("%foot");
	rule->policy.output_name = xstrdup(output);
	rule->type = type;
	char *argv[] = { "nop" }, *cause = NULL;
	rule->cmdlist = cmd_list_parse(1, argv, NULL, 0, &cause);
	CHECK(rule->cmdlist != NULL);
	TAILQ_FIRST(&rule->cmdlist->list)->entry = &record_entry;
	cow_rule_add(rule);
}

static void
arm(int x, int y)
{
	seat.pointer_x = x;
	seat.pointer_y = y;
	cow_output_pan_op_arm(&seat);
}

static void
dwell(void)
{
	CHECK(cow_wm.pan_lock_output != NULL);
	cow_wm.pan_lock_output->pan_arm_ms = cow_mono_ms() - 100;
	cow_output_pan_op_arm(&seat);
}

int
main(void)
{
	cow_config = &config;
	config.page_cols = config.page_rows = 1;
	config.edge_scroll_delay_ms = 50;
	wl_list_init(&cow_wm.outputs);
	wl_list_init(&cow_wm.seats);
	wl_list_init(&cow_wm.views);
	wl_list_init(&cow_wm.menus);
	wl_list_init(&cow_wm.open_menus);
	wl_list_init(&cow_wm.menu_graveyard);
	left.name = "left";
	right.name = "right";
	left.width = right.width = left.height = right.height = 1000;
	right.x = 1000;
	left.current_desk = &left_desk;
	right.current_desk = &right_desk;
	left.pan_edge = right.pan_edge = -1;
	wl_list_insert(&cow_wm.outputs, &left.wm_link);
	wl_list_insert(&cow_wm.outputs, &right.wm_link);
	wl_list_insert(&cow_wm.seats, &seat.wm_link);
	wl_list_init(&seat.keybindings);
	wl_list_init(&seat.pointer_bindings);
	seat.op_in_progress = true;
	seat.has_pointer_position = true;
	seat.op.type = COW_POINTER_OP_MOVE;
	seat.op.view = &view;
	view.app_id = "foot";
	view.output = &left;
	view.desk = &left_desk;
	pan_frames_init();
	cow_rule_init();
	add_rule("east", COW_RULE_ON_EDGEDROP_E, "right");
	add_rule("north", COW_RULE_ON_EDGEDROP_N, "right");

	/* No paging required; scope follows the edge, not the view's owner. */
	arm(999, 500);
	CHECK(cow_wm.pan_lock_output == NULL);
	arm(1999, 500);
	CHECK(cow_wm.pan_lock_output == &right);
	CHECK(seat.op.drop_output == NULL);
	dwell();
	CHECK(seat.op.drop_output == &right);
	CHECK(seat.op.drop_edge == COW_PAN_RIGHT);

	/* Switching directly to another edge starts a fresh dwell. */
	arm(1999, 0);
	CHECK(seat.op.drop_output == NULL);
	CHECK(cow_wm.pan_lock_edge == COW_PAN_TOP);
	dwell();
	CHECK(seat.op.drop_edge == COW_PAN_TOP);
	arm(1500, 500);
	CHECK(seat.op.drop_output == NULL);
	CHECK(cow_wm.pan_lock_output == NULL);

	/* Losing modifiers clears the candidate, including while stationary. */
	config.edge_scroll_modifiers = 1;
	seat.active_modifiers = 1;
	arm(1999, 500);
	dwell();
	seat.active_modifiers = 0;
	cow_pan_frames_modifiers_changed(&seat);
	CHECK(seat.op.drop_output == NULL);
	CHECK(cow_wm.pan_lock_output == NULL);
	config.edge_scroll_modifiers = 0;

	/* Rule removal and selector changes invalidate a ready candidate. */
	arm(1999, 500);
	dwell();
	CHECK(cow_rule_remove_by_name("east"));
	arm(1999, 500);
	CHECK(seat.op.drop_output == NULL);
	add_rule("east", COW_RULE_ON_EDGEDROP_E, "right");
	arm(1999, 500);
	dwell();
	view.app_id = "other";
	arm(1999, 500);
	CHECK(seat.op.drop_output == NULL);
	view.app_id = "foot";

	/* Crossing outputs cannot retain the previous output's candidate. */
	arm(1999, 500);
	dwell();
	arm(999, 500);
	CHECK(seat.op.drop_output == NULL);
	CHECK(cow_wm.pan_lock_output == NULL);

	/* Output destruction also cancels the lock and pending drop. */
	arm(1999, 500);
	dwell();
	pan_frames_destroy_output(&right);
	CHECK(seat.op.drop_output == NULL);
	CHECK(cow_wm.pan_lock_output == NULL);

	/* Losing the drag's view cancels before it can be freed. */
	arm(1999, 500);
	dwell();
	cow_seat_forget_view(&seat, &view);
	CHECK(seat.op.drop_output == NULL);
	CHECK(cow_wm.pan_lock_output == NULL);
	seat.op.view = &view;

	/* Unmatched rules leave normal paging available, except global seams. */
	config.edge_scroll = true;
	config.page_cols = 2;
	config.desktop_config = COW_DESKTOP_GLOBAL;
	view.app_id = "other";
	arm(999, 500);
	CHECK(cow_wm.pan_lock_output == NULL);
	arm(1999, 500);
	CHECK(cow_wm.pan_lock_output == &right);
	CHECK(seat.op.drop_output == NULL);
	cow_pan_cancel();
	config.desktop_config = COW_DESKTOP_PER_OUTPUT;
	arm(999, 500);
	CHECK(cow_wm.pan_lock_output == &left);
	cow_pan_cancel();
	config.edge_scroll = false;
	config.page_cols = 1;
	view.app_id = "foot";

	/* Dispatch after ending the grab, retaining the triggering context. */
	arm(1999, 500);
	dwell();
	struct cow_output *target = seat.op.drop_output;
	int edge = seat.op.drop_edge;
	cow_pan_cancel();
	seat.op_in_progress = false;
	cow_pan_finish_drop(&view, target, edge);
	CHECK(fired == 1);
	CHECK(seat.op.drop_output == NULL);

	cow_rule_fini();
	return 0;
}
