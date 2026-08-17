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

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "cow/compat.h"
#include "cow/config.h"
#include "cow/memory.h"
#include "cow/output.h"
#include "cow/view.h"
#include "cow/wm.h"

void
cow_desk_set_init(struct cow_desk_set *set)
{
	wl_list_init(&set->desks);
	set->count	= 0;
	set->next_nr	= 0;
	set->user_named = false;
}

void
cow_desk_set_fini(struct cow_desk_set *set)
{
	struct cow_desk *d, *tmp;
	wl_list_for_each_safe(d, tmp, &set->desks, set_link)
	{
		wl_list_remove(&d->set_link);
		xfree(d->name);
		xfree(d);
	}
	set->count   = 0;
	set->next_nr = 0;
}

struct cow_desk *
cow_desk_set_append_default(struct cow_output *output)
{
	struct cow_desk_set *set = &output->desk_set;
	struct cow_desk	    *d	 = xcalloc(1, sizeof(*d));
	d->nr			 = set->next_nr++;
	d->output		 = output;
	wl_list_init(&d->views);

	char buf[16];
	snprintf(buf, sizeof(buf), "%u", (unsigned)d->nr);
	d->name = xstrdup(buf);

	wl_list_insert(set->desks.prev, &d->set_link);
	set->count++;
	return (d);
}

void
cow_desk_set_populate_defaults(struct cow_output *output)
{
	uint8_t count = cow_config != NULL ? cow_config->desk_count :
	    COW_DEFAULT_DESK_COUNT;

	for (uint8_t i = 0; i < count; i++)
		(void)cow_desk_set_append_default(output);
}

struct cow_desk *
cow_desk_by_nr(struct cow_output *output, uint8_t nr)
{
	if (output == NULL)
		return (NULL);
	struct cow_desk *d;
	wl_list_for_each(d, &output->desk_set.desks, set_link)
	{
		if (d->nr == nr)
			return (d);
	}
	return (NULL);
}

struct cow_desk *
cow_desk_by_name(struct cow_output *output, const char *name)
{
	if (output == NULL || name == NULL)
		return (NULL);
	struct cow_desk *d;
	wl_list_for_each(d, &output->desk_set.desks, set_link)
	{
		if (d->name != NULL && strcmp(d->name, name) == 0)
			return (d);
	}
	return (NULL);
}

struct cow_desk *
cow_desk_first(struct cow_output *output)
{
	if (output == NULL || wl_list_empty(&output->desk_set.desks))
		return (NULL);
	struct cow_desk *d;
	d = wl_container_of(output->desk_set.desks.next, d, set_link);
	return (d);
}

struct cow_desk *
cow_desk_last(struct cow_output *output)
{
	if (output == NULL || wl_list_empty(&output->desk_set.desks))
		return (NULL);
	struct cow_desk *d;
	d = wl_container_of(output->desk_set.desks.prev, d, set_link);
	return (d);
}

struct cow_desk *
cow_desk_next(struct cow_output *output, struct cow_desk *d)
{
	if (output == NULL || d == NULL)
		return (NULL);
	if (d->set_link.next == &output->desk_set.desks)
		return (cow_desk_first(output));
	struct cow_desk *n;
	n = wl_container_of(d->set_link.next, n, set_link);
	return (n);
}

struct cow_desk *
cow_desk_prev(struct cow_output *output, struct cow_desk *d)
{
	if (output == NULL || d == NULL)
		return (NULL);
	if (d->set_link.prev == &output->desk_set.desks)
		return (cow_desk_last(output));
	struct cow_desk *p;
	p = wl_container_of(d->set_link.prev, p, set_link);
	return (p);
}

uint8_t
cow_desk_count(const struct cow_output *output)
{
	return (output != NULL ? output->desk_set.count : 0);
}

bool
cow_desk_name_valid(const char *name, char **cause)
{
	if (name == NULL || name[0] == '\0') {
		xasprintf(cause, "desk-name: empty name");
		return (false);
	}
	for (const char *p = name; *p != '\0'; p++) {
		unsigned char c = (unsigned char)*p;
		if (c == ':' || c == ' ' || c == '\t' || c == '\n' ||
		    c == '\r' || c == '/' || !isprint(c)) {
			xasprintf(cause,
			    "desk-name: invalid character in '%s'", name);
			return (false);
		}
	}
	return (true);
}

static bool
ensure_up_to(struct cow_output *output, uint8_t nr)
{
	struct cow_desk_set *set = &output->desk_set;
	while (set->count <= nr) {
		if (cow_desk_set_append_default(output) == NULL)
			return (false);
	}
	return (true);
}

static bool
desk_has_views(const struct cow_desk *d)
{
	if (d == NULL)
		return (false);
	struct cow_view *v;
	wl_list_for_each(v, &cow_wm.views, wm_link)
	{
		if (!v->closed && v->desk == d)
			return (true);
	}
	return (false);
}

static bool
set_name_one(struct cow_output *output, uint8_t nr, const char *name,
    char **cause)
{
	if (!ensure_up_to(output, nr)) {
		xasprintf(cause, "desk-name: cannot extend set");
		return (false);
	}
	struct cow_desk *target = cow_desk_by_nr(output, nr);
	if (target == NULL) {
		xasprintf(cause, "desk-name: no such desk: %u", nr);
		return (false);
	}

	struct cow_desk *clash = cow_desk_by_name(output, name);
	if (clash != NULL && clash != target) {
		xasprintf(cause, "desk-name: name '%s' already used", name);
		return (false);
	}

	xstrset(&target->name, name);
	output->desk_set.user_named = true;
	return (true);
}

bool
cow_desk_set_name(struct cow_output *output, uint8_t nr, const char *name,
    char **cause)
{
	if (output == NULL) {
		xasprintf(cause, "desk-name: no output");
		return (false);
	}
	if (!cow_desk_name_valid(name, cause))
		return (false);

	bool global = cow_config != NULL &&
	    cow_config->desktop_config == COW_DESKTOP_GLOBAL;
	if (global) {
		struct cow_output *o;
		wl_list_for_each(o, &cow_wm.outputs, wm_link)
		{
			if (!set_name_one(o, nr, name, cause))
				return (false);
		}
		return (true);
	}
	return (set_name_one(output, nr, name, cause));
}

static bool
delete_one(struct cow_output *output, uint8_t nr, char **cause)
{
	struct cow_desk *target = cow_desk_by_nr(output, nr);
	if (target == NULL)
		return (true); /* nothing to do */
	if (desk_has_views(target)) {
		xasprintf(cause,
		    "desk-name: desk %u has windows; move them first", nr);
		return (false);
	}
	if (output->current_desk == target) {
		struct cow_desk *fallback = cow_desk_prev(output, target);
		if (fallback == target)
			fallback = NULL;
		output->current_desk = fallback;
	}
	wl_list_remove(&target->set_link);
	xfree(target->name);
	xfree(target);
	output->desk_set.count--;
	output->desk_set.user_named = true;
	if (output->current_desk == NULL)
		output->current_desk = cow_desk_first(output);
	return (true);
}

bool
cow_desk_set_delete(struct cow_output *output, uint8_t nr, char **cause)
{
	if (output == NULL) {
		xasprintf(cause, "desk-name: no output");
		return (false);
	}
	bool global = cow_config != NULL &&
	    cow_config->desktop_config == COW_DESKTOP_GLOBAL;
	if (global) {
		struct cow_output *o;
		wl_list_for_each(o, &cow_wm.outputs, wm_link)
		{
			if (!delete_one(o, nr, cause))
				return (false);
		}
		return (true);
	}
	return (delete_one(output, nr, cause));
}

static void
reset_one(struct cow_output *output)
{
	cow_desk_set_fini(&output->desk_set);
	cow_desk_set_init(&output->desk_set);
	cow_desk_set_populate_defaults(output);
	output->current_desk = cow_desk_first(output);
}

void
cow_desk_set_reset(struct cow_output *output)
{
	if (output == NULL)
		return;
	bool global = cow_config != NULL &&
	    cow_config->desktop_config == COW_DESKTOP_GLOBAL;
	if (global) {
		struct cow_output *o;
		wl_list_for_each(o, &cow_wm.outputs, wm_link)
		    reset_one(o);
		return;
	}
	reset_one(output);
}

static bool
resize_one_check(struct cow_output *output, uint8_t count, char **cause)
{
	if (output->desk_set.count <= count)
		return (true);
	struct cow_desk *d;
	wl_list_for_each(d, &output->desk_set.desks, set_link)
	{
		if (d->nr < count)
			continue;
		if (desk_has_views(d)) {
			xasprintf(cause,
			    "desks.count: desk %u has windows", d->nr);
			return (false);
		}
	}
	return (true);
}

static void
resize_one_apply(struct cow_output *output, uint8_t count)
{
	while (output->desk_set.count < count)
		(void)cow_desk_set_append_default(output);

	struct cow_desk *d, *tmp;
	wl_list_for_each_safe(d, tmp, &output->desk_set.desks, set_link)
	{
		if (d->nr < count)
			continue;
		if (output->current_desk == d)
			output->current_desk = NULL;
		wl_list_remove(&d->set_link);
		xfree(d->name);
		xfree(d);
		output->desk_set.count--;
	}
	if (output->desk_set.next_nr > count)
		output->desk_set.next_nr = count;
	if (output->current_desk == NULL)
		output->current_desk = cow_desk_last(output);
}

bool
cow_desk_set_resize(struct cow_output *output, uint8_t count, char **cause)
{
	if (output == NULL) {
		xasprintf(cause, "desks.count: no output");
		return (false);
	}
	if (count == 0) {
		xasprintf(cause, "desks.count: must be > 0");
		return (false);
	}
	if (count > COW_MAX_DESK_COUNT) {
		xasprintf(cause, "desks.count: max %d", COW_MAX_DESK_COUNT);
		return (false);
	}

	bool global = cow_config != NULL &&
	    cow_config->desktop_config == COW_DESKTOP_GLOBAL;

	if (global) {
		struct cow_output *o;
		wl_list_for_each(o, &cow_wm.outputs, wm_link)
		{
			if (!resize_one_check(o, count, cause))
				return (false);
		}
		wl_list_for_each(o, &cow_wm.outputs, wm_link)
		    resize_one_apply(o, count);
		return (true);
	}

	if (!resize_one_check(output, count, cause))
		return (false);
	resize_one_apply(output, count);
	return (true);
}

bool
cow_desk_sets_resize_all(uint8_t count, char **cause)
{
	if (count == 0) {
		xasprintf(cause, "desks.count: must be > 0");
		return (false);
	}
	if (count > COW_MAX_DESK_COUNT) {
		xasprintf(cause, "desks.count: max %d", COW_MAX_DESK_COUNT);
		return (false);
	}

	struct cow_output *o;
	wl_list_for_each(o, &cow_wm.outputs, wm_link)
	{
		if (!resize_one_check(o, count, cause))
			return (false);
	}
	wl_list_for_each(o, &cow_wm.outputs, wm_link)
	    resize_one_apply(o, count);
	return (true);
}
