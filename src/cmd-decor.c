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
#include "cow/ipc.h"
#include "cow/memory.h"
#include "cow/view.h"
#include "cow/style.h"

#include "decor-profile.h"
#include "state-value.h"

struct decor_entry {
	char	       *key;
	char	       *value;
	bool		inherited;
	struct wl_list link;
};

struct decor_profile {
	char	       *name;
	struct wl_list entries;
	struct wl_list link;
};

static struct wl_list decor_profiles;
static bool	      decor_profiles_init;
static char	     *runtime_active_profile;

static enum cmd_retval cmd_decor_exec(struct cmd *, struct cmd_q *);
static enum cmd_retval decor_define(const char *, const char *, const char *,
    struct cmd_q *);
static enum cmd_retval decor_apply(const char *, struct cmd_q *);
static bool	      decor_apply_resets(const char *, struct decor_profile *,
    struct cmd_q *);
static bool	      decor_profile_resets(struct decor_profile *);

struct cmd_entry cmd_decor_entry = {
	"decor",
	"d:a:t:",
	0,
	-1,
	"decor -d name key value\n"
	"decor -a name [-t target]",
	cmd_decor_exec,
	CMD_CONFIG,
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

static struct state_value *
decor_profile_snapshot(const struct decor_profile *profile)
{
	struct state_value *record = state_value_record();
	struct state_value *name = state_value_string(profile->name);
	struct state_value *active = state_value_bool(
	    runtime_active_profile != NULL &&
	    strcmp(runtime_active_profile, profile->name) == 0);
	struct state_value *settings = state_value_record();

	state_record_set(record, "name", name);
	state_record_set(record, "active", active);
	struct decor_entry *entry;
	wl_list_for_each(entry, &profile->entries, link)
	{
		struct state_value *value = state_value_string(entry->value);
		state_record_set(settings, entry->key, value);
		state_value_unref(value);
	}
	state_record_set(record, "settings", settings);
	state_value_unref(settings);
	state_value_unref(active);
	state_value_unref(name);
	return (record);
}

struct state_value *
cow_decor_snapshot_all(void)
{
	struct state_value *list = state_value_list();
	struct decor_profile *profile;

	decor_ensure_init();
	wl_list_for_each(profile, &decor_profiles, link)
	{
		struct state_value *record = decor_profile_snapshot(profile);
		state_list_append(list, record);
		state_value_unref(record);
	}
	return (list);
}

struct state_value *
cow_decor_snapshot_named(const char *name)
{
	struct decor_profile *profile = decor_find(name);

	return (profile != NULL ? decor_profile_snapshot(profile) : NULL);
}

void
cow_decor_reset_profiles(void)
{
	struct decor_profile *profile, *profile_tmp;

	decor_ensure_init();
	wl_list_for_each_safe(profile, profile_tmp, &decor_profiles, link)
	{
		struct decor_entry *entry, *entry_tmp;

		wl_list_for_each_safe(entry, entry_tmp, &profile->entries, link)
		{
			wl_list_remove(&entry->link);
			xfree(entry->key);
			xfree(entry->value);
			xfree(entry);
		}
		wl_list_remove(&profile->link);
		xfree(profile->name);
		xfree(profile);
	}
	wl_list_init(&decor_profiles);
	xfree(runtime_active_profile);
	runtime_active_profile = NULL;
}

static void
decor_override_defaults(struct decor_profile *profile, const char *key,
    const char *value)
{
	static const struct {
		const char *colour, *effect;
		bool	    copy_colour;
	} dependencies[] = {
		{ "border.active",		   "border.active.gradient",	     false },
		{ "border.inactive",		     "border.inactive.gradient",	 false },
		{ "titlebar.active_colour",	    "titlebar.active_gradient",	false },
		{ "titlebar.inactive_colour",      "titlebar.inactive_gradient",
		    false								      },
		{ "titlebar.active_colour",	    "container.tab.active_gradient",
		    false								      },
		{ "titlebar.inactive_colour",      "container.tab.inactive_gradient",
		    false								      },
		{ "titlebar.active_colour",	    "container.tab.active_colour",
		    true								       },
		{ "titlebar.inactive_colour",      "container.tab.inactive_colour",
		    true								       },
		{ "titlebar.fg_active",		"container.tab.fg_active",	   true	},
		{ "titlebar.fg_inactive",	  "container.tab.fg_inactive",       true  },
		{ "container.tab.active_colour",
		    "container.tab.active_gradient",				     false },
		{ "container.tab.inactive_colour",
		    "container.tab.inactive_gradient",				       false },
	};
	if (cow_ipc.loading_defaults)
		return;
	for (size_t i = 0; i < nitems(dependencies); i++) {
		if (strcmp(key, dependencies[i].colour) != 0)
			continue;
		struct decor_entry *entry;
		wl_list_for_each(entry, &profile->entries, link)
		{
			if (!entry->inherited ||
			    strcmp(entry->key, dependencies[i].effect))
				continue;
			const char *replacement = dependencies[i].copy_colour ?
			    value :
			    "none";
			xstrset(&entry->value, replacement);
			char *argv[]  = { "decor", "-d", profile->name,
				 entry->key, entry->value };
			char *command = cmd_argv_to_command_string(5, argv, 0);
			cow_config_command_add(cow_config, command);
			xfree(command);
		}
	}
}

static enum cmd_retval
decor_define(const char *name, const char *key, const char *value,
    struct cmd_q *cmdq)
{
	if (!cmd_set_is_decor_key(key)) {
		cmdq_error(cmdq, "decor: unknown key: %s", key);
		return (CMD_RETURN_ERROR);
	}
	char *stored = cmd_set_normalize_decor_value(key, value, cmdq);
	if (stored == NULL)
		return (CMD_RETURN_ERROR);

	struct decor_profile *profile = decor_profile_get(name);
	decor_override_defaults(profile, key, stored);

	struct decor_entry *entry;
	wl_list_for_each(entry, &profile->entries, link)
	{
		if (strcmp(entry->key, key) == 0) {
			xfree(entry->value);
			entry->value = stored;
			entry->inherited = cow_ipc.loading_defaults;
			cow_style_apply_all_live();
			return (CMD_RETURN_NORMAL);
		}
	}

	entry = xcalloc(1, sizeof(*entry));
	entry->key = xstrdup(key);
	entry->value = stored;
	entry->inherited = cow_ipc.loading_defaults;
	wl_list_insert(profile->entries.prev, &entry->link);
	cow_style_apply_all_live();
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

	if (decor_apply_resets(name, profile, cmdq))
		cmd_set_reset_decor_target(cmdq);

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

bool
cow_decor_apply_named(const char *name, struct cmd_q *cmdq)
{
	return (decor_apply(name, cmdq) == CMD_RETURN_NORMAL);
}

static bool
decor_apply_resets(const char *name, struct decor_profile *profile,
    struct cmd_q *cmdq)
{
	if (!decor_profile_resets(profile))
		return (false);
	if (cmdq != NULL && cmdq->in_rule)
		return (false);
	if (cmdq != NULL && cmdq->target.window != NULL)
		return (false);
	if (cmdq != NULL && !cmdq->in_rule && cmdq->target.window == NULL &&
	    runtime_active_profile != NULL &&
	    strcmp(runtime_active_profile, name) == 0)
		return (false);
	return (true);
}

static bool
decor_profile_resets(struct decor_profile *profile)
{
	struct decor_entry *entry;

	wl_list_for_each(entry, &profile->entries, link)
	{
		if (strncmp(entry->key, "icon.", 5) != 0)
			return (true);
	}
	return (false);
}

void
cow_decor_apply_runtime_active(struct cow_view *view)
{
	if (runtime_active_profile == NULL || view == NULL || view->closed)
		return;
	if (decor_find(runtime_active_profile) == NULL)
		return;

	cow_view_cfg_apply_global(&view->cfg);
	view->dec.dirty = true;
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
	enum cmd_retval ret = decor_apply(app, cmdq);
	if (ret == CMD_RETURN_NORMAL && cmdq != NULL &&
	    !cmdq->in_rule && cmdq->target.window == NULL) {
		xstrset(&runtime_active_profile, app);
	}
	if (ret == CMD_RETURN_NORMAL && (cmdq == NULL || !cmdq->in_style))
		cow_style_apply_all_live();
	return (ret);
}
