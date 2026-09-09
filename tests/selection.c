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

#include <assert.h>

#include "cow/compat.h"
#include "cow/memory.h"
#include "cow/selection.h"
#include "cow/update.h"
#include "cow/view.h"
#include "cow/wm.h"

struct cow_wm cow_wm;

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

	assert(cow_selection_add(&a, NULL));
	assert(cow_selection_add(&b, "work"));
	assert(!cow_selection_add(&a, NULL));
	assert(cow_selection_has(&a, COW_SELECTION_DEFAULT));
	assert(!cow_selection_has(&a, "work"));

	assert(cow_selection_collect(NULL, &views) == 1);
	assert(views[0] == &a);
	xfree(views);

	struct cow_view *replacement[] = { &b, &c };
	cow_selection_replace(NULL, replacement, 2);
	assert(!cow_selection_has(&a, NULL));
	assert(cow_selection_has(&b, NULL));
	assert(cow_selection_has(&c, NULL));
	assert(cow_selection_has(&b, "work"));

	assert(cow_selection_toggle(&b, "work"));
	assert(!cow_selection_has(&b, "work"));
	cow_selection_clear(NULL);
	assert(!cow_selection_has(&b, NULL));
	assert(!cow_selection_has(&c, NULL));

	cow_selection_fini_view(&a);
	cow_selection_fini_view(&b);
	cow_selection_fini_view(&c);
	return 0;
}
