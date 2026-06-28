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

#include <wayland-client.h>

#include "cow/cmd.h"
#include "cow/compat.h"
#include "cow/memory.h"

struct decor_entry {
	char	       *key;
	char	       *value;
	struct wl_list link;
};

struct decor_profile {
	char	       *name;
	struct wl_list entries;
	struct wl_list link;
};

static struct wl_list decor_profiles;
static bool	      decor_profiles_init;

static enum cmd_retval cmd_decor_exec(struct cmd *, struct cmd_q *);
static enum cmd_retval decor_define(const char *, const char *, const char *,
    struct cmd_q *);
static enum cmd_retval decor_apply(const char *, struct cmd_q *);

struct cmd_entry cmd_decor_entry = {
	"decor",
	"d:a:t:",
	0,
	-1,
	"decor -d name key value\n"
	"decor -a name [-t target]",
	cmd_decor_exec,
	0,
};

static void
decor_ensure_init(void)
{
	if (decor_profiles_init)
		return;
	wl_list_init(&decor_profiles);
	decor_profiles_init = true;
}

static struct decor_profile *
decor_find(const char *name)
{
	struct decor_profile *profile;

	decor_ensure_init();
	wl_list_for_each(profile, &decor_profiles, link)
	{
		if (strcmp(profile->name, name) == 0)
			return profile;
	}
	return NULL;
}

static struct decor_profile *
decor_profile_get(const char *name)
{
	struct decor_profile *profile = decor_find(name);
	if (profile != NULL)
		return profile;

	profile = xcalloc(1, sizeof(*profile));
	profile->name = xstrdup(name);
	wl_list_init(&profile->entries);
	wl_list_insert(decor_profiles.prev, &profile->link);
	return profile;
}

static enum cmd_retval
decor_define(const char *name, const char *key, const char *value,
    struct cmd_q *cmdq)
{
	if (!cmd_set_is_decor_key(key)) {
		cmdq_error(cmdq, "decor: unknown key: %s", key);
		return (CMD_RETURN_ERROR);
	}

	struct decor_profile *profile = decor_profile_get(name);

	struct decor_entry *entry;
	wl_list_for_each(entry, &profile->entries, link)
	{
		if (strcmp(entry->key, key) == 0) {
			xfree(entry->value);
			entry->value = xstrdup(value);
			return (CMD_RETURN_NORMAL);
		}
	}

	entry = xcalloc(1, sizeof(*entry));
	entry->key = xstrdup(key);
	entry->value = xstrdup(value);
	wl_list_insert(profile->entries.prev, &entry->link);
	return (CMD_RETURN_NORMAL);
}

static enum cmd_retval
decor_apply(const char *name, struct cmd_q *cmdq)
{
	struct decor_profile *profile = decor_find(name);
	if (profile == NULL) {
		cmdq_error(cmdq, "decor: unknown profile: %s", name);
		return (CMD_RETURN_ERROR);
	}

	struct decor_entry *entry;
	wl_list_for_each(entry, &profile->entries, link)
	{
		enum cmd_retval ret = cmd_set_apply_decor(entry->key,
		    entry->value, cmdq);
		if (ret != CMD_RETURN_NORMAL)
			return ret;
	}
	return (CMD_RETURN_NORMAL);
}

static enum cmd_retval
cmd_decor_exec(struct cmd *self, struct cmd_q *cmdq)
{
	struct args *args = self->args;
	const char  *def = args_get(args, 'd');
	const char  *app = args_get(args, 'a');

	if ((def == NULL && app == NULL) || (def != NULL && app != NULL)) {
		cmdq_error(cmdq, "usage: %s", self->entry->usage);
		return (CMD_RETURN_ERROR);
	}
	if (def != NULL) {
		if (args->argc < 2) {
			cmdq_error(cmdq, "usage: decor -d name key value");
			return (CMD_RETURN_ERROR);
		}
		char *value = cmd_argv_to_string(args->argc, args->argv, 1);
		enum cmd_retval ret = decor_define(def, args->argv[0], value,
		    cmdq);
		xfree(value);
		return (ret);
	}
	if (args->argc != 0) {
		cmdq_error(cmdq, "usage: decor -a name [-t target]");
		return (CMD_RETURN_ERROR);
	}
	return (decor_apply(app, cmdq));
}
