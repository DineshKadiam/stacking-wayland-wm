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
/*
 * Command: bind -- add a keyboard binding.
 *
 * usage: bind [-r] [-x release-action] [contexts:]key[,key...] action ...
 *        bind -d pattern
 *
 *   -r  mark the binding as repeatable: while the triggering key is
 *       held the action re-fires every set repeat_interval_ms after
 *       an initial set repeat_delay_ms dwell.  Without -r the action
 *       fires once per press.
 *
 *   -x  run an action once the key and all of the binding's modifiers
 *       have been released.  Options must precede the key specification.
 *
 *   -d  delete bindings whose complete key specification matches pattern.
 */

#include <fnmatch.h>
#include <string.h>

#include "cow/cmd.h"
#include "cow/config.h"
#include "cow/keybinding.h"
#include "cow/memory.h"

static enum cmd_retval cmd_bind_exec(struct cmd *, struct cmd_q *);

static bool
binding_sequence_is_prefix(const char *prefix, const char *sequence)
{
	size_t len = strlen(prefix);

	return (strncmp(prefix, sequence, len) == 0 && sequence[len] == ',');
}

static bool
binding_sequence_validate(const char *sequence, const char **error)
{
	char *copy = xstrdup(sequence);
	char *cursor = copy;
	char *part;
	unsigned int depth = 0;
	bool valid = true;

	while ((part = strsep(&cursor, ",")) != NULL) {
		uint32_t context;
		const char *key, *context_error;

		if (*part == '\0') {
			*error = "empty key in sequence";
			valid = false;
			break;
		}
		if (depth > 0 && strchr(part, ':') != NULL) {
			*error = "only the first key may have a context";
			valid = false;
			break;
		}
		if (!cow_binding_parse_context(part, &context, &key,
			&context_error)) {
			*error = context_error != NULL ? context_error :
			    "invalid context";
			valid = false;
			break;
		}
		(void)context;
		(void)key;
		depth++;
	}
	xfree(copy);
	return (valid);
}

/*
 * The args template prefix `+` puts glibc getopt into POSIX mode so
 * option parsing stops at the first non-option argument (i.e. the
 * key spec).  Without this, getopt permutes ahead and tries to
 * interpret any `-foo` inside the action body -- e.g.
 * `bind L+q window-resize -d right` would have `-d` eaten as if it
 * were a flag of `bind`, fail with "unknown option", and silently
 * drop the binding.  Other binding-style commands (mouse) carry an
 * empty template and so bypass getopt entirely; we need a template
 * here for `-r` and the `+` keeps it from breaking the others.
 */
struct cmd_entry cmd_bind_entry = {
	"bind",
	"+drx:",
	1,
	-1,
	"bind [-r] [-x release-action] [contexts:]key[,key...] action ...\n"
	"bind -d pattern",
	cmd_bind_exec,
	0,
};

static enum cmd_retval
cmd_bind_exec(struct cmd *self, struct cmd_q *cmdq)
{
	struct args *args = self->args;

	if (args_has(args, 'd')) {
		if (args_has(args, 'r') || args_has(args, 'x') ||
		    args->argc != 1) {
			cmdq_error(cmdq, "usage: bind -d pattern");
			return (CMD_RETURN_ERROR);
		}

		struct cow_binding_entry *existing, *tmp;
		wl_list_for_each_safe(existing, tmp,
		    &cow_config->keyboard_bindings, link)
		{
			if (fnmatch(args->argv[0], existing->key_string, 0) != 0)
				continue;
			wl_list_remove(&existing->link);
			xfree(existing->key_string);
			xfree(existing->action_string);
			xfree(existing->release_action_string);
			xfree(existing);
		}
		cow_rebind_all_seats();
		return (CMD_RETURN_NORMAL);
	}

	if (args->argc < 2) {
		cmdq_error(cmdq, "usage: bind [-r] [-x release-action] "
		    "[contexts:]key[,key...] action ...");
		return (CMD_RETURN_ERROR);
	}
	const char *sequence_error = NULL;
	if (!binding_sequence_validate(args->argv[0], &sequence_error)) {
		cmdq_error(cmdq, "bind: invalid key sequence %s: %s",
		    args->argv[0], sequence_error);
		return (CMD_RETURN_ERROR);
	}
	if (!cow_binding_validate_key_sequence(args->argv[0])) {
		cmdq_error(cmdq, "bind: invalid key sequence %s", args->argv[0]);
		return (CMD_RETURN_ERROR);
	}

	uint32_t context;
	const char *key, *error;
	if (!cow_binding_parse_context(args->argv[0], &context, &key,
		&error)) {
		cmdq_error(cmdq, "bind: invalid key specification %s: %s",
		    args->argv[0], error != NULL ? error : "invalid context");
		return (CMD_RETURN_ERROR);
	}
	if ((context & COW_BIND_CONTEXT_SSD_NONBUTTON) != 0) {
		cmdq_error(cmdq,
		    "bind: SSD titlebar, frame, and corner contexts are only "
		    "supported by mouse bindings");
		return (CMD_RETURN_ERROR);
	}
	(void)key;

	struct cow_binding_entry *replace = NULL;
	struct cow_binding_entry *existing;
	wl_list_for_each(existing, &cow_config->keyboard_bindings, link)
	{
		if (strcmp(existing->key_string, args->argv[0]) == 0) {
			replace = existing;
			continue;
		}
		if (binding_sequence_is_prefix(existing->key_string,
			args->argv[0]) ||
		    binding_sequence_is_prefix(args->argv[0],
			existing->key_string)) {
			cmdq_error(cmdq,
			    "bind: a key cannot be both an action and a prefix: %s",
			    args->argv[0]);
			return (CMD_RETURN_ERROR);
		}
	}
	if (replace != NULL) {
		wl_list_remove(&replace->link);
		xfree(replace->key_string);
		xfree(replace->action_string);
		xfree(replace->release_action_string);
		xfree(replace);
	}

	struct cow_binding_entry *entry = xcalloc(1, sizeof(*entry));

	entry->key_string = xstrdup(args->argv[0]);
	if (args_has(args, 'x'))
		entry->release_action_string = xstrdup(args_get(args, 'x'));

	entry->action_string =
	    cmd_argv_to_action_string(args->argc, args->argv, 1);
	entry->repeatable = args_has(args, 'r');

	wl_list_insert(cow_config->keyboard_bindings.prev, &entry->link);

	cow_rebind_all_seats();

	return (CMD_RETURN_NORMAL);
}
