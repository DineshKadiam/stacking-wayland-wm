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
#include <string.h>

#include "cow/memory.h"
#include "cow/output.h"

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
	for (int i = 0; i < COW_DEFAULT_DESK_COUNT; i++)
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
