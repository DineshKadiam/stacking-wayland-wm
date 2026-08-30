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
 * Command: mouse -- add a pointer/mouse binding.
 *
 * usage: mouse [contexts:]key action
 */

#include <stdlib.h>
#include <string.h>

#include "cow/cmd.h"
#include "cow/config.h"
#include "cow/keybinding.h"
#include "cow/memory.h"
#include "cow/wm.h"

static enum cmd_retval cmd_mouse_exec(struct cmd *, struct cmd_q *);

struct cmd_entry cmd_mouse_entry = {
	"mouse",
	"",
	2,
	-1,
	"mouse [contexts:]key action ...",
	cmd_mouse_exec,
	CMD_CONFIG,
};

static enum cmd_retval
add_mouse_binding(struct cmd *self)
{
	struct args *args = self->args;
	struct wl_list *bindings = &cow_config->mouse_bindings;

	struct cow_mouse_binding_entry *existing, *tmp;
	wl_list_for_each_safe(existing, tmp, bindings, link)
	{
		if (strcmp(existing->key_string, args->argv[0]) == 0) {
			wl_list_remove(&existing->link);
			xfree(existing->key_string);
			xfree(existing->action_string);
			xfree(existing);
		}
	}

	struct cow_mouse_binding_entry *entry = xcalloc(1, sizeof(*entry));
	entry->key_string		      = xstrdup(args->argv[0]);
	entry->action_string =
	    cmd_argv_to_action_string(args->argc, args->argv, 1);
	wl_list_insert(bindings->prev, &entry->link);

	cow_rebind_all_seats();

	return (CMD_RETURN_NORMAL);
}

static enum cmd_retval
cmd_mouse_exec(struct cmd *self, struct cmd_q *cmdq)
{
	uint32_t context;
	const char *mouse, *error;
	if (!cow_binding_parse_context(self->args->argv[0], &context,
		&mouse, &error)) {
		cmdq_error(cmdq, "mouse: invalid button specification %s: %s",
		    self->args->argv[0],
		    error != NULL ? error : "invalid context");
		return (CMD_RETURN_ERROR);
	}
	(void)context;
	(void)mouse;
	return add_mouse_binding(self);
}
