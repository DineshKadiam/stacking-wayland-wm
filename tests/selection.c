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

#include "cow/compat.h"
#include "cow/memory.h"
#include "cow/selection.h"
#include "cow/update.h"
#include "cow/view.h"
#include "cow/wm.h"

struct cow_wm cow_wm;

#define CHECK(expr) do { if (!(expr)) return 1; } while (0)

void
cow_update_request_at(__unused uint32_t flags, __unused const char *file,
    __unused int line, __unused const char *function)
{
}

static void
view_init(struct cow_view *view)
{
	wl_list_init(&view->selections);
	wl_list_insert(cow_wm.views.prev, &view->wm_link);
}

int
main(void)
{
	struct cow_view a = { 0 }, b = { 0 }, c = { 0 };
	struct cow_view **views = NULL;

	wl_list_init(&cow_wm.views);
	view_init(&a);
	view_init(&b);
	view_init(&c);

	CHECK(cow_selection_add(&a, NULL));
	CHECK(cow_selection_add(&b, "work"));
	CHECK(!cow_selection_add(&a, NULL));
	CHECK(cow_selection_has(&a, COW_SELECTION_DEFAULT));
	CHECK(!cow_selection_has(&a, "work"));

	CHECK(cow_selection_collect(NULL, &views) == 1);
	CHECK(views[0] == &a);
	xfree(views);

	struct cow_view *replacement[] = { &b, &c };
	cow_selection_replace(NULL, replacement, 2);
	CHECK(!cow_selection_has(&a, NULL));
	CHECK(cow_selection_has(&b, NULL));
	CHECK(cow_selection_has(&c, NULL));
	CHECK(cow_selection_has(&b, "work"));

	struct cow_selection_summary *summaries = NULL;
	CHECK(cow_selection_summaries(&summaries) == 2);
	CHECK(strcmp(summaries[0].name, "default") == 0);
	CHECK(summaries[0].count == 2);
	CHECK(strcmp(summaries[1].name, "work") == 0);
	CHECK(summaries[1].count == 1);
	cow_selection_summaries_free(summaries, 2);

	CHECK(cow_selection_toggle(&b, "work"));
	CHECK(!cow_selection_has(&b, "work"));
	cow_selection_clear(NULL);
	CHECK(!cow_selection_has(&b, NULL));
	CHECK(!cow_selection_has(&c, NULL));

	cow_selection_fini_view(&a);
	cow_selection_fini_view(&b);
	cow_selection_fini_view(&c);
	return 0;
}
