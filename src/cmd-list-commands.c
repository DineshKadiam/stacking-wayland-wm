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
 * Command: list-commands -- list all registered commands.
 *
 * usage: list-commands
 */

#include <cJSON.h>
#include <stdio.h>

#include "cow/cmd.h"
#include "cow/ipc.h"

static enum cmd_retval cmd_list_commands_exec(struct cmd *, struct cmd_q *);

struct cmd_entry cmd_list_commands_entry = {
	"list-commands",
	"",
	0,
	0,
	"list-commands",
	cmd_list_commands_exec,
};

static enum cmd_retval
cmd_list_commands_exec(__unused struct cmd *self, struct cmd_q *cmdq)
{
	int    fd  = cmdq->client_fd;
	cJSON *arr = cJSON_CreateArray();

	for (struct cmd_entry **ent = cmd_table; *ent != NULL; ent++) {
		cJSON *c = cJSON_CreateObject();
		cJSON_AddStringToObject(c, "name", (*ent)->name);
		cJSON_AddStringToObject(c, "usage", (*ent)->usage);
		cJSON_AddItemToArray(arr, c);
	}
	cow_ipc_respond_data(fd, arr);

	return (CMD_RETURN_NORMAL);
}
