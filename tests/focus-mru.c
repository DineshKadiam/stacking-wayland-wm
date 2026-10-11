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
#include <stdlib.h>

#include "cow/cmd.h"
#include "cow/config.h"
#include "cow/focus-history.h"
#include "cow/seat.h"
#include "cow/view.h"
#include "cow/wm.h"
#include "river-window-management-v1-client-protocol.h"

#define CHECK(x) do { if (!(x)) { \
	fprintf(stderr, "%d: %s\n", __LINE__, #x); abort(); \
} } while (0)

static void
run(const char *text, bool success, struct cow_view *expected)
{
	struct cmd_list *list = NULL;
	char *cause = NULL;
	CHECK(cmd_string_parse(text, &list, NULL, 0, &cause) == 0);
	struct cmd_q *queue = cmdq_new();
	queue->client_fd = -1;
	cmdq_run(queue, list);
	CHECK(!queue->failed == success);
	CHECK(cow_wm_effective_focus() == expected);
	cmdq_free(queue);
	cmd_list_free(list);
}

static void
settle(void)
{
	if (cow_wm.pending_focus != NULL)
		cow_wm_set_focused_view(cow_wm.pending_focus);
	cow_wm.pending_focus = NULL;
}

int
main(void)
{
	cow_config = cow_config_create();
	wl_list_init(&cow_wm.views);
	wl_list_init(&cow_wm.outputs);
	wl_list_init(&cow_wm.seats);
	wl_list_init(&cow_wm.containers);
	struct cow_seat seat = {
	    .active_modifiers = RIVER_SEAT_V1_MODIFIERS_MOD1,
	    .binding_action_modifiers = RIVER_SEAT_V1_MODIFIERS_MOD1 };
	cow_wm.xkb_bindings_version = 3;
	cow_wm.current_interaction_seat = &seat;
	struct cow_view a = { .has_dimensions = true, .app_id = "foot" };
	struct cow_view b = { .has_dimensions = true, .app_id = "browser" };
	struct cow_view c = { .has_dimensions = true, .app_id = "foot" };
	wl_list_init(&a.selections);
	wl_list_init(&b.selections);
	wl_list_init(&c.selections);
	wl_list_insert(&cow_wm.views, &a.wm_link);
	wl_list_insert(&cow_wm.views, &b.wm_link);
	wl_list_insert(&cow_wm.views, &c.wm_link);
	cow_wm_set_focused_view(&a);
	cow_wm_set_focused_view(&b);
	cow_wm_set_focused_view(&c);
	uint64_t original = c.focus_seq;
	run("focus -n -O mru", true, &b);
	/* A follow operation can temporarily clear focus before settling. */
	cow_wm_set_focused_view(NULL);
	settle();
	run("focus -n -O mru", true, &a);
	settle();
	run("focus -n -O mru", true, &c);
	settle();
	seat.binding_action_modifiers |= RIVER_SEAT_V1_MODIFIERS_SHIFT;
	seat.active_modifiers |= RIVER_SEAT_V1_MODIFIERS_SHIFT;
	run("focus -p -O mru", true, &a);
	settle();
	CHECK(c.focus_seq == original && a.focus_seq < b.focus_seq);
	cow_focus_cycle_modifiers(&seat, RIVER_SEAT_V1_MODIFIERS_MOD1);
	CHECK(a.focus_seq < b.focus_seq);
	cow_focus_cycle_modifiers(&seat, 0);
	CHECK(a.focus_seq > c.focus_seq);
	/* IPC calls switch independently; the previous window is next again. */
	cow_wm.current_interaction_seat = NULL;
	run("focus -n -O mru", true, &c);
	settle();
	run("focus -n -O mru", true, &a);
	settle();
	run("focus -n -O mru -F 'self.app_id == \"foot\"'", true, &c);
	settle();
	a.flags |= COW_VIEW_CIRCULATE_SKIP_FLAG;
	run("focus -n -O mru -F 'self.app_id == \"foot\"'", true, &c);
	run("focus -n -O mru -F 'false'", false, &c);
	run("focus -O mru", false, &c);
	run("focus -n -O invalid", false, &c);
	b.layer = 7;
	run("focus -n -O mru -l 7", true, &b);
	settle();
	cow_wm_set_focused_view(&a);
	cow_wm_set_focused_view(&c);
	a.flags = 0;
	/* A closed snapshot entry is removed before its memory is released. */
	cow_wm.current_interaction_seat = &seat;
	run("focus -n -O mru", true, &a);
	settle();
	cow_focus_history_forget(&b);
	wl_list_remove(&b.wm_link);
	run("focus -n -O mru", true, &c);
	settle();
	/* Mouse/explicit focus interrupts the snapshot and records recency. */
	cow_wm_set_focused_view(&a);
	CHECK(a.focus_seq > c.focus_seq);
	cow_focus_cycle_end();
	cow_wm.current_interaction_seat = NULL;
	cow_wm_set_focused_view(NULL);
	wl_list_remove(&a.wm_link);
	wl_list_remove(&c.wm_link);
	cow_config_destroy(cow_config);
	return 0;
}
