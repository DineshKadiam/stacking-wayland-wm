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

#include "cow/alias.h"
#include "cow/config.h"
#include "cow/memory.h"

struct cow_alias *
cow_alias_find(const char *name)
{
	if (cow_config == NULL || name == NULL)
		return (NULL);

	struct cow_alias *alias;
	wl_list_for_each(alias, &cow_config->aliases, link)
	{
		if (strcmp(alias->name, name) == 0)
			return (alias);
	}
	return (NULL);
}

bool
cow_alias_set(const char *name, const char *command)
{
	if (cow_config == NULL || name == NULL || command == NULL)
		return (false);

	struct cow_alias *alias = cow_alias_find(name);
	if (alias == NULL) {
		alias = xcalloc(1, sizeof(*alias));
		alias->name = xstrdup(name);
		wl_list_insert(cow_config->aliases.prev, &alias->link);
	}
	xstrset(&alias->command, command);
	return (true);
}

bool
cow_alias_remove(const char *name)
{
	struct cow_alias *alias = cow_alias_find(name);
	if (alias == NULL)
		return (false);
	wl_list_remove(&alias->link);
	cow_alias_free(alias);
	return (true);
}

void
cow_alias_free(struct cow_alias *alias)
{
	xfree(alias->name);
	xfree(alias->command);
	xfree(alias->parameters);
	xfree(alias);
}

const struct wl_list *
cow_aliases_head(void)
{
	return (cow_config != NULL ? &cow_config->aliases : NULL);
}
