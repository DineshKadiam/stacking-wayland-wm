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

#include <cairo.h>
#include <cJSON.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <unistd.h>
#include <event2/event.h>
#include <wayland-client.h>

#include "cow/compat.h"
#include "cow/module.h"
#include "xdg-shell-client-protocol.h"

#define PROG "cowbuttons"

#define MAX_CELLS 256
#define MAX_FIELDS 8
#define DEFAULT_CELL_W 96
#define DEFAULT_CELL_H 32
#define MIN_CELL_W 16
#define MIN_CELL_H 16
#define TEXT_PAD 6
#define BTN_LEFT 0x110
#define WL_SEAT_VERSION 4

enum cell_type {
	CELL_EMPTY,
	CELL_BUTTON,
	CELL_STATUS,
	CELL_SPACER,
};

enum status_type {
	STATUS_FOCUSED_TITLE,
	STATUS_FOCUSED_APP,
	STATUS_CURRENT_OUTPUT,
	STATUS_CURRENT_DESK,
	STATUS_DESK_LIST,
};

struct cell {
	enum cell_type	  type;
	enum status_type  status;
	int		  row;
	int		  col;
	char		  label[128];
	char		  command[512];
};

struct config {
	int  cell_w;
	int  cell_h;
	int  rows;
	int  cols;
	char font[128];
};

struct state {
	char focused_title[256];
	char focused_app[128];
	char current_output[128];
	char current_desk[64];
	char desk_list[512];
};

static struct wl_display    *display;
static struct wl_compositor *compositor;
static struct wl_shm	    *wl_shm;
static struct xdg_wm_base   *xdg_wm_base;
static struct wl_surface    *surface;
static struct xdg_surface   *xdg_surface;
static struct xdg_toplevel  *xdg_toplevel;
static struct wl_seat	    *seat;
static struct wl_pointer    *pointer;

static struct config cfg = {
	.cell_w = DEFAULT_CELL_W,
	.cell_h = DEFAULT_CELL_H,
	.rows = 1,
	.cols = 0,
	.font = "Sans 10",
};
static struct cell cells[MAX_CELLS];
static int	   n_cells;
static int	   configured;
static int	   running = 1;
static int	   win_w;
static int	   win_h;
static int	   pressed_cell = -1;
static double	   ptr_x;
static double	   ptr_y;
static int	   wake_pipe[2];

static pthread_mutex_t state_lock = PTHREAD_MUTEX_INITIALIZER;
static struct state    current_state;
static int	       dirty = 1;

static void render(void);

static void
usage(void)
{
	fprintf(stderr, "usage: cowbuttons [-c path] [-h]\n");
}

static int
parse_int(const char *s, int *out)
{
	char *end;
	long  n;

	if (s == NULL || *s == '\0')
		return 0;
	errno = 0;
	n = strtol(s, &end, 10);
	if (errno != 0 || *end != '\0')
		return 0;
	*out = (int)n;
	return 1;
}

static void
free_fields(char **fields, int n)
{
	for (int i = 0; i < n; i++)
		free(fields[i]);
}

static int
parse_fields(const char *line, char **fields, int max_fields, char *err,
    size_t errsz)
{
	int n = 0;
	const char *p = line;

	while (*p != '\0') {
		while (*p == ' ' || *p == '\t' || *p == '\n')
			p++;
		if (*p == '\0' || *p == '#')
			break;
		if (n == max_fields) {
			snprintf(err, errsz, "too many fields");
			goto fail;
		}

		char buf[1024];
		size_t len = 0;
		if (*p == '"') {
			p++;
			while (*p != '\0' && *p != '"') {
				if (*p == '\\' && p[1] != '\0')
					p++;
				if (len + 1 >= sizeof(buf)) {
					snprintf(err, errsz, "quoted field too long");
					goto fail;
				}
				buf[len++] = *p++;
			}
			if (*p != '"') {
				snprintf(err, errsz, "unterminated quote");
				goto fail;
			}
			p++;
		} else {
			while (*p != '\0' && *p != ' ' && *p != '\t' &&
			    *p != '\n') {
				if (*p == '#')
					break;
				if (len + 1 >= sizeof(buf)) {
					snprintf(err, errsz, "field too long");
					goto fail;
				}
				buf[len++] = *p++;
			}
		}
		buf[len] = '\0';
		fields[n] = strdup(buf);
		if (fields[n] == NULL) {
			snprintf(err, errsz, "out of memory");
			goto fail;
		}
		n++;
	}
	return n;

fail:
	free_fields(fields, n);
	return -1;
}

static int
status_type_parse(const char *s, enum status_type *out)
{
	if (strcmp(s, "focused-title") == 0)
		*out = STATUS_FOCUSED_TITLE;
	else if (strcmp(s, "focused-app") == 0)
		*out = STATUS_FOCUSED_APP;
	else if (strcmp(s, "current-output") == 0)
		*out = STATUS_CURRENT_OUTPUT;
	else if (strcmp(s, "current-desk") == 0)
		*out = STATUS_CURRENT_DESK;
	else if (strcmp(s, "desk-list") == 0)
		*out = STATUS_DESK_LIST;
	else
		return 0;
	return 1;
}

static struct cell *
cell_add(int row, int col)
{
	if (n_cells >= MAX_CELLS)
		return NULL;
	struct cell *c = &cells[n_cells++];
	memset(c, 0, sizeof(*c));
	c->row = row;
	c->col = col;
	if (row + 1 > cfg.rows)
		cfg.rows = row + 1;
	if (col + 1 > cfg.cols)
		cfg.cols = col + 1;
	return c;
}

static int
parse_cell_pos(char **fields, int n, int *row, int *col, const char **why)
{
	if (n < 3) {
		*why = "missing row/column";
		return 0;
	}
	if (!parse_int(fields[1], row) || !parse_int(fields[2], col) ||
	    *row < 0 || *col < 0) {
		*why = "bad row/column";
		return 0;
	}
	return 1;
}

static int
load_config(const char *path)
{
	FILE *f;
	char *line = NULL;
	size_t cap = 0;
	unsigned int lineno = 0;

	f = fopen(path, "r");
	if (f == NULL) {
		fprintf(stderr, "%s: %s: %s\n", PROG, path, strerror(errno));
		return 0;
	}

	while (getline(&line, &cap, f) != -1) {
		char *fields[MAX_FIELDS] = { 0 };
		char err[128] = "";
		const char *why = NULL;
		int n, v, row, col;

		lineno++;
		n = parse_fields(line, fields, MAX_FIELDS, err, sizeof(err));
		if (n < 0) {
			fprintf(stderr, "%s: %s:%u: %s\n", PROG, path, lineno,
			    err);
			continue;
		}
		if (n == 0)
			continue;

		if (strcmp(fields[0], "cell_width") == 0) {
			if (n != 2 || !parse_int(fields[1], &v) ||
			    v < MIN_CELL_W) {
				why = "bad cell_width";
				goto line_error;
			}
			cfg.cell_w = v;
		} else if (strcmp(fields[0], "cell_height") == 0) {
			if (n != 2 || !parse_int(fields[1], &v) ||
			    v < MIN_CELL_H) {
				why = "bad cell_height";
				goto line_error;
			}
			cfg.cell_h = v;
		} else if (strcmp(fields[0], "rows") == 0) {
			if (n != 2 || !parse_int(fields[1], &v) || v < 1) {
				why = "bad rows";
				goto line_error;
			}
			cfg.rows = v;
		} else if (strcmp(fields[0], "cols") == 0) {
			if (n != 2 || !parse_int(fields[1], &v) || v < 1) {
				why = "bad cols";
				goto line_error;
			}
			cfg.cols = v;
		} else if (strcmp(fields[0], "font") == 0) {
			if (n != 2) {
				why = "bad font";
				goto line_error;
			}
			strlcpy(cfg.font, fields[1], sizeof(cfg.font));
		} else if (strcmp(fields[0], "button") == 0) {
			if (n != 5 || !parse_cell_pos(fields, n, &row, &col,
			    &why))
				goto line_error;
			struct cell *c = cell_add(row, col);
			if (c == NULL) {
				why = "too many cells";
				goto line_error;
			}
			c->type = CELL_BUTTON;
			strlcpy(c->label, fields[3], sizeof(c->label));
			strlcpy(c->command, fields[4], sizeof(c->command));
		} else if (strcmp(fields[0], "status") == 0) {
			if (n != 4 || !parse_cell_pos(fields, n, &row, &col,
			    &why))
				goto line_error;
			enum status_type type;
			if (!status_type_parse(fields[3], &type)) {
				why = "bad status type";
				goto line_error;
			}
			struct cell *c = cell_add(row, col);
			if (c == NULL) {
				why = "too many cells";
				goto line_error;
			}
			c->type = CELL_STATUS;
			c->status = type;
		} else if (strcmp(fields[0], "spacer") == 0) {
			if (n != 3 || !parse_cell_pos(fields, n, &row, &col,
			    &why))
				goto line_error;
			struct cell *c = cell_add(row, col);
			if (c == NULL) {
				why = "too many cells";
				goto line_error;
			}
			c->type = CELL_SPACER;
		} else {
			why = "unknown command";
			goto line_error;
		}
		free_fields(fields, n);
		continue;

line_error:
		fprintf(stderr, "%s: %s:%u: %s\n", PROG, path, lineno, why);
		free_fields(fields, n);
	}

	free(line);
	fclose(f);
	if (cfg.cols < 1)
		cfg.cols = 1;
	if (cfg.rows < 1)
		cfg.rows = 1;
	return 1;
}

static char *
default_config_path(void)
{
	const char *xdg = getenv("XDG_CONFIG_HOME");
	const char *home;
	char *path;

	if (xdg != NULL && *xdg != '\0') {
		if (asprintf(&path, "%s/cow/cowbuttons.conf", xdg) < 0)
			return NULL;
		return path;
	}
	home = getenv("HOME");
	if (home == NULL || *home == '\0')
		return NULL;
	if (asprintf(&path, "%s/.config/cow/cowbuttons.conf", home) < 0)
		return NULL;
	return path;
}

static const struct cell *
cell_at_grid(int row, int col)
{
	for (int i = 0; i < n_cells; i++)
		if (cells[i].row == row && cells[i].col == col)
			return &cells[i];
	return NULL;
}

static int
cell_at_point(double x, double y)
{
	int col = (int)(x / cfg.cell_w);
	int row = (int)(y / cfg.cell_h);

	if (row < 0 || col < 0 || row >= cfg.rows || col >= cfg.cols)
		return -1;
	for (int i = 0; i < n_cells; i++)
		if (cells[i].row == row && cells[i].col == col)
			return i;
	return -1;
}

static const char *
status_text(const struct cell *c, const struct state *st)
{
	switch (c->status) {
	case STATUS_FOCUSED_TITLE:
		return st->focused_title;
	case STATUS_FOCUSED_APP:
		return st->focused_app;
	case STATUS_CURRENT_OUTPUT:
		return st->current_output;
	case STATUS_CURRENT_DESK:
		return st->current_desk;
	case STATUS_DESK_LIST:
		return st->desk_list;
	}
	return "";
}

static void
draw_text(cairo_t *cr, double x, double y, double max_w, const char *text)
{
	cairo_text_extents_t te;

	if (text == NULL || max_w <= 0)
		return;
	cairo_text_extents(cr, text, &te);
	if (te.width <= max_w) {
		cairo_move_to(cr, x, y);
		cairo_show_text(cr, text);
		return;
	}

	char buf[256];
	strlcpy(buf, text, sizeof(buf));
	size_t len = strlen(buf);
	while (len > 1) {
		buf[--len] = '\0';
		cairo_text_extents(cr, buf, &te);
		if (te.width + 12 <= max_w)
			break;
	}
	if (len > 1)
		strlcat(buf, "...", sizeof(buf));
	cairo_move_to(cr, x, y);
	cairo_show_text(cr, buf);
}

static void
draw_relief(cairo_t *cr, double x, double y, double w, double h, int sunken)
{
	if (sunken)
		cairo_set_source_rgb(cr, 0.18, 0.18, 0.20);
	else
		cairo_set_source_rgb(cr, 0.78, 0.78, 0.80);
	cairo_move_to(cr, x + 0.5, y + h - 0.5);
	cairo_line_to(cr, x + 0.5, y + 0.5);
	cairo_line_to(cr, x + w - 0.5, y + 0.5);
	cairo_stroke(cr);

	if (sunken)
		cairo_set_source_rgb(cr, 0.78, 0.78, 0.80);
	else
		cairo_set_source_rgb(cr, 0.18, 0.18, 0.20);
	cairo_move_to(cr, x + w - 0.5, y + 0.5);
	cairo_line_to(cr, x + w - 0.5, y + h - 0.5);
	cairo_line_to(cr, x + 0.5, y + h - 0.5);
	cairo_stroke(cr);
}

static struct wl_buffer *
shm_buf(int w, int h, void **px)
{
	int size = w * 4 * h;
#if HAVE_MEMFD_CREATE
	int fd = memfd_create("cowbuttons", MFD_CLOEXEC);
#else
	char name[64];
	snprintf(name, sizeof(name), "/cowbuttons-%d", getpid());
	int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
	if (fd >= 0)
		shm_unlink(name);
#endif

	if (fd < 0)
		return NULL;
	if (ftruncate(fd, size) < 0) {
		close(fd);
		return NULL;
	}
	*px = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (*px == MAP_FAILED) {
		close(fd);
		return NULL;
	}

	struct wl_shm_pool *pool = wl_shm_create_pool(wl_shm, fd, size);
	struct wl_buffer *buf = wl_shm_pool_create_buffer(pool, 0, w, h,
	    w * 4, WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);
	return buf;
}

static void
render(void)
{
	void *px;
	struct wl_buffer *buf;
	cairo_surface_t *cs;
	cairo_t *cr;
	struct state st;

	if (!configured)
		return;

	win_w = cfg.cols * cfg.cell_w;
	win_h = cfg.rows * cfg.cell_h;
	buf = shm_buf(win_w, win_h, &px);
	if (buf == NULL)
		return;

	pthread_mutex_lock(&state_lock);
	st = current_state;
	dirty = 0;
	pthread_mutex_unlock(&state_lock);

	cs = cairo_image_surface_create_for_data(px, CAIRO_FORMAT_ARGB32,
	    win_w, win_h, win_w * 4);
	cr = cairo_create(cs);
	cairo_select_font_face(cr, cfg.font, CAIRO_FONT_SLANT_NORMAL,
	    CAIRO_FONT_WEIGHT_NORMAL);
	cairo_set_font_size(cr, 12);

	cairo_set_source_rgb(cr, 0.16, 0.16, 0.17);
	cairo_paint(cr);

	for (int row = 0; row < cfg.rows; row++) {
		for (int col = 0; col < cfg.cols; col++) {
			const struct cell *c = cell_at_grid(row, col);
			double x = col * cfg.cell_w;
			double y = row * cfg.cell_h;
			const char *label = "";
			int sunken = 0;

			if (c != NULL && c->type == CELL_SPACER)
				continue;
			if (c != NULL && c->type == CELL_BUTTON) {
				label = c->label;
				sunken = pressed_cell >= 0 &&
				    &cells[pressed_cell] == c;
			} else if (c != NULL && c->type == CELL_STATUS)
				label = status_text(c, &st);

			cairo_set_source_rgb(cr, 0.42, 0.42, 0.45);
			cairo_rectangle(cr, x, y, cfg.cell_w, cfg.cell_h);
			cairo_fill(cr);
			draw_relief(cr, x, y, cfg.cell_w, cfg.cell_h, sunken);

			cairo_set_source_rgb(cr, 0.94, 0.94, 0.96);
			draw_text(cr, x + TEXT_PAD,
			    y + cfg.cell_h / 2.0 + 4.0,
			    cfg.cell_w - 2 * TEXT_PAD, label);
		}
	}

	cairo_destroy(cr);
	cairo_surface_flush(cs);
	cairo_surface_destroy(cs);

	wl_surface_attach(surface, buf, 0, 0);
	wl_surface_damage_buffer(surface, 0, 0, win_w, win_h);
	wl_surface_commit(surface);
	wl_buffer_destroy(buf);
	munmap(px, (size_t)win_w * 4 * (size_t)win_h);
}

static void
ipc_send(const char *cmd)
{
	int fd = cow_module_cmd_connect(PROG);

	if (fd < 0)
		return;
	if (cow_module_cmd_send(fd, cmd, true)) {
		char b[COW_MODULE_ACK_BUFFER_SIZE];
		read(fd, b, sizeof(b));
	}
	close(fd);
}

static void
state_clear(struct state *st)
{
	memset(st, 0, sizeof(*st));
	strlcpy(st->focused_title, "", sizeof(st->focused_title));
	strlcpy(st->focused_app, "", sizeof(st->focused_app));
	strlcpy(st->current_output, "", sizeof(st->current_output));
	strlcpy(st->current_desk, "", sizeof(st->current_desk));
	strlcpy(st->desk_list, "", sizeof(st->desk_list));
}

static void
state_from_json(struct state *st, cJSON *root)
{
	state_clear(st);

	cJSON *focused = cJSON_GetObjectItem(root, "focused");
	if (cJSON_IsObject(focused)) {
		cJSON *title = cJSON_GetObjectItem(focused, "display_title");
		if (!cJSON_IsString(title))
			title = cJSON_GetObjectItem(focused, "title");
		if (cJSON_IsString(title) && title->valuestring != NULL)
			strlcpy(st->focused_title, title->valuestring,
			    sizeof(st->focused_title));
		cJSON *app = cJSON_GetObjectItem(focused, "app_id");
		if (cJSON_IsString(app) && app->valuestring != NULL)
			strlcpy(st->focused_app, app->valuestring,
			    sizeof(st->focused_app));
	}

	cJSON *outputs = cJSON_GetObjectItem(root, "outputs");
	if (!cJSON_IsArray(outputs))
		outputs = cJSON_GetObjectItem(root, "output");
	cJSON *out;
	cJSON_ArrayForEach(out, outputs) {
		cJSON *current = cJSON_GetObjectItem(out, "current");
		if (!cJSON_IsTrue(current))
			continue;
		cJSON *name = cJSON_GetObjectItem(out, "name");
		if (cJSON_IsString(name) && name->valuestring != NULL)
			strlcpy(st->current_output, name->valuestring,
			    sizeof(st->current_output));
		cJSON *desk = cJSON_GetObjectItem(out, "current_desk");
		if (cJSON_IsNumber(desk))
			snprintf(st->current_desk, sizeof(st->current_desk),
			    "%d", (int)desk->valuedouble);

		cJSON *desks = cJSON_GetObjectItem(out, "desks");
		if (cJSON_IsArray(desks)) {
			size_t len = 0;
			cJSON *d;
			cJSON_ArrayForEach(d, desks) {
				char item[64];
				cJSON *nr = cJSON_GetObjectItem(d, "nr");
				cJSON *dn = cJSON_GetObjectItem(d, "name");
				const char *s = NULL;

				if (cJSON_IsString(dn))
					s = dn->valuestring;
				if (s != NULL && *s != '\0')
					snprintf(item, sizeof(item), "%s", s);
				else if (cJSON_IsNumber(nr))
					snprintf(item, sizeof(item), "%d",
					    (int)nr->valuedouble);
				else
					continue;
				if (len > 0 && len + 1 < sizeof(st->desk_list))
					st->desk_list[len++] = ' ';
				for (const char *p = item; *p != '\0' &&
				    len + 1 < sizeof(st->desk_list); p++)
					st->desk_list[len++] = *p;
				st->desk_list[len] = '\0';
			}
		}
		break;
	}
}

static void
handle_status_line(const char *line)
{
	cJSON *root = cJSON_Parse(line);
	if (root == NULL)
		return;

	struct state st;
	state_from_json(&st, root);
	cJSON_Delete(root);

	pthread_mutex_lock(&state_lock);
	current_state = st;
	dirty = 1;
	pthread_mutex_unlock(&state_lock);

	if (wake_pipe[1] >= 0)
		write(wake_pipe[1], "x", 1);
}

static void *
status_thread(__unused void *arg)
{
	int fd = cow_module_status_connect(PROG, COW_MODULE_STATUS_RETRIES,
	    COW_MODULE_STATUS_RETRY_DELAY);
	char buf[COW_MODULE_STATUS_LINE_MAX];
	size_t len = 0;

	if (fd < 0)
		return NULL;
	for (;;) {
		char ch;
		ssize_t n = read(fd, &ch, 1);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (n == 0)
			break;
		if (ch == '\n') {
			buf[len] = '\0';
			handle_status_line(buf);
			len = 0;
			continue;
		}
		if (len + 1 < sizeof(buf))
			buf[len++] = ch;
	}
	close(fd);
	return NULL;
}

static void
handle_wake(void)
{
	char buf[64];
	while (read(wake_pipe[0], buf, sizeof(buf)) > 0)
		;
	if (dirty)
		render();
}

static void
wayland_cb(__unused evutil_socket_t fd, __unused short events,
    __unused void *data)
{
	if (wl_display_dispatch(display) < 0)
		running = 0;
}

static void
wake_cb(__unused evutil_socket_t fd, __unused short events,
    __unused void *data)
{
	handle_wake();
}

static void
xdg_wm_base_ping(__unused void *data, struct xdg_wm_base *wm_base,
    uint32_t serial)
{
	xdg_wm_base_pong(wm_base, serial);
}

static const struct xdg_wm_base_listener xdg_wm_base_listener = {
	.ping = xdg_wm_base_ping,
};

static void
xdg_surface_configure(__unused void *data, struct xdg_surface *xs,
    uint32_t serial)
{
	xdg_surface_ack_configure(xs, serial);
	configured = 1;
	render();
}

static const struct xdg_surface_listener xdg_surface_listener = {
	.configure = xdg_surface_configure,
};

static void
xdg_toplevel_configure(__unused void *data,
    __unused struct xdg_toplevel *toplevel, __unused int32_t w,
    __unused int32_t h, __unused struct wl_array *states)
{
}

static void
xdg_toplevel_close(__unused void *data, __unused struct xdg_toplevel *toplevel)
{
	running = 0;
}

static const struct xdg_toplevel_listener xdg_toplevel_listener = {
	.configure = xdg_toplevel_configure,
	.close = xdg_toplevel_close,
};

static void
pointer_enter(__unused void *data, __unused struct wl_pointer *p,
    __unused uint32_t serial, __unused struct wl_surface *s,
    wl_fixed_t sx, wl_fixed_t sy)
{
	ptr_x = wl_fixed_to_double(sx);
	ptr_y = wl_fixed_to_double(sy);
}

static void
pointer_leave(__unused void *data, __unused struct wl_pointer *p,
    __unused uint32_t serial, __unused struct wl_surface *s)
{
	ptr_x = ptr_y = -1;
}

static void
pointer_motion(__unused void *data, __unused struct wl_pointer *p,
    __unused uint32_t time, wl_fixed_t sx, wl_fixed_t sy)
{
	ptr_x = wl_fixed_to_double(sx);
	ptr_y = wl_fixed_to_double(sy);
}

static void
pointer_button(__unused void *data, __unused struct wl_pointer *p,
    __unused uint32_t serial, __unused uint32_t time, uint32_t button,
    uint32_t state)
{
	if (button != BTN_LEFT)
		return;

	if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
		int idx = cell_at_point(ptr_x, ptr_y);
		if (idx >= 0 && cells[idx].type == CELL_BUTTON) {
			pressed_cell = idx;
			render();
		}
		return;
	}

	int idx = cell_at_point(ptr_x, ptr_y);
	int pressed = pressed_cell;
	pressed_cell = -1;
	render();
	if (pressed >= 0 && idx == pressed && cells[pressed].type ==
	    CELL_BUTTON)
		ipc_send(cells[pressed].command);
}

static void
pointer_axis(__unused void *data, __unused struct wl_pointer *p,
    __unused uint32_t time, __unused uint32_t axis, __unused wl_fixed_t value)
{
}

static const struct wl_pointer_listener pointer_listener = {
	.enter = pointer_enter,
	.leave = pointer_leave,
	.motion = pointer_motion,
	.button = pointer_button,
	.axis = pointer_axis,
};

static void
seat_capabilities(__unused void *data, struct wl_seat *s, uint32_t caps)
{
	if ((caps & WL_SEAT_CAPABILITY_POINTER) && pointer == NULL) {
		pointer = wl_seat_get_pointer(s);
		wl_pointer_add_listener(pointer, &pointer_listener, NULL);
	} else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && pointer != NULL) {
		wl_pointer_destroy(pointer);
		pointer = NULL;
	}
}

static void
seat_name(__unused void *data, __unused struct wl_seat *s,
    __unused const char *name)
{
}

static const struct wl_seat_listener seat_listener = {
	.capabilities = seat_capabilities,
	.name = seat_name,
};

static void
reg_global(__unused void *data, struct wl_registry *reg, uint32_t name,
    const char *interface, uint32_t version)
{
	if (strcmp(interface, wl_compositor_interface.name) == 0)
		compositor = wl_registry_bind(reg, name, &wl_compositor_interface,
		    version < 4 ? version : 4);
	else if (strcmp(interface, wl_shm_interface.name) == 0)
		wl_shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
	else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
		xdg_wm_base = wl_registry_bind(reg, name,
		    &xdg_wm_base_interface, 1);
		xdg_wm_base_add_listener(xdg_wm_base, &xdg_wm_base_listener,
		    NULL);
	} else if (strcmp(interface, wl_seat_interface.name) == 0) {
		seat = wl_registry_bind(reg, name, &wl_seat_interface,
		    version < WL_SEAT_VERSION ? version : WL_SEAT_VERSION);
		wl_seat_add_listener(seat, &seat_listener, NULL);
	}
}

static void
reg_remove(__unused void *data, __unused struct wl_registry *reg,
    __unused uint32_t name)
{
}

static const struct wl_registry_listener reg_listener = {
	.global = reg_global,
	.global_remove = reg_remove,
};

static int
parse_args(int argc, char **argv, char **config_path)
{
	int ch;

	while ((ch = getopt(argc, argv, "c:h")) != -1) {
		switch (ch) {
		case 'c':
			*config_path = strdup(optarg);
			if (*config_path == NULL)
				return 0;
			break;
		case 'h':
			usage();
			exit(0);
		default:
			return 0;
		}
	}
	return optind == argc;
}

int
main(int argc, char **argv)
{
	char *config_path = NULL;
	struct wl_registry *reg;
	pthread_t tid;
	struct event_base *base;
	struct event *wl_ev = NULL, *wake_ev = NULL;

	if (!parse_args(argc, argv, &config_path)) {
		usage();
		return 1;
	}
	if (config_path == NULL) {
		config_path = default_config_path();
		if (config_path == NULL) {
			fprintf(stderr, "%s: cannot determine config path\n",
			    PROG);
			return 1;
		}
	}
	if (!load_config(config_path)) {
		free(config_path);
		return 1;
	}
	free(config_path);

	if (pipe(wake_pipe) < 0) {
		perror("pipe");
		return 1;
	}
	fcntl(wake_pipe[0], F_SETFL, O_NONBLOCK);
	fcntl(wake_pipe[1], F_SETFL, O_NONBLOCK);

	display = wl_display_connect(NULL);
	if (display == NULL) {
		fprintf(stderr, "%s: no Wayland display\n", PROG);
		return 1;
	}
	reg = wl_display_get_registry(display);
	wl_registry_add_listener(reg, &reg_listener, NULL);
	wl_display_roundtrip(display);
	wl_display_roundtrip(display);

	if (compositor == NULL || wl_shm == NULL || xdg_wm_base == NULL) {
		fprintf(stderr, "%s: missing Wayland globals\n", PROG);
		return 1;
	}

	surface = wl_compositor_create_surface(compositor);
	xdg_surface = xdg_wm_base_get_xdg_surface(xdg_wm_base, surface);
	xdg_surface_add_listener(xdg_surface, &xdg_surface_listener, NULL);
	xdg_toplevel = xdg_surface_get_toplevel(xdg_surface);
	xdg_toplevel_add_listener(xdg_toplevel, &xdg_toplevel_listener, NULL);
	xdg_toplevel_set_title(xdg_toplevel, PROG);
	xdg_toplevel_set_app_id(xdg_toplevel, PROG);
	xdg_toplevel_set_min_size(xdg_toplevel, cfg.cols * cfg.cell_w,
	    cfg.rows * cfg.cell_h);
	xdg_toplevel_set_max_size(xdg_toplevel, cfg.cols * cfg.cell_w,
	    cfg.rows * cfg.cell_h);
	wl_surface_commit(surface);

	state_clear(&current_state);
	pthread_create(&tid, NULL, status_thread, NULL);
	pthread_detach(tid);

	base = event_base_new();
	if (base == NULL) {
		fprintf(stderr, "%s: failed to initialise event loop\n", PROG);
		return 1;
	}
	wl_ev = event_new(base, wl_display_get_fd(display), EV_READ | EV_PERSIST,
	    wayland_cb, NULL);
	wake_ev = event_new(base, wake_pipe[0], EV_READ | EV_PERSIST, wake_cb,
	    NULL);
	if (wl_ev == NULL || wake_ev == NULL || event_add(wl_ev, NULL) < 0 ||
	    event_add(wake_ev, NULL) < 0) {
		fprintf(stderr, "%s: failed to add events\n", PROG);
		running = 0;
	}

	while (running) {
		if (wl_display_flush(display) < 0 && errno != EAGAIN)
			break;
		if (event_base_loop(base, EVLOOP_ONCE) < 0)
			break;
	}

	if (wl_ev != NULL)
		event_free(wl_ev);
	if (wake_ev != NULL)
		event_free(wake_ev);
	event_base_free(base);

	if (pointer != NULL)
		wl_pointer_destroy(pointer);
	if (seat != NULL)
		wl_seat_destroy(seat);
	if (xdg_toplevel != NULL)
		xdg_toplevel_destroy(xdg_toplevel);
	if (xdg_surface != NULL)
		xdg_surface_destroy(xdg_surface);
	if (surface != NULL)
		wl_surface_destroy(surface);
	wl_display_disconnect(display);
	return 0;
}
