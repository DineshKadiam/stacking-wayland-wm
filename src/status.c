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
#include <event2/event.h>

#include "cow/compat.h"
#include "cow/config.h"
#include "cow/event.h"
#include "cow/memory.h"
#include "cow/output.h"
#include "cow/util.h"
#include "cow/view.h"
#include "cow/wm.h"

#define MAX_STATUS_CLIENTS 64

static int    server_fd	  = -1;
static struct event *server_ev = NULL;
static char  *socket_path = NULL;
static int    clients[MAX_STATUS_CLIENTS];
static int    nclients		= 0;
static char  *last_snapshot	= NULL;
static size_t last_snapshot_len = 0;

static void status_accept_cb(evutil_socket_t, short, void *);

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
	server_ev = event_new(cow_event_base(), fd, EV_READ | EV_PERSIST,
	    status_accept_cb, NULL);
	if (server_ev == NULL || event_add(server_ev, NULL) < 0) {
		cow_status_fini();
		return;
	}
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

	if (server_ev != NULL) {
		event_free(server_ev);
		server_ev = NULL;
	}

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

static void
status_accept_cb(__unused evutil_socket_t fd, __unused short events,
    __unused void *data)
{
	status_accept();
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
cow_status_add_output_geometry(cJSON *out, const struct cow_output *output)
{
	cJSON_AddStringToObject(out, "name", output->name ? output->name : "");
	cJSON_AddNumberToObject(out, "number", output->number);

	struct cow_box geom = {
		.x	= output->x,
		.y	= output->y,
		.width	= output->width,
		.height = output->height,
	};
	cow_box_json_add(out, &geom);

	cJSON *ua = cJSON_CreateObject();
	cow_box_json_add(ua, &output->usable_area);
	cJSON_AddItemToObject(out, "usable", ua);
}

void
cow_status_add_output_desks(cJSON *out, const struct cow_output *output)
{
	cJSON_AddBoolToObject(out, "current", output == cow_wm.current_output);
	cJSON_AddNumberToObject(out, "current_desk",
	    output->current_desk->nr);

	cJSON *desks = cJSON_AddArrayToObject(out, "desks");
	bool   global = cow_config->desktop_config == COW_DESKTOP_GLOBAL;
	struct cow_desk *dk;
	wl_list_for_each(dk, &output->desk_set.desks, set_link)
	{
		int		 count = 0;
		struct cow_view *view;
		wl_list_for_each(view, &cow_wm.views, wm_link)
		{
			if (view->closed)
				continue;
			if (!global && view->output != output)
				continue;
			if (view->desk == dk)
				count++;
		}

		cJSON *d = cJSON_CreateObject();
		cJSON_AddNumberToObject(d, "nr", dk->nr);
		cJSON_AddStringToObject(d, "name",
		    dk->name != NULL ? dk->name : "");
		cJSON_AddNumberToObject(d, "windows", count);
		cJSON_AddBoolToObject(d, "collected",
		    (output->collected_desks & (1u << dk->nr)) != 0);
		cJSON_AddBoolToObject(d, "active", dk == output->current_desk);
		cJSON_AddItemToArray(desks, d);
	}
}

void
cow_status_add_output_pages(cJSON *out, const struct cow_output *output)
{
	int pw	  = output->usable_area.width;
	int ph	  = output->usable_area.height;
	int pcols = cow_config ? cow_config->page_cols : COW_CFG_DEFAULT_PAGE_COLS;
	int prows = cow_config ? cow_config->page_rows : COW_CFG_DEFAULT_PAGE_ROWS;
	int pcol  = (pw > 0 && pcols > 1) ? output->current_desk->page_vx / pw :
					    0;
	int prow  = (ph > 0 && prows > 1) ? output->current_desk->page_vy / ph :
					    0;

	cJSON_AddNumberToObject(out, "page_cols", pcols);
	cJSON_AddNumberToObject(out, "page_rows", prows);
	cJSON_AddNumberToObject(out, "page_col", pcol);
	cJSON_AddNumberToObject(out, "page_row", prow);
	cJSON_AddNumberToObject(out, "page_vx", output->current_desk->page_vx);
	cJSON_AddNumberToObject(out, "page_vy", output->current_desk->page_vy);
	cJSON_AddNumberToObject(out, "output_w", pw);
	cJSON_AddNumberToObject(out, "output_h", ph);
}

void
cow_status_emit(void)
{
	if (server_fd < 0)
		return;

	cJSON *root = cJSON_CreateObject();

	/* Per-output desk state */
	cJSON		  *outputs = cJSON_AddArrayToObject(root, "outputs");
	struct cow_output *output;
	wl_list_for_each(output, &cow_wm.outputs, wm_link)
	{
		cJSON *out = cJSON_CreateObject();
		cow_status_add_output_geometry(out, output);
		cow_status_add_output_desks(out, output);
		cow_status_add_output_pages(out, output);

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
	wl_list_for_each(v, &cow_wm.views, wm_link)
	{
		if (!v->closed)
			total++;
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
