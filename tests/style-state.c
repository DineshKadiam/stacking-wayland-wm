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
#include <string.h>

#include "cow/cmd.h"
#include "cow/ipc.h"
#include "cow/memory.h"
#include "cow/style.h"
#include "cow/view.h"
#include "cow/wm.h"

#define CHECK(x)                                                   \
	do {                                                       \
		if (!(x)) {                                        \
			fprintf(stderr, "%d: %s\n", __LINE__, #x); \
			abort();                                   \
		}                                                  \
	} while (0)

static bool
run_result(const char *text)
{
	struct cmd_list *list  = NULL;
	char		*cause = NULL;
	CHECK(cmd_string_parse(text, &list, "/tmp/config/cow.conf", 0,
		  &cause) == 0);
	struct cmd_q *queue = cmdq_new();
	queue->client_fd    = -1;
	cmdq_run(queue, list);
	bool ok = !queue->failed;
	cmdq_free(queue);
	cmd_list_free(list);
	return ok;
}

static void
run(const char *text)
{
	CHECK(run_result(text));
}

int
main(void)
{
	struct cow_view panel = {.title = "cowbuttons:Starter:DP-1",
	    .app_id = "cowbuttons", .has_dimensions = true};
	CHECK(cow_view_matches_selector(&panel, "=cowbuttons:Starter:DP-1"));
	CHECK(cow_view_matches_selector(&panel, "%cowbuttons"));
	CHECK(cow_view_matches_selector(&panel, "/Starter/"));
	panel.title = "cowbuttons:Starter:DP-10";
	CHECK(!cow_view_matches_selector(&panel, "=cowbuttons:Starter:DP-1"));
	panel.title = "cowbuttons:Starter:DP-1-terminal";
	CHECK(!cow_view_matches_selector(&panel, "=cowbuttons:Starter:DP-1"));
	panel.title = NULL;
	CHECK(!cow_view_matches_selector(&panel, "=cowbuttons:Starter:DP-1"));

	wl_list_init(&cow_wm.views);
	wl_list_init(&cow_wm.outputs);
	panel.title = "cowbuttons:Starter:DP-1.0";
	wl_list_insert(&cow_wm.views, &panel.wm_link);
	struct cmd_ctx ctx;
	char *cause = NULL;
	CHECK(cmd_ctx_parse("=cowbuttons:Starter:DP-1.0", &ctx, &cause));
	CHECK(ctx.window == &panel);
	xfree(cause);
	wl_list_remove(&panel.wm_link);

	cow_ipc.startup_mode = true;
	run("style -g -n from-config {\nlayer 2\n}");
	cow_ipc.startup_mode = false;
	run("style -t DP-1:work -n pager -s /cowpager:extslack:DP-1/ {\n"
	    "decor {\nborder.width 4\ntitlebar.enabled false\n}\n"
	    "placement.anchor top-right\nsticky.desk true\n}");
	cJSON *saved = cow_style_save_runtime();
	CHECK(cJSON_GetArraySize(saved) == 1);
	cow_style_fini();
	cow_style_restore_runtime(saved);
	struct cow_style *style = cow_style_find_by_name("pager");
	CHECK(style != NULL && style->runtime);
	CHECK(strcmp(style->file, "/tmp/config/cow.conf") == 0);
	CHECK(strcmp(style->policy.output_name, "DP-1") == 0);
	CHECK(strcmp(style->policy.desk_name, "work") == 0);
	CHECK(
	    strcmp(style->policy.match_spec, "/cowpager:extslack:DP-1/") == 0);
	CHECK(wl_list_length(&style->entries) == 4);
	CHECK(cow_style_find_by_name("from-config") == NULL);
	cow_style_restore_runtime(saved);
	CHECK(wl_list_length(cow_styles_head()) == 1);
	cJSON *again = cow_style_save_runtime();
	CHECK(cJSON_Compare(saved, again, true));
	cJSON_Delete(again);

	cow_style_fini();
	cow_ipc.startup_mode = true;
	run("style -g -n pager {\nlayer 3\n}");
	cow_ipc.startup_mode = false;
	cow_style_restore_runtime(saved);
	style = cow_style_find_by_name("pager");
	CHECK(!style->runtime && wl_list_length(&style->entries) == 1);
	cJSON_Delete(saved);

	/* A saved record must never execute other commands or deletions. */
	saved	      = cJSON_CreateArray();
	cJSON *record = cJSON_CreateObject();
	cJSON_AddStringToObject(record, "command", "style -d -n pager");
	cJSON_AddItemToArray(saved, record);
	cow_style_restore_runtime(saved);
	CHECK(cow_style_find_by_name("pager") == style);
	cJSON_ReplaceItemInObject(record, "command",
	    cJSON_CreateString("style -d -n pager; nop"));
	cow_style_restore_runtime(saved);
	CHECK(cow_style_find_by_name("pager") == style);
	cJSON_Delete(saved);
	cow_style_fini();
	run("style -r -g -n restartable {\nlayer 2\n}");
	run("style -g -n later {\nlayer 4\n}");
	run("style -r -g -n restartable {\nlayer 3\n}");
	CHECK(wl_list_length(cow_styles_head()) == 2);
	style = cow_style_find_by_name("restartable");
	CHECK(strstr(style->body, "layer 3") != NULL);
	CHECK(!run_result("style -r -g -n restartable {\ninvalid true\n}"));
	CHECK(cow_style_find_by_name("restartable") == style);
	CHECK(!run_result("style -g -n restartable {\nlayer 1\n}"));
	CHECK(cow_style_find_by_name("restartable") == style);
	CHECK(cow_styles_head()->next == &style->link);
	CHECK(style->link.next == &cow_style_find_by_name("later")->link);
	cow_style_fini();
	return 0;
}
