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
#include "cow/log.h"

#include <cJSON.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <event2/event.h>

#include "cow/compat.h"
#include "cow/config.h"
#include "cow/decoration.h"
#include "cow/desk_ops.h"
#include "cow/event.h"
#include "cow/keyboard_layout.h"
#include "cow/memory.h"
#include "cow/module_protocol.h"
#include "cow/output.h"
#include "cow/titlebar.h"
#include "cow/util.h"
#include "cow/view.h"
#include "cow/wm.h"

#define MAX_STATUS_CLIENTS 64

struct status_client {
	int fd;
	bool active;
	bool identified;
	char name[COW_MODULE_CLIENT_NAME_MAX + 1];
	char hello[sizeof(COW_MODULE_HELLO) + COW_MODULE_CLIENT_NAME_MAX + 1];
	size_t hello_len;
	struct event *ev;
};

static int    server_fd	  = -1;
static struct event *server_ev = NULL;
static char  *socket_path = NULL;
static struct status_client clients[MAX_STATUS_CLIENTS];
static int    nclients		= 0;
static char  *last_snapshot	= NULL;
static size_t last_snapshot_len = 0;
static bool   status_pending	= false;

static void status_accept_cb(evutil_socket_t, short, void *);
static void status_client_cb(evutil_socket_t, short, void *);

static void
status_remove_client(struct status_client *client, const char *reason)
{
	if (client == NULL || !client->active)
		return;
	int fd = client->fd;
	if (client->ev != NULL) {
		event_free(client->ev);
		client->ev = NULL;
	}
	close(fd);
	client->fd = -1;
	client->active = false;
	if (nclients > 0)
		nclients--;
	cow_log_debug("status client disconnected "
	    "(name=%s fd=%d clients=%d reason=%s)",
	    client->identified ? client->name : "unidentified", fd, nclients,
	    reason);
	client->identified = false;
	client->name[0] = '\0';
	client->hello_len = 0;
}

void
cow_status_init(void)
{
	char path[108]; /* sockaddr_un.sun_path size */
	int  fd = cow_unix_listen("status", path, sizeof(path),
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
	cow_log_debug("status socket listening on %s", socket_path);
}

void
cow_status_fini(void)
{
	for (int i = 0; i < MAX_STATUS_CLIENTS; i++)
		if (clients[i].active)
			status_remove_client(&clients[i], "shutdown");

	xfree(last_snapshot);
	last_snapshot	  = NULL;
	last_snapshot_len = 0;
	status_pending	  = false;

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
		struct status_client *client = NULL;
		for (int i = 0; i < MAX_STATUS_CLIENTS; i++) {
			if (!clients[i].active) {
				client = &clients[i];
				break;
			}
		}
		if (client == NULL) {
			close(fd);
			break;
		}
		client->fd = fd;
		client->active = true;
		client->identified = false;
		client->name[0] = '\0';
		client->hello_len = 0;
		client->ev = event_new(cow_event_base(), fd,
		    EV_READ | EV_PERSIST, status_client_cb, client);
		nclients++;
		if (client->ev == NULL || event_add(client->ev, NULL) < 0) {
			status_remove_client(client, "event-error");
			continue;
		}
		cow_log_trace("status transport connected (fd=%d clients=%d)",
		    fd, nclients);
	}
}

static void
status_client_cb(__unused evutil_socket_t fd, short events, void *data)
{
	struct status_client *client = data;
	if ((events & EV_READ) == 0 || client == NULL || !client->active)
		return;

	if (client->identified) {
		char byte;
		ssize_t n = read(client->fd, &byte, sizeof(byte));
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK ||
			errno == EINTR))
			return;
		status_remove_client(client, n == 0 ? "closed" : "protocol-error");
		return;
	}

	size_t available = sizeof(client->hello) - client->hello_len - 1;
	if (available == 0) {
		status_remove_client(client, "invalid-identification");
		return;
	}
	ssize_t n = read(client->fd, client->hello + client->hello_len,
	    available);
	if (n <= 0) {
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK ||
			errno == EINTR))
			return;
		status_remove_client(client, n == 0 ? "closed" : "read-error");
		return;
	}
	client->hello_len += (size_t)n;
	client->hello[client->hello_len] = '\0';

	char *newline = strchr(client->hello, '\n');
	if (newline == NULL)
		return;
	if (newline[1] != '\0') {
		status_remove_client(client, "protocol-error");
		return;
	}
	*newline = '\0';
	if (!cow_module_parse_hello(client->hello, client->name,
		sizeof(client->name))) {
		status_remove_client(client, "invalid-identification");
		return;
	}
	client->identified = true;
	cow_log_debug("status client identified (name=%s fd=%d clients=%d)",
	    client->name, client->fd, nclients);

	/* Send the current state only after the mandatory handshake. */
	if (last_snapshot != NULL && cow_write_all(client->fd, last_snapshot,
		last_snapshot_len) < 0)
		status_remove_client(client, "initial-write-error");
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
	while (i < MAX_STATUS_CLIENTS) {
		struct status_client *client = &clients[i++];
		if (!client->active || !client->identified)
			continue;
		ssize_t n = write(client->fd, msg, len);
		if (n < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK ||
			    errno == EINTR) {
				/* Socket full; skip this tick. */
				continue;
			}
			/* Client disconnected */
			status_remove_client(client, "write-error");
			continue;
		}
		if ((size_t)n < len) {
			/* Partial write; stream is desynced -- drop. */
			status_remove_client(client, "partial-write");
			continue;
		}
	}
}

static void
status_add_colour(cJSON *obj, const char *name, const float colour[4])
{
	uint8_t r = (uint8_t)(colour[0] * 255.0f + 0.5f);
	uint8_t g = (uint8_t)(colour[1] * 255.0f + 0.5f);
	uint8_t b = (uint8_t)(colour[2] * 255.0f + 0.5f);
	uint8_t a = (uint8_t)(colour[3] * 255.0f + 0.5f);
	char	buf[16];

	if (a == 0xff)
		snprintf(buf, sizeof(buf), "0x%02X%02X%02X", r, g, b);
	else
		snprintf(buf, sizeof(buf), "0x%02X%02X%02X%02X", r, g, b,
		    a);
	cJSON_AddStringToObject(obj, name, buf);
}

static void
status_add_theme(cJSON *root)
{
	if (cow_config == NULL)
		return;

	cJSON *theme = cJSON_AddObjectToObject(root, "theme");
	if (theme == NULL)
		return;

	if (cow_config->has_output_bg_colour) {
		status_add_colour(theme, "background",
		    cow_config->output_bg_colour);
	} else if (cow_wm.current_output != NULL &&
	    cow_wm.current_output->has_bg_colour) {
		status_add_colour(theme, "background",
		    cow_wm.current_output->bg_colour);
	}
	if (cow_config->has_output_bg_image &&
	    cow_config->output_bg_image != NULL)
		cJSON_AddStringToObject(theme, "background_image",
		    cow_config->output_bg_image);
	else if (cow_wm.current_output != NULL &&
	    cow_wm.current_output->has_bg_image &&
	    cow_wm.current_output->bg_image != NULL)
		cJSON_AddStringToObject(theme, "background_image",
		    cow_wm.current_output->bg_image);

	const float *active_bg = cow_config->container.has_tab_active ?
	    cow_config->container.tab_active :
	    cow_config->titlebar.active_colour;
	const float *inactive_bg = cow_config->container.has_tab_inactive ?
	    cow_config->container.tab_inactive :
	    cow_config->titlebar.inactive_colour;
	const float *active_fg = cow_config->container.has_tab_fg_active ?
	    cow_config->container.tab_fg_active :
	    cow_config->titlebar.fg_active;
	const float *inactive_fg = cow_config->container.has_tab_fg_inactive ?
	    cow_config->container.tab_fg_inactive :
	    cow_config->titlebar.fg_inactive;

	status_add_colour(theme, "active_bg", active_bg);
	status_add_colour(theme, "inactive_bg", inactive_bg);
	status_add_colour(theme, "active_fg", active_fg);
	status_add_colour(theme, "inactive_fg", inactive_fg);

	cJSON *titlebar = cJSON_AddObjectToObject(theme, "titlebar");
	if (titlebar == NULL)
		return;
	status_add_colour(titlebar, "active_bg",
	    cow_config->titlebar.active_colour);
	status_add_colour(titlebar, "inactive_bg",
	    cow_config->titlebar.inactive_colour);
	status_add_colour(titlebar, "active_fg",
	    cow_config->titlebar.fg_active);
	status_add_colour(titlebar, "inactive_fg",
	    cow_config->titlebar.fg_inactive);
	const char *font = cow_config->font_name != NULL ?
	    cow_config->font_name : "monospace 10";
	cJSON_AddStringToObject(titlebar, "font", font);

	int height = cow_config->titlebar.height;
	if (height == 0) {
		height = cow_titlebar_compute_thickness(
		    cow_decoration_titlebar_font());
	}
	cJSON_AddNumberToObject(titlebar, "height", height);
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

	if (output->has_bg_colour) {
		status_add_colour(out, "background", output->bg_colour);
	} else if (cow_config != NULL && cow_config->has_output_bg_colour) {
		status_add_colour(out, "background",
		    cow_config->output_bg_colour);
	}
	if (output->has_bg_image) {
		if (output->bg_image != NULL)
			cJSON_AddStringToObject(out, "background_image",
			    output->bg_image);
	} else if (cow_config != NULL && cow_config->has_output_bg_image &&
	    cow_config->output_bg_image != NULL) {
		cJSON_AddStringToObject(out, "background_image",
		    cow_config->output_bg_image);
	}
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
	int pw	  = cow_page_width(output);
	int ph	  = cow_page_height(output);
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

static void
status_emit(void)
{
	if (server_fd < 0)
		return;

	cJSON *root = cJSON_CreateObject();
	status_add_theme(root);
	cow_keyboard_layout_add_status(root);

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
		xstrset(&last_snapshot, msg);
		last_snapshot_len = len + 1;

		status_broadcast(msg, len + 1);
		xfree(msg);
	}
}

void
cow_status_request(void)
{
	if (server_fd >= 0)
		status_pending = true;
}

void
cow_status_flush(void)
{
	if (!status_pending)
		return;
	status_pending = false;
	status_emit();
}
