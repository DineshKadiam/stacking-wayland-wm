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

#include <string.h>

#include "cow/memory.h"
#include "cow/selection.h"
#include "cow/update.h"
#include "cow/view.h"
#include "cow/wm.h"

const char *
cow_selection_name(const char *name)
{
	return name == NULL || name[0] == '\0' ? COW_SELECTION_DEFAULT : name;
}

static struct cow_selection_entry *
selection_find(const struct cow_view *view, const char *name)
{
	struct cow_selection_entry *entry;

	name = cow_selection_name(name);
	wl_list_for_each(entry, &view->selections, link)
	{
		if (strcmp(entry->name, name) == 0)
			return entry;
	}
	return NULL;
}

bool
cow_selection_has(const struct cow_view *view, const char *name)
{
	return view != NULL && selection_find(view, name) != NULL;
}

static void
selection_changed(void)
{
	cow_update_request(COW_UPDATE_STATUS);
}

bool
cow_selection_add(struct cow_view *view, const char *name)
{
	if (view == NULL || view->closed || selection_find(view, name) != NULL)
		return false;

	struct cow_selection_entry *entry = xcalloc(1, sizeof(*entry));
	entry->name = xstrdup(cow_selection_name(name));
	wl_list_insert(view->selections.prev, &entry->link);
	selection_changed();
	return true;
}

bool
cow_selection_remove(struct cow_view *view, const char *name)
{
	if (view == NULL)
		return false;
	struct cow_selection_entry *entry = selection_find(view, name);
	if (entry == NULL)
		return false;

	wl_list_remove(&entry->link);
	xfree(entry->name);
	xfree(entry);
	selection_changed();
	return true;
}

bool
cow_selection_toggle(struct cow_view *view, const char *name)
{
	if (cow_selection_has(view, name))
		return cow_selection_remove(view, name);
	return cow_selection_add(view, name);
}

void
cow_selection_clear(const char *name)
{
	struct cow_view *view;

	name = cow_selection_name(name);
	wl_list_for_each(view, &cow_wm.views, wm_link)
		(void)cow_selection_remove(view, name);
}

void
cow_selection_fini_view(struct cow_view *view)
{
	struct cow_selection_entry *entry, *tmp;

	if (view == NULL)
		return;
	wl_list_for_each_safe(entry, tmp, &view->selections, link)
	{
		wl_list_remove(&entry->link);
		xfree(entry->name);
		xfree(entry);
	}
}

size_t
cow_selection_collect(const char *name, struct cow_view ***views)
{
	size_t count = 0;
	struct cow_view *view;

	*views = NULL;
	wl_list_for_each(view, &cow_wm.views, wm_link)
	{
		if (!view->closed && cow_selection_has(view, name))
			count++;
	}
	if (count == 0)
		return 0;

	*views = xcalloc(count, sizeof(**views));
	size_t i = 0;
	wl_list_for_each(view, &cow_wm.views, wm_link)
	{
		if (!view->closed && cow_selection_has(view, name))
			(*views)[i++] = view;
	}
	return count;
}

void
cow_selection_replace(const char *name, struct cow_view **views, size_t count)
{
	cow_selection_clear(name);
	for (size_t i = 0; i < count; i++) {
		if (views[i] != NULL && !views[i]->closed)
			(void)cow_selection_add(views[i], name);
	}
}
