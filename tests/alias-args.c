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
#include "cow/alias.h"
#include "cow/cmd.h"
#include "cow/config.h"
#include "cow/memory.h"
#include "cow/menu.h"
#include "cow/wm.h"
#include "config-serialize.h"
#include "state-snapshot.h"
#include "state-value.h"

#define CHECK(x)                                                           \
	do {                                                               \
		if (!(x)) {                                                \
			fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, \
			    #x);                                           \
			abort();                                           \
		}                                                          \
	} while (0)

static bool
run(const char *text)
{
	struct cmd_list *list  = NULL;
	char		*cause = NULL;
	if (cmd_string_parse(text, &list, NULL, 0, &cause) != 0) {
		xfree(cause);
		return false;
	}
	struct cmd_q *queue = cmdq_new();
	queue->client_fd    = -1;
	cmdq_run(queue, list);
	bool ok = !queue->failed;
	cmdq_free(queue);
	cmd_list_free(list);
	return ok;
}

int
main(void)
{
	cow_config = cow_config_create();
	wl_list_init(&cow_wm.menus);
	wl_list_init(&cow_wm.open_menus);
	CHECK(run(
	    "alias -p value save script { menu-add Results #{value} nop }"));
	CHECK(run("save 'spaces ; nop # and quotes'"));
	struct cow_menu *menu = cow_menu_find("Results");
	CHECK(menu != NULL && wl_list_length(&menu->items) == 1);
	struct cow_menu_item *item = wl_container_of(menu->items.next, item,
	    link);
	CHECK(strcmp(item->label, "spaces ; nop # and quotes") == 0);
	struct state_value *record = state_snapshot_alias(
	    cow_alias_find("save"));
	char *serialized = cow_config_serialize_topic("alias", record);
	CHECK(serialized != NULL);
	CHECK(run("alias -d save"));
	CHECK(run(serialized));
	xfree(serialized);
	state_value_unref(record);
	CHECK(strcmp(cow_alias_find("save")->parameters, "value") == 0);
	CHECK(!run("save"));
	CHECK(!run("save a b"));
	CHECK(!run("alias -p value,value bad script { nop }"));
	CHECK(!run("alias -p self bad script { nop }"));
	CHECK(!run("alias -p value, bad script { nop }"));
	CHECK(!run("alias -p value bad nop"));
	CHECK(!run("alias -dp value save"));
	CHECK(run("alias -p value forward script { save #{value} }"));
	CHECK(run("forward nested"));
	CHECK(run("alias -p first,second pair script { "
		  "menu-add Results \"#{first}:#{second}\" nop }"));
	CHECK(run("pair '' '-a ; {}'"));
	item = wl_container_of(menu->items.prev, item, link);
	CHECK(strcmp(item->label, ":-a ; {}") == 0);
	CHECK(!run("script { nop #{value} }"));
	CHECK(run("alias -p value recur script { nop }"));
	CHECK(run("alias -p value recur script { recur #{value} }"));
	CHECK(!run("recur recursion"));
	CHECK(
	    run("alias -p value remove-self script { alias -d remove-self }"));
	CHECK(run("remove-self safely"));
	CHECK(cow_alias_find("remove-self") == NULL);
	CHECK(run(
	    "alias -p value replace-self script { alias replace-self nop }"));
	CHECK(run("replace-self safely"));
	CHECK(run("replace-self"));
	CHECK(cow_alias_find("replace-self")->parameters == NULL);
	CHECK(!run("alias -p value save script { if }"));
	CHECK(strcmp(cow_alias_find("save")->parameters, "value") == 0);
	CHECK(run("save still-valid"));
	cow_menu_destroy(menu);
	cow_config_destroy(cow_config);
	cow_config = NULL;
	return 0;
}
