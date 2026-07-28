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

#include <cJSON.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <event2/event.h>

#include "cow/compat.h"
#include "cow/cmd.h"
#include "cow/cmd_scan.h"
#include "cow/command.h"
#include "cow/config.h"
#include "cow/event.h"
#include "cow/ipc.h"
#include "cow/keybinding.h"
#include "cow/memory.h"
#include "cow/util.h"
#include "cow/wm.h"

struct cow_ipc cow_ipc;

static void ipc_accept_cb(evutil_socket_t, short, void *);
static void ipc_client_cb(evutil_socket_t, short, void *);
static void ipc_clear_causes(void);
static bool cow_ipc_load_file(const char *, int);
static struct cow_ipc_client *ipc_find_client_fd(int);

bool
cow_ipc_init(void)
{
	memset(&cow_ipc, 0, sizeof(cow_ipc));
	cow_ipc.server_fd = -1;
	for (int i = 0; i < COW_IPC_MAX_CLIENTS; i++)
		cow_ipc.clients[i].fd = -1;

	char path[108]; /* sockaddr_un.sun_path size */
	int  fd = cow_unix_listen("cow-cmd.sock", path, sizeof(path),
	     /* backlog */ 4);
	if (fd < 0)
		return false;

	cow_ipc.server_fd   = fd;
	cow_ipc.socket_path = xstrdup(path);
	cow_ipc.server_ev   = event_new(cow_event_base(), fd,
	    EV_READ | EV_PERSIST, ipc_accept_cb, NULL);
	if (cow_ipc.server_ev == NULL || event_add(cow_ipc.server_ev, NULL) < 0) {
		cow_ipc_fini();
		return false;
	}
	fprintf(stderr, "cow: ipc: listening on %s\n", cow_ipc.socket_path);
	fflush(stderr);
	return true;
}

void
cow_ipc_fini(void)
{
	for (int i = 0; i < COW_IPC_MAX_CLIENTS; i++) {
		if (cow_ipc.clients[i].ev != NULL) {
			event_free(cow_ipc.clients[i].ev);
			cow_ipc.clients[i].ev = NULL;
		}
		if (cow_ipc.clients[i].fd >= 0) {
			close(cow_ipc.clients[i].fd);
			cow_ipc.clients[i].fd = -1;
		}
		cow_ipc.clients[i].active = false;
	}
	cow_ipc.num_clients = 0;

	if (cow_ipc.server_ev != NULL) {
		event_free(cow_ipc.server_ev);
		cow_ipc.server_ev = NULL;
	}

	if (cow_ipc.server_fd >= 0) {
		close(cow_ipc.server_fd);
		cow_ipc.server_fd = -1;
	}

	if (cow_ipc.socket_path != NULL) {
		unlink(cow_ipc.socket_path);
		xfree(cow_ipc.socket_path);
		cow_ipc.socket_path = NULL;
	}
}

static void
ipc_clear_causes(void)
{
	if (ARRAY_EMPTY(&cfg_causes))
		return;

	for (u_int i = 0; i < ARRAY_LENGTH(&cfg_causes); i++)
		xfree(ARRAY_ITEM(&cfg_causes, i));
	ARRAY_FREE(&cfg_causes);
}

void
cow_ipc_respond(int fd, const char *json)
{
	if (fd < 0)
		return;
	struct cow_ipc_client *client = ipc_find_client_fd(fd);
	if (client != NULL)
		client->responded = true;
	size_t len = strlen(json);
	if (cow_write_all(fd, json, len) < 0)
		return;
	if (len == 0 || json[len - 1] != '\n')
		(void)cow_write_all(fd, "\n", 1);
}

void
cow_ipc_respond_error(int fd, const char *msg)
{
	if (fd < 0) {
		fprintf(stderr, "cow: ipc: error: %s\n", msg);
		return;
	}
	cJSON *root = cJSON_CreateObject();
	cJSON_AddBoolToObject(root, "ok", 0);
	cJSON_AddStringToObject(root, "error", msg);
	char *s = cJSON_PrintUnformatted(root);
	cow_ipc_respond(fd, s);
	cJSON_free(s);
	cJSON_Delete(root);
}

void
cow_ipc_respond_data(int fd, cJSON *data)
{
	if (fd < 0) {
		cJSON_Delete(data);
		return;
	}
	cJSON *root = cJSON_CreateObject();
	cJSON_AddBoolToObject(root, "ok", 1);
	cJSON_AddItemToObject(root, "data", data);
	char *s = cJSON_PrintUnformatted(root);
	cow_ipc_respond(fd, s);
	cJSON_free(s);
	cJSON_Delete(root);
}

bool
cow_ipc_dispatch(const char *line, int client_fd)
{
	ipc_clear_causes();

	/* Skip leading whitespace */
	while (*line == ' ' || *line == '\t')
		line++;

	/* Skip comments and blank lines */
	if (*line == '#' || *line == '\0' || *line == '\n')
		return true;

	/* Parse through the lswm/tmux command infrastructure */
	struct cmd_list *cmdlist = NULL;
	char		*cause	 = NULL;

	if (cmd_string_parse(line, &cmdlist, NULL, 0, &cause) != 0) {
		if (cause != NULL) {
			/* Try as a bare action name before giving up */
			cow_action_fn fn;
			void	     *arg;
			if (cow_resolve_action(cause, &fn, &arg)) {
				xfree(cause);
				fn(arg);
				return true;
			}
			cow_ipc_respond_error(client_fd, cause);
			xfree(cause);
		}
		return false;
	}

	if (cmdlist == NULL)
		return true;

	/* Execute through a command queue */
	struct cmd_q *cmdq = cmdq_new();
	cmdq->client_fd	   = client_fd;
	cmdq_run(cmdq, cmdlist);
	cmd_list_free(cmdlist);

	/* Check for errors from cmdq execution */
	bool ok = true;
	if (!ARRAY_EMPTY(&cfg_causes)) {
		for (u_int i = 0; i < ARRAY_LENGTH(&cfg_causes); i++) {
			char *c = ARRAY_ITEM(&cfg_causes, i);
			cow_ipc_respond_error(client_fd, c);
			xfree(c);
			ok = false;
		}
		ARRAY_FREE(&cfg_causes);
	}
	struct cow_ipc_client *client = ipc_find_client_fd(client_fd);
	if (ok && client != NULL && !client->responded)
		cow_ipc_respond(client_fd, "{\"ok\":true}");

	cmdq_free(cmdq);

	return ok;
}

bool
cow_ipc_load_config(const char *path)
{
	cow_ipc.startup_mode = true;

	bool ok = cow_ipc_load_file(path, -1);

	cow_ipc.startup_mode = false;

	if (!ok)
		fprintf(stderr, "cow: cannot open config: %s\n", path);

	if (ok) {
		/* Remember the path so that 'moocow quit -r' doesn't crash. */
		char *new_path = xstrdup(path);
		xfree(cow_ipc.config_path);
		cow_ipc.config_path = new_path;
	}

	/* Report any errors */
	if (!ARRAY_EMPTY(&cfg_causes)) {
		for (u_int i = 0; i < ARRAY_LENGTH(&cfg_causes); i++) {
			char *c = ARRAY_ITEM(&cfg_causes, i);
			fprintf(stderr, "cow: %s\n", c);
			xfree(c);
		}
		ARRAY_FREE(&cfg_causes);
	}

	return ok;
}

bool
cow_ipc_include_config(const char *path, int client_fd)
{
	return cow_ipc_load_file(path, client_fd);
}

static bool
cow_ipc_load_file(const char *path, int client_fd)
{
	FILE *f = fopen(path, "r");
	if (f == NULL)
		return false;

	/* Build a command queue from all config lines.
	 * exec commands are deferred internally by cmd_exec_exec. */
	struct cmd_q *cmdq = cmdq_new();
	cmdq->client_fd	   = client_fd;

	size_t lineno = 0, cmd_lineno = 0;
	size_t len, cmd_len = 0, cmd_cap = 0;
	char  *line, *cmd_buf = NULL;
	/* Custom delimiters: escape=\, continuation=\, comment=\0.
	 * Disable fparseln's comment handling so that # inside
	 * quoted strings (e.g. '#66ba66') is preserved.
	 * cmd_string_parse handles # comments with quote awareness. */
	const char delim[3] = { '\\', '\\', '\0' };
	while ((line = fparseln(f, &len, &lineno, delim, FPARSELN_UNESCCONT)) !=
	    NULL) {
		char *p = line;
		if (cmd_len == 0) {
			while (*p == ' ' || *p == '\t')
				p++;
		}
		if (cmd_len == 0 && *p == '\0') {
			xfree(line);
			continue;
		}
		if (cmd_len == 0)
			cmd_lineno = lineno;

		size_t add = strlen(p);
		if (cmd_len + add + 2 > cmd_cap) {
			cmd_cap = cmd_cap ? cmd_cap : 128;
			while (cmd_len + add + 2 > cmd_cap)
				cmd_cap *= 2;
			cmd_buf = xrealloc(cmd_buf, cmd_cap, 1);
		}
		memcpy(cmd_buf + cmd_len, p, add);
		cmd_len += add;
		cmd_buf[cmd_len++] = '\n';
		cmd_buf[cmd_len]   = '\0';
		xfree(line);

		size_t end = 0;
		if (cow_cmd_find_terminator(cmd_buf, cmd_len, &end) !=
		    COW_CMD_SCAN_COMPLETE)
			continue;

		cmd_buf[end] = '\0';
		char		*cause = NULL;
		struct cmd_list *cmdlist;
		if (cmd_string_parse(cmd_buf, &cmdlist, path, cmd_lineno,
			&cause) != 0) {
			if (cause != NULL) {
				char *msg;
				xasprintf(&msg, "%s:%zu: %s", path, cmd_lineno,
				    cause);
				ARRAY_ADD(&cfg_causes, msg);
				xfree(cause);
			}
			cmd_len = 0;
			continue;
		}
		cmd_len = 0;

		if (cmdlist == NULL)
			continue;

		cmdq_append(cmdq, cmdlist);
		cmd_list_free(cmdlist);
	}
	if (cmd_len > 0) {
		char *msg;
		xasprintf(&msg, "%s:%zu: unterminated command", path,
		    cmd_lineno);
		ARRAY_ADD(&cfg_causes, msg);
	}
	xfree(cmd_buf);

	fclose(f);

	/* Execute all queued commands */
	cmdq_continue(cmdq);
	cmdq_free(cmdq);

	fprintf(stderr, "cow: config loaded from %s (%zu lines)\n", path,
	    lineno);
	return true;
}

static struct cow_ipc_client *
ipc_find_free_client(void)
{
	for (int i = 0; i < COW_IPC_MAX_CLIENTS; i++) {
		if (!cow_ipc.clients[i].active)
			return (&cow_ipc.clients[i]);
	}
	return (NULL);
}

static struct cow_ipc_client *
ipc_find_client_fd(int fd)
{
	if (fd < 0)
		return NULL;
	for (int i = 0; i < COW_IPC_MAX_CLIENTS; i++) {
		if (cow_ipc.clients[i].active && cow_ipc.clients[i].fd == fd)
			return &cow_ipc.clients[i];
	}
	return NULL;
}

static void
ipc_remove_client(struct cow_ipc_client *client)
{
	if (client == NULL || !client->active)
		return;
	if (client->ev != NULL) {
		event_free(client->ev);
		client->ev = NULL;
	}
	if (client->fd >= 0) {
		close(client->fd);
		client->fd = -1;
	}
	client->buf_len = 0;
	client->active  = false;
	if (cow_ipc.num_clients > 0)
		cow_ipc.num_clients--;
}

static void
ipc_accept_client(void)
{
	int fd = accept4(cow_ipc.server_fd, NULL, NULL,
	    SOCK_NONBLOCK | SOCK_CLOEXEC);
	if (fd < 0)
		return;

	struct cow_ipc_client *client = ipc_find_free_client();
	if (client == NULL) {
		const char *msg =
		    "{\"ok\":false,\"error\":\"too many clients\"}\n";
		write(fd, msg, strlen(msg));
		close(fd);
		return;
	}

	client->fd	 = fd;
	client->buf_len	 = 0;
	client->active	 = true;
	client->responded = false;
	client->ev	 = event_new(cow_event_base(), fd, EV_READ | EV_PERSIST,
	     ipc_client_cb, client);
	if (client->ev == NULL || event_add(client->ev, NULL) < 0) {
		ipc_remove_client(client);
		return;
	}
	cow_ipc.num_clients++;
}

static void
ipc_handle_client_data(struct cow_ipc_client *client)
{
	int avail = COW_IPC_BUF_SIZE - client->buf_len - 1;
	if (avail <= 0) {
		client->buf_len = 0;
		return;
	}

	int n = read(client->fd, client->buf + client->buf_len, avail);
	if (n <= 0) {
		ipc_remove_client(client);
		return;
	}
	client->buf_len += n;
	client->buf[client->buf_len] = '\0';

	char *start = client->buf;
	for (;;) {
		size_t end = 0;
		enum cow_cmd_scan_result r = cow_cmd_find_terminator(start,
		    client->buf_len - (start - client->buf), &end);
		if (r != COW_CMD_SCAN_COMPLETE)
			break;

		start[end] = '\0';
		cow_ipc_dispatch(start, client->fd);
		start += end + 1;
		/* Close after each command: moocow sends one command and
		 * reads until EOF, so closing here unblocks it. */
		ipc_remove_client(client);
		return;
	}

	/* Shift remaining data to front */
	int remaining = client->buf_len - (start - client->buf);
	if (remaining > 0 && start != client->buf)
		memmove(client->buf, start, remaining);
	client->buf_len = remaining;
}

static void
ipc_accept_cb(__unused evutil_socket_t fd, __unused short events,
    __unused void *data)
{
	for (;; ) {
		int before = cow_ipc.num_clients;
		ipc_accept_client();
		if (cow_ipc.num_clients == before)
			break;
	}
}

static void
ipc_client_cb(__unused evutil_socket_t fd, short events, void *data)
{
	struct cow_ipc_client *client = data;
	if ((events & EV_READ) == 0)
		return;
	ipc_handle_client_data(client);
}
