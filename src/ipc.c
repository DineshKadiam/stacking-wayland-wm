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

#include "cow/compat.h"
#include "cow/cmd.h"
#include "cow/ipc.h"
#include "cow/command.h"
#include "cow/config.h"
#include "cow/keybinding.h"
#include "cow/memory.h"
#include "cow/util.h"
#include "cow/wm.h"

struct cow_ipc cow_ipc;

bool
cow_ipc_init(void)
{
	memset(&cow_ipc, 0, sizeof(cow_ipc));
	cow_ipc.server_fd = -1;

	char path[108]; /* sockaddr_un.sun_path size */
	int  fd = cow_unix_listen("cow-cmd.sock", path, sizeof(path),
	     /* backlog */ 4);
	if (fd < 0)
		return false;

	cow_ipc.server_fd   = fd;
	cow_ipc.socket_path = xstrdup(path);
	fprintf(stderr, "cow: ipc: listening on %s\n", cow_ipc.socket_path);
	fflush(stderr);
	return true;
}

void
cow_ipc_fini(void)
{
	for (int i = 0; i < cow_ipc.num_clients; i++) {
		if (cow_ipc.clients[i].fd >= 0)
			close(cow_ipc.clients[i].fd);
	}
	cow_ipc.num_clients = 0;

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

int
cow_ipc_fill_pollfds(struct pollfd *fds)
{
	int n = 0;

	if (cow_ipc.server_fd >= 0) {
		fds[n].fd      = cow_ipc.server_fd;
		fds[n].events  = POLLIN;
		fds[n].revents = 0;
		n++;
	}

	for (int i = 0; i < cow_ipc.num_clients; i++) {
		if (cow_ipc.clients[i].fd >= 0) {
			fds[n].fd      = cow_ipc.clients[i].fd;
			fds[n].events  = POLLIN;
			fds[n].revents = 0;
			n++;
		}
	}

	return n;
}

void
cow_ipc_respond(int fd, const char *json)
{
	if (fd < 0)
		return;
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

	cmdq_free(cmdq);

	return ok;
}

bool
cow_ipc_load_config(const char *path)
{
	FILE *f = fopen(path, "r");
	if (f == NULL) {
		fprintf(stderr, "cow: cannot open config: %s\n", path);
		return false;
	}

	/* Remember the path so that 'moocow quit -r' doesn't crash. */
	char *new_path = xstrdup(path);
	xfree(cow_ipc.config_path);
	cow_ipc.config_path = new_path;

	cow_ipc.startup_mode = true;

	/* Build a command queue from all config lines.
	 * exec commands are deferred internally by cmd_exec_exec. */
	struct cmd_q *cmdq = cmdq_new();
	cmdq->client_fd	   = -1;

	size_t lineno = 0;
	size_t len;
	char  *line;
	/* Custom delimiters: escape=\, continuation=\, comment=\0.
	 * Disable fparseln's comment handling so that # inside
	 * quoted strings (e.g. '#66ba66') is preserved.
	 * cmd_string_parse handles # comments with quote awareness. */
	const char delim[3] = { '\\', '\\', '\0' };
	while ((line = fparseln(f, &len, &lineno, delim, FPARSELN_UNESCCONT)) !=
	    NULL) {
		/* Skip blank lines */
		char *p = line;
		while (*p == ' ' || *p == '\t')
			p++;
		if (*p == '\0') {
			xfree(line);
			continue;
		}

		/* Parse line into a cmd_list and append to queue */
		char		*cause = NULL;
		struct cmd_list *cmdlist;
		if (cmd_string_parse(p, &cmdlist, path, lineno, &cause) != 0) {
			if (cause != NULL) {
				char *msg;
				xasprintf(&msg, "%s:%zu: %s", path, lineno,
				    cause);
				ARRAY_ADD(&cfg_causes, msg);
				xfree(cause);
			}
			xfree(line);
			continue;
		}
		xfree(line);

		if (cmdlist == NULL)
			continue;

		cmdq_append(cmdq, cmdlist);
		cmd_list_free(cmdlist);
	}

	fclose(f);

	/* Execute all queued commands */
	cmdq_continue(cmdq);
	cmdq_free(cmdq);

	cow_ipc.startup_mode = false;

	/* Report any errors */
	if (!ARRAY_EMPTY(&cfg_causes)) {
		for (u_int i = 0; i < ARRAY_LENGTH(&cfg_causes); i++) {
			char *c = ARRAY_ITEM(&cfg_causes, i);
			fprintf(stderr, "cow: %s\n", c);
			xfree(c);
		}
		ARRAY_FREE(&cfg_causes);
	}

	fprintf(stderr, "cow: config loaded from %s (%zu lines)\n", path,
	    lineno);
	return true;
}

static void
ipc_accept_client(void)
{
	int fd = accept4(cow_ipc.server_fd, NULL, NULL,
	    SOCK_NONBLOCK | SOCK_CLOEXEC);
	if (fd < 0)
		return;

	if (cow_ipc.num_clients >= COW_IPC_MAX_CLIENTS) {
		const char *msg =
		    "{\"ok\":false,\"error\":\"too many clients\"}\n";
		write(fd, msg, strlen(msg));
		close(fd);
		return;
	}

	struct cow_ipc_client *client = &cow_ipc.clients[cow_ipc.num_clients++];
	client->fd		      = fd;
	client->buf_len		      = 0;
}

static void
ipc_remove_client(int idx)
{
	close(cow_ipc.clients[idx].fd);
	cow_ipc.clients[idx] = cow_ipc.clients[--cow_ipc.num_clients];
}

/*
 * Handle a POLLIN event on client 'idx'.  Returns true if the client
 * was removed (either because the read reached EOF/error or because
 * the dispatched command completed and we closed the connection);
 * the caller must then NOT advance past this slot, since the last
 * client was swapped into it by ipc_remove_client().
 */
static bool
ipc_handle_client_data(int idx)
{
	struct cow_ipc_client *client = &cow_ipc.clients[idx];
	int		       avail  = COW_IPC_BUF_SIZE - client->buf_len - 1;
	if (avail <= 0) {
		client->buf_len = 0;
		return false;
	}

	int n = read(client->fd, client->buf + client->buf_len, avail);
	if (n <= 0) {
		ipc_remove_client(idx);
		return true;
	}
	client->buf_len += n;
	client->buf[client->buf_len] = '\0';

	/* Process complete lines */
	char *start = client->buf;
	char *nl;
	while ((nl = strchr(start, '\n')) != NULL) {
		*nl = '\0';
		cow_ipc_dispatch(start, client->fd);
		start = nl + 1;
		/* Close after each command: moocow sends one command and
		 * reads until EOF, so closing here unblocks it. */
		ipc_remove_client(idx);
		return true;
	}

	/* Shift remaining data to front */
	int remaining = client->buf_len - (start - client->buf);
	if (remaining > 0 && start != client->buf)
		memmove(client->buf, start, remaining);
	client->buf_len = remaining;
	return false;
}

void
cow_ipc_process_events(struct pollfd *fds, int nfds)
{
	if (nfds <= 0)
		return;

	int idx = 0;

	/* First fd is the server socket */
	if (cow_ipc.server_fd >= 0 && idx < nfds) {
		if (fds[idx].revents & POLLIN)
			ipc_accept_client();
		idx++;
	}

	/*
	 * Remaining fds are clients.  Walk the original pollfd array --
	 * both ipc_handle_client_data() and the POLLHUP/POLLERR branch
	 * may call ipc_remove_client(), which swaps the last client into
	 * slot i and decrements num_clients.  When that happens we must
	 * re-examine slot i instead of advancing, but we still move on
	 * in the pollfd array since those revents were already consumed.
	 */
	for (int i = 0; i < cow_ipc.num_clients && idx < nfds; idx++) {
		short rev = fds[idx].revents;
		if (rev & POLLIN) {
			if (ipc_handle_client_data(i))
				continue; /* slot refilled; re-examine */
		} else if (rev & (POLLHUP | POLLERR)) {
			ipc_remove_client(i);
			continue; /* slot refilled; re-examine */
		}
		i++;
	}
}
