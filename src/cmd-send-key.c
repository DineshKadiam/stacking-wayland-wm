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


#include "cow/cmd.h"
#include "cow/memory.h"
#include "cow/virtual_keyboard.h"

static enum cmd_retval
cmd_send_key_exec(struct cmd *self, struct cmd_q *queue)
{
	char *error;
	if (!cow_virtual_keyboard_send_key(self->args->argv[0], &error)) {
		cmdq_error(queue, "send-key: %s", error);
		xfree(error);
		return CMD_RETURN_ERROR;
	}
	return CMD_RETURN_NORMAL;
}

struct cmd_entry cmd_send_key_entry = {
	"send-key", "", 1, 1, "[MODIFIERS+]KEY", cmd_send_key_exec, CMD_OWN_TS,
};
