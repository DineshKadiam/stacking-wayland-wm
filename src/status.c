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
#include "cow/status.h"

#include <cJSON.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "cow/memory.h"
#include "cow/output.h"
#include "cow/util.h"
#include "cow/view.h"
#include "cow/wm.h"

#define MAX_STATUS_CLIENTS 64

static int    server_fd	  = -1;
static char  *socket_path = NULL;
static int    clients[MAX_STATUS_CLIENTS];
static int    nclients		= 0;
static char  *last_snapshot	= NULL;
static size_t last_snapshot_len = 0;

void
cow_status_init(void)
{
	char path[108]; /* sockaddr_un.sun_path size */
	int  fd = cow_unix_listen("cow-status.sock", path, sizeof(path),
	     /* backlog */ 4);
	if (fd < 0)
		return;

	server_fd   = fd;
	socket_path = xstrdup(path);
	fprintf(stderr, "cow: status: listening on %s\n", socket_path);
}

void
cow_status_fini(void)
{
	for (int i = 0; i < nclients; i++)
		close(clients[i]);
	nclients = 0;

	xfree(last_snapshot);
	last_snapshot	  = NULL;
	last_snapshot_len = 0;

	if (server_fd >= 0) {
		close(server_fd);
		server_fd = -1;
	}
	if (socket_path != NULL) {
		unlink(socket_path);
		xfree(socket_path);
		socket_path = NULL;
	}
}

/*
 * Accept any pending connections.  Called before emitting.
 */
static void
status_accept(void)
{
	if (server_fd < 0)
		return;

	for (;;) {
		int fd = accept4(server_fd, NULL, NULL,
		    SOCK_NONBLOCK | SOCK_CLOEXEC);
		if (fd < 0)
			break;
		if (nclients >= MAX_STATUS_CLIENTS) {
			close(fd);
			break;
		}
		/* Send the last snapshot immediately so the client does not
		 * have to wait for the next state change to get initial data.
		 * If the write fails (e.g. client went away between accept
		 * and this call), drop the fd.
		 */
		if (last_snapshot != NULL) {
			if (cow_write_all(fd, last_snapshot,
				last_snapshot_len) < 0) {
				close(fd);
				continue;
			}
		}
		clients[nclients++] = fd;
	}
}

/*
 * Send a string to all connected status clients.
 * Remove clients that disconnect or that cannot accept a short write;
 * the status protocol is line-oriented and a partial write corrupts
 * the stream, so better to drop the client than desync.
 */
static void
status_broadcast(const char *msg, size_t len)
{
	int i = 0;
	while (i < nclients) {
		ssize_t n = write(clients[i], msg, len);
		if (n < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK ||
			    errno == EINTR) {
				/* Socket full; skip this tick. */
				i++;
				continue;
			}
			/* Client disconnected */
			close(clients[i]);
			clients[i] = clients[--nclients];
			continue;
		}
		if ((size_t)n < len) {
			/* Partial write; stream is desynced -- drop. */
			close(clients[i]);
			clients[i] = clients[--nclients];
			continue;
		}
		i++;
	}
}

void
cow_status_emit(void)
{
	if (server_fd < 0)
		return;

	/* Accept new clients first */
	status_accept();

	if (nclients == 0)
		return;

	cJSON *root = cJSON_CreateObject();

	/* Per-output desk state */
	cJSON		  *outputs = cJSON_AddArrayToObject(root, "outputs");
	struct cow_output *output;
	wl_list_for_each(output, &cow_wm.outputs, wm_link)
	{
		cJSON *out = cJSON_CreateObject();
		cJSON_AddStringToObject(out, "name",
		    output->name ? output->name : "");
		cJSON_AddNumberToObject(out, "number", output->number);
		cJSON_AddNumberToObject(out, "current_desk",
		    output->current_desk->nr);

		cJSON *desks = cJSON_AddArrayToObject(out, "desks");
		bool global  = cow_config->desktop_config == COW_DESKTOP_GLOBAL;
		for (int i = 0; i < COW_NR_OF_DESKS; i++) {
			int		 count = 0;
			struct cow_view *v;
			wl_list_for_each(v, &cow_wm.views, wm_link)
			{
				if (v->closed)
					continue;
				if (!global && v->output != output)
					continue;
				if (v->desk != NULL && v->desk->nr == i)
					count++;
			}
			cJSON *d = cJSON_CreateObject();
			cJSON_AddNumberToObject(d, "nr", i);
			cJSON_AddNumberToObject(d, "windows", count);
			cJSON_AddBoolToObject(d, "collected",
			    (output->collected_desks & (1u << i)) != 0);
			cJSON_AddBoolToObject(d, "active",
			    i == (int)output->current_desk->nr);
			cJSON_AddItemToArray(desks, d);
		}

		/* Page grid state */
		int pw	  = output->usable_area.width;
		int ph	  = output->usable_area.height;
		int pcols = cow_config ? cow_config->page_cols :
					 COW_CFG_DEFAULT_PAGE_COLS;
		int prows = cow_config ? cow_config->page_rows :
					 COW_CFG_DEFAULT_PAGE_ROWS;
		int pcol  = (pw > 0 && pcols > 1) ?
		     output->current_desk->page_vx / pw :
		     0;
		int prow  = (ph > 0 && prows > 1) ?
		     output->current_desk->page_vy / ph :
		     0;
		cJSON_AddNumberToObject(out, "page_cols", pcols);
		cJSON_AddNumberToObject(out, "page_rows", prows);
		cJSON_AddNumberToObject(out, "page_col", pcol);
		cJSON_AddNumberToObject(out, "page_row", prow);
		/*
		 * Raw sub-page viewport offsets in desk pixel space.
		 * page_col / page_row are truncated to the page index
		 * the viewport's top-left sits on -- but with the
		 * `scroll` command (or an interactive scroll-drag) the
		 * viewport can sit at an arbitrary offset between two
		 * pages.  Emit the raw pixels too so cowpager can draw
		 * the viewport's true position rather than snapping to
		 * a whole-page cell.
		 */
		cJSON_AddNumberToObject(out, "page_vx",
		    output->current_desk->page_vx);
		cJSON_AddNumberToObject(out, "page_vy",
		    output->current_desk->page_vy);
		/* Usable area for cowpager window scaling */
		cJSON_AddNumberToObject(out, "output_w", pw);
		cJSON_AddNumberToObject(out, "output_h", ph);

		cJSON_AddItemToArray(outputs, out);
	}

	struct cow_view *focused = cow_wm.focused_view;
	if (focused != NULL && !focused->closed) {
		cJSON *fw = cJSON_CreateObject();
		cow_view_json_add_identity(fw, focused);
		cow_view_json_add_placement(fw, focused);
		cJSON_AddItemToObject(root, "focused", fw);
	} else {
		cJSON_AddNullToObject(root, "focused");
	}

	int		 total = 0;
	struct cow_view *v;
	cJSON *window_ids = cJSON_AddArrayToObject(root, "window_ids");
	wl_list_for_each(v, &cow_wm.views, wm_link)
	{
		if (!v->closed) {
			total++;
			if (v->identifier != NULL)
				cJSON_AddItemToArray(window_ids,
				    cJSON_CreateString(v->identifier));
		}
	}
	cJSON_AddNumberToObject(root, "total_windows", total);
	cJSON_AddStringToObject(root, "desktop_configuration",
	    (cow_config->desktop_config == COW_DESKTOP_GLOBAL) ? "global" :
		(cow_config->desktop_config == COW_DESKTOP_PER_OUTPUT) ?
								 "per-output" :
								 "Shared");

	char *s = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);

	if (s != NULL) {
		size_t len = strlen(s);
		/* Append newline for line-based reading */
		char *msg = xmalloc(len + 2);
		memcpy(msg, s, len);
		msg[len]     = '\n';
		msg[len + 1] = '\0';
		cJSON_free(s);

		/* Cache for clients that connect between emits */
		xfree(last_snapshot);
		last_snapshot	  = xstrdup(msg);
		last_snapshot_len = len + 1;

		status_broadcast(msg, len + 1);
		xfree(msg);
	}
}
