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
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"

/* -----------------------------------------------------------------------
 * Layout constants
 * ----------------------------------------------------------------------- */

#define CELL_W 140  /* width of one desk cell */
#define CELL_H 105  /* height of one desk cell */
#define CELL_GAP 4  /* gap between desk cells */
#define MARGIN 6    /* outer margin */
#define LABEL_H 14  /* height of desk label strip */
#define PAGE_PAD 2  /* padding around page grid within desk cell */
#define BTN_SIZE 18 /* expand toggle button size */
#define MAX_DESKS 10
#define MAX_WINS 512
#define MAX_OUTS 8

/* -----------------------------------------------------------------------
 * Data types
 * ----------------------------------------------------------------------- */

typedef struct {
	int  desk;
	int  vx, vy, w, h;
	int  focused;
	char id[128];
	char output[64];
} pwin_t;

typedef struct {
	int nr;
	int window_count;
	int active;
} pdesk_t;

typedef struct {
	char name[64];
	int  current_desk;
	int  page_cols, page_rows;
	int  page_col, page_row;
	/*
	 * Raw sub-page viewport offsets in desk pixel space.  page_col
	 * and page_row are truncated indexes; page_vx / page_vy give
	 * the viewport's exact top-left so we can render the visible
	 * area as a rectangle rather than a single cell when the user
	 * has sub-page-scrolled (via `scroll` or an interactive drag).
	 */
	int	page_vx, page_vy;
	int	output_w, output_h;
	pdesk_t desks[MAX_DESKS];
	int	n_desks;
} pout_t;

typedef struct {
	pout_t outs[MAX_OUTS];
	int    n_outs;
	pwin_t wins[MAX_WINS];
	int    n_wins;
	char   focused_id[128];
	char   desktop_config[32]; /* "global" | "per-output" | "shared" */
} state_t;

/* -----------------------------------------------------------------------
 * Globals
 * ----------------------------------------------------------------------- */

static struct wl_display    *display;
static struct wl_compositor *compositor;
static struct wl_shm	    *wl_shm;
static struct xdg_wm_base   *xdg_wm_base;
static struct wl_seat	    *seat;
static struct wl_pointer    *pointer;
static struct wl_surface    *surface;
static struct xdg_surface   *xdg_surface;
static struct xdg_toplevel  *xdg_toplevel;

static int    surface_configured;
static int    win_w, win_h; /* current window size from configure */
static double ptr_x, ptr_y;
static int    opt_running = 1;
static char   status_sock[4096];
static char   cmd_sock[4096];
static char   opt_output[64]; /* -o NAME, empty = auto */
static int    expanded;	      /* expanded = show all desks */

static state_t	       g_state;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int	       g_dirty;
static int	       wake_pipe[2];

/* -----------------------------------------------------------------------
 * IPC helpers
 * ----------------------------------------------------------------------- */

static void
ipc_send(const char *cmd)
{
	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0)
		return;

	struct sockaddr_un a = { .sun_family = AF_UNIX };
	if (strlen(cmd_sock) >= sizeof(a.sun_path)) {
		fprintf(stderr, "cowpager: socket path too long: %s\n",
		    cmd_sock);
		close(fd);
		return;
	}
	strncpy(a.sun_path, cmd_sock, sizeof(a.sun_path) - 1);

	if (connect(fd, (struct sockaddr *)&a, sizeof(a)) == 0) {
		write(fd, cmd, strlen(cmd));
		write(fd, "\n", 1);

		char b[4096];
		read(fd, b, sizeof(b));
	}
	close(fd);
}

static char *
ipc_query(const char *cmd)
{
	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0)
		return NULL;

	struct sockaddr_un a = { .sun_family = AF_UNIX };
	if (strlen(cmd_sock) >= sizeof(a.sun_path)) {
		fprintf(stderr, "cowpager: socket path too long: %s\n",
		    cmd_sock);
		close(fd);
		return NULL;
	}
	strncpy(a.sun_path, cmd_sock, sizeof(a.sun_path) - 1);

	if (connect(fd, (struct sockaddr *)&a, sizeof(a)) != 0) {
		close(fd);
		return NULL;
	}

	write(fd, cmd, strlen(cmd));
	write(fd, "\n", 1);

	/* FIXME: this should be calculated dynamically, rather than an
	 * arbitrary value.
	 */
	char *resp = malloc(131072);
	if (!resp) {
		close(fd);
		return NULL;
	}

	ssize_t tot = 0, n;
	while ((n = read(fd, resp + tot, 131072 - 1 - tot)) > 0)
		tot += n;
	close(fd);

	resp[tot] = '\0';
	return resp;
}

static struct wl_buffer *
shm_buf(int w, int h, void **px)
{
	int size = w * 4 * h;
	int fd	 = memfd_create("cp", MFD_CLOEXEC);
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

	struct wl_shm_pool *p = wl_shm_create_pool(wl_shm, fd, size);
	struct wl_buffer   *b = wl_shm_pool_create_buffer(p, 0, w, h, w * 4,
	      WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(p);
	close(fd);
	return b;
}

/* Return the pout_t to display (per-output: the tracked output; global: NULL
 * meaning "render all").
 */
static const pout_t *
tracked_output(const state_t *st)
{
	if (strcmp(st->desktop_config, "global") == 0)
		return NULL;

	/* per-output: find by name */
	for (int i = 0; i < st->n_outs; i++) {
		if (opt_output[0]) {
			if (strcmp(st->outs[i].name, opt_output) == 0)
				return &st->outs[i];
		} else {
			return &st->outs[0]; /* first */
		}
	}
	return st->n_outs ? &st->outs[0] : NULL;
}

/* Compute the required window size for the current state/mode.
 * expanded=0: single-desk NxM page grid.
 * expanded=1 or global: all desks side by side, each with NxM pages. */
static void
compute_size(const state_t *st, int *rw, int *rh)
{
	const pout_t *o = tracked_output(st);
	int	      n_desks;

	if (!o && st->n_outs > 0)
		o = &st->outs[0]; /* global: pick first for grid dims */

	if (expanded || strcmp(st->desktop_config, "global") == 0) {
		/* Show all desks */
		n_desks = o ? o->n_desks : 1;
		if (n_desks < 1)
			n_desks = 1;
	} else {
		/* Single current desk */
		n_desks = 1;
	}

	*rw = MARGIN * 2 + n_desks * (CELL_W + CELL_GAP) - CELL_GAP;
	*rh = MARGIN * 2 + CELL_H + BTN_SIZE;
}

/* Draw one desk cell at pixel position (dx, dy).
 * active: this is the current desk on the output.
 * Show miniature page grid and window thumbnails for windows on this
 * output/desk. */
static void
draw_desk(cairo_t *cr, const state_t *st, const pout_t *o, const pdesk_t *d,
    int dx, int dy, int is_active, const char *out_name)
{
	int pcols = o->page_cols > 0 ? o->page_cols : 1;
	int prows = o->page_rows > 0 ? o->page_rows : 1;
	int ow	  = o->output_w > 0 ? o->output_w : 1920;
	int oh	  = o->output_h > 0 ? o->output_h : 1080;

	/* Desk background */
	double r = 6, x0 = dx, y0 = dy, x1 = dx + CELL_W, y1 = dy + CELL_H;

	/* FIXME: should be configurable. */
	if (is_active)
		cairo_set_source_rgba(cr, 0.15, 0.28, 0.50, 1.0);
	else
		cairo_set_source_rgba(cr, 0.18, 0.18, 0.20, 1.0);
	cairo_move_to(cr, x0 + r, y0);
	cairo_line_to(cr, x1 - r, y0);
	cairo_arc(cr, x1 - r, y0 + r, r, -M_PI / 2, 0);
	cairo_line_to(cr, x1, y1 - r);
	cairo_arc(cr, x1 - r, y1 - r, r, 0, M_PI / 2);
	cairo_line_to(cr, x0 + r, y1);
	cairo_arc(cr, x0 + r, y1 - r, r, M_PI / 2, M_PI);
	cairo_line_to(cr, x0, y0 + r);
	cairo_arc(cr, x0 + r, y0 + r, r, M_PI, 3 * M_PI / 2);
	cairo_close_path(cr);
	cairo_fill(cr);

	/* Desk label */
	cairo_set_source_rgba(cr, 0.85, 0.85, 0.88, 1.0);
	cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
	    CAIRO_FONT_WEIGHT_BOLD);
	cairo_set_font_size(cr, 8.5);
	char lbl[256];
	snprintf(lbl, sizeof(lbl), "(%s): Desk %d", o->name, d->nr);
	cairo_move_to(cr, dx + 4, dy + LABEL_H - 2);
	cairo_show_text(cr, lbl);

	/* Page grid */
	int    gx = dx + PAGE_PAD;
	int    gy = dy + LABEL_H;
	int    gw = CELL_W - PAGE_PAD * 2;
	int    gh = CELL_H - LABEL_H - PAGE_PAD;
	double cw = (double)gw / pcols;
	double ch = (double)gh / prows;

	for (int pr = 0; pr < prows; pr++) {
		for (int pc = 0; pc < pcols; pc++) {
			double cx     = gx + pc * cw;
			double cy     = gy + pr * ch;
			double cpw    = cw - 1;
			double cph    = ch - 1;
			int    is_cur = is_active && pc == o->page_col &&
			    pr == o->page_row;

			/* Page cell background */
			if (is_cur)
				cairo_set_source_rgba(cr, 0.18, 0.38, 0.18,
				    1.0);
			else
				cairo_set_source_rgba(cr, 0.24, 0.24, 0.27,
				    1.0);
			cairo_rectangle(cr, cx, cy, cpw, cph);
			cairo_fill(cr);

			/* Page cell border */
			cairo_set_source_rgba(cr, 0.38, 0.38, 0.42, 1.0);
			cairo_set_line_width(cr, 0.5);
			cairo_rectangle(cr, cx + 0.5, cy + 0.5, cpw - 1,
			    cph - 1);
			cairo_stroke(cr);

			/* Window miniatures */
			int pvx = pc * ow;
			int pvy = pr * oh;
			for (int wi = 0; wi < st->n_wins; wi++) {
				const pwin_t *w = &st->wins[wi];
				if (w->desk != d->nr)
					continue;
				/* Filter by output in per-output mode */
				if (out_name && out_name[0] && w->output[0] &&
				    strcmp(w->output, out_name) != 0)
					continue;
				if (w->vx < pvx || w->vx >= pvx + ow)
					continue;
				if (w->vy < pvy || w->vy >= pvy + oh)
					continue;
				double sx = cx +
				    (double)(w->vx - pvx) / ow * cpw;
				double sy = cy +
				    (double)(w->vy - pvy) / oh * cph;
				double sw = (double)w->w / ow * cpw;
				double sh = (double)w->h / oh * cph;
				if (sw < 2)
					sw = 2;
				if (sh < 2)
					sh = 2;
				if (sx + sw > cx + cpw)
					sw = cx + cpw - sx;
				if (sy + sh > cy + cph)
					sh = cy + cph - sy;
				int foc = strcmp(w->id, st->focused_id) == 0;
				if (foc)
					cairo_set_source_rgba(cr, 0.95, 0.75,
					    0.20, 1.0);
				else
					cairo_set_source_rgba(cr, 0.50, 0.58,
					    0.70, 1.0);
				cairo_rectangle(cr, sx, sy, sw, sh);
				cairo_fill(cr);
			}
		}
	}

	/*
	 * Sub-page viewport overlay.  When the active desk's viewport
	 * sits at an arbitrary pixel offset (after `scroll` or an
	 * interactive scroll-drag), highlight the actual visible
	 * region rather than just one whole-page cell.  Drawn after
	 * the page-grid pass so it sits on top of the cells and any
	 * window miniatures that fall inside it.
	 */
	if (is_active) {
		/* Desk extent in pixels. */
		double dx_w = (double)pcols * ow;
		double dy_h = (double)prows * oh;
		if (dx_w > 0 && dy_h > 0) {
			double vx = o->page_vx;
			double vy = o->page_vy;
			double vw = ow;
			double vh = oh;
			/* Map desk-pixels onto the grid's pixel area. */
			double ax = gx + (vx / dx_w) * gw;
			double ay = gy + (vy / dy_h) * gh;
			double aw = (vw / dx_w) * gw;
			double ah = (vh / dy_h) * gh;
			/* Translucent fill + crisp outline so it reads
			 * over any cell-highlight underneath. */
			cairo_set_source_rgba(cr, 0.95, 0.85, 0.30, 0.18);
			cairo_rectangle(cr, ax, ay, aw, ah);
			cairo_fill(cr);
			cairo_set_source_rgba(cr, 0.95, 0.85, 0.30, 0.95);
			cairo_set_line_width(cr, 1.2);
			cairo_rectangle(cr, ax + 0.5, ay + 0.5, aw - 1, ah - 1);
			cairo_stroke(cr);
		}
	}
}

static void
do_render(const state_t *st)
{
	if (!surface_configured || win_w <= 0 || win_h <= 0)
		return;

	void		 *px  = NULL;
	struct wl_buffer *buf = shm_buf(win_w, win_h, &px);
	if (!buf)
		return;

	cairo_surface_t *cs = cairo_image_surface_create_for_data(px,
	    CAIRO_FORMAT_ARGB32, win_w, win_h, win_w * 4);
	cairo_t		*cr = cairo_create(cs);

	/* Background */
	cairo_set_source_rgba(cr, 0.10, 0.10, 0.12, 0.96);
	cairo_paint(cr);

	if (st->n_outs == 0)
		goto commit;

	const pout_t *o	     = tracked_output(st);
	int	      global = strcmp(st->desktop_config, "global") == 0;

	if (!o)
		o = &st->outs[0];

	const char *out_name = global ? NULL : o->name;

	/* Determine which desks to draw */
	int start_desk, end_desk;
	if (expanded || global) {
		start_desk = 0;
		end_desk   = o->n_desks;
	} else {
		/* Single-desk mode: find the current desk index */
		start_desk = 0;
		end_desk   = o->n_desks;
		for (int i = 0; i < o->n_desks; i++) {
			if (o->desks[i].nr == o->current_desk) {
				start_desk = i;
				end_desk   = i + 1;
				break;
			}
		}
	}

	int cell_idx = 0;
	for (int i = start_desk; i < end_desk; i++) {
		const pdesk_t *d  = &o->desks[i];
		int	       dx = MARGIN + cell_idx * (CELL_W + CELL_GAP);
		int	       dy = MARGIN;
		int	       is_active = (d->nr == o->current_desk);
		draw_desk(cr, st, o, d, dx, dy, is_active, out_name);
		cell_idx++;
	}

	/* Expand toggle button. */
	{
		int bx = win_w - MARGIN - BTN_SIZE;
		int by = win_h - MARGIN - BTN_SIZE;
		if (expanded || global)
			cairo_set_source_rgba(cr, 0.30, 0.48, 0.30, 1.0);
		else
			cairo_set_source_rgba(cr, 0.28, 0.28, 0.32, 1.0);
		cairo_rectangle(cr, bx, by, BTN_SIZE, BTN_SIZE);
		cairo_fill(cr);
		cairo_set_source_rgba(cr, 0.85, 0.85, 0.88, 1.0);
		cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
		    CAIRO_FONT_WEIGHT_BOLD);
		cairo_set_font_size(cr, 11.0);
		cairo_move_to(cr, bx + 3, by + BTN_SIZE - 4);
		cairo_show_text(cr, global ? "" : (expanded ? "-" : "+"));
	}

commit:
	cairo_destroy(cr);
	cairo_surface_destroy(cs);
	wl_surface_attach(surface, buf, 0, 0);
	wl_surface_damage_buffer(surface, 0, 0, win_w, win_h);
	wl_surface_commit(surface);
	munmap(px, (size_t)win_w * win_h * 4);
	wl_buffer_destroy(buf);
}

static void
request_resize(const state_t *st)
{
	int rw, rh;
	compute_size(st, &rw, &rh);
	if (rw == win_w && rh == win_h)
		return; /* no change */
	win_w = rw;
	win_h = rh;

	/* Unconstrained: compositor follows the buffer size we attach */
	xdg_toplevel_set_min_size(xdg_toplevel, 0, 0);
	xdg_toplevel_set_max_size(xdg_toplevel, 0, 0);
}

static int
hit_test(const state_t *st, double x, double y, int *desk_nr, int *pc_out,
    int *pr_out)
{
	*desk_nr = *pc_out = *pr_out = -1;
	if (!st->n_outs)
		return 0;
	const pout_t *o = tracked_output(st);
	if (!o)
		o = &st->outs[0];

	int global = strcmp(st->desktop_config, "global") == 0;
	int start, end;
	if (expanded || global) {
		start = 0;
		end   = o->n_desks;
	} else {
		start = end = 0;
		for (int i = 0; i < o->n_desks; i++) {
			if (o->desks[i].nr == o->current_desk) {
				start = i;
				end   = i + 1;
				break;
			}
		}
	}

	int pcols = o->page_cols > 0 ? o->page_cols : 1;
	int prows = o->page_rows > 0 ? o->page_rows : 1;

	int cell_idx = 0;
	for (int i = start; i < end; i++) {
		int dx = MARGIN + cell_idx * (CELL_W + CELL_GAP);
		int dy = MARGIN;
		if (x >= dx && x < dx + CELL_W && y >= dy && y < dy + CELL_H) {
			*desk_nr = o->desks[i].nr;

			/* Check if click is in the page grid area */
			double gx = dx + PAGE_PAD;
			double gy = dy + LABEL_H;
			double gw = CELL_W - PAGE_PAD * 2;
			double gh = CELL_H - LABEL_H - PAGE_PAD;
			int    pc = (int)((x - gx) / (gw / pcols));
			int    pr = (int)((y - gy) / (gh / prows));
			if (pc >= 0 && pc < pcols && pr >= 0 && pr < prows &&
			    x >= gx && y >= gy) {
				*pc_out = pc;
				*pr_out = pr;
			}
			return 1;
		}
		cell_idx++;
	}
	return 0;
}

/* Find the window miniature under (x, y) within a specific page cell.
 * Returns the window id string or NULL. */
static const char *
hit_test_window(const state_t *st, double x, double y)
{
	if (!st->n_outs)
		return NULL;

	const pout_t *o = tracked_output(st);
	if (!o)
		o = &st->outs[0];

	int	    global   = strcmp(st->desktop_config, "global") == 0;
	const char *out_name = global ? NULL : o->name;

	int pcols = o->page_cols > 0 ? o->page_cols : 1;
	int prows = o->page_rows > 0 ? o->page_rows : 1;
	int ow	  = o->output_w > 0 ? o->output_w : 1920;
	int oh	  = o->output_h > 0 ? o->output_h : 1080;

	int start, end;
	if (expanded || global) {
		start = 0;
		end   = o->n_desks;
	} else {
		start = end = 0;
		for (int i = 0; i < o->n_desks; i++) {
			if (o->desks[i].nr == o->current_desk) {
				start = i;
				end   = i + 1;
				break;
			}
		}
	}

	int cell_idx = 0;
	for (int i = start; i < end; i++) {
		const pdesk_t *d  = &o->desks[i];
		int	       dx = MARGIN + cell_idx++ * (CELL_W + CELL_GAP);
		int	       dy = MARGIN;
		if (x < dx || x >= dx + CELL_W || y < dy || y >= dy + CELL_H)
			continue;

		double gx = dx + PAGE_PAD;
		double gy = dy + LABEL_H;
		double gw = CELL_W - PAGE_PAD * 2;
		double gh = CELL_H - LABEL_H - PAGE_PAD;
		double cw = gw / pcols;
		double ch = gh / prows;
		int    pc = (int)((x - gx) / cw);
		int    pr = (int)((y - gy) / ch);

		if (pc < 0 || pc >= pcols || pr < 0 || pr >= prows)
			continue;

		double cx  = gx + pc * cw;
		double cy  = gy + pr * ch;
		double cpw = cw - 1;
		double cph = ch - 1;
		int    pvx = pc * ow;
		int    pvy = pr * oh;

		/* Find topmost window under cursor */
		const char *found = NULL;
		for (int wi = 0; wi < st->n_wins; wi++) {
			const pwin_t *w = &st->wins[wi];
			if (w->desk != d->nr)
				continue;
			if (out_name && out_name[0] && w->output[0] &&
			    strcmp(w->output, out_name) != 0)
				continue;
			if (w->vx < pvx || w->vx >= pvx + ow)
				continue;
			if (w->vy < pvy || w->vy >= pvy + oh)
				continue;
			double sx = cx + (double)(w->vx - pvx) / ow * cpw;
			double sy = cy + (double)(w->vy - pvy) / oh * cph;
			double sw = (double)w->w / ow * cpw;
			double sh = (double)w->h / oh * cph;
			if (sw < 2)
				sw = 2;
			if (sh < 2)
				sh = 2;
			if (x >= sx && x < sx + sw && y >= sy && y < sy + sh)
				found = w->id;
		}
		return found;
	}
	return NULL;
}

static void
xdg_surface_configure(void *data, struct xdg_surface *xs, uint32_t serial)
{
	(void)data;
	xdg_surface_ack_configure(xs, serial);
	surface_configured = 1;

	pthread_mutex_lock(&g_lock);
	state_t st = g_state;

	pthread_mutex_unlock(&g_lock);
	do_render(&st);
}

static const struct xdg_surface_listener xdg_surface_listener = {
	.configure = xdg_surface_configure,
};

static void
xdg_toplevel_configure(void *data, struct xdg_toplevel *tl, int32_t w,
    int32_t h, struct wl_array *states)
{
	(void)data;
	(void)tl;
	(void)states;
	if (w > 0)
		win_w = w;
	if (h > 0)
		win_h = h;
}

static void
xdg_toplevel_close(void *data, struct xdg_toplevel *tl)
{
	(void)data;
	(void)tl;
	opt_running = 0;
}

static const struct xdg_toplevel_listener xdg_toplevel_listener = {
	.configure = xdg_toplevel_configure,
	.close	   = xdg_toplevel_close,
};

static void
xdg_wm_base_ping(void *data, struct xdg_wm_base *base, uint32_t serial)
{
	(void)data;
	xdg_wm_base_pong(base, serial);
}

static const struct xdg_wm_base_listener xdg_wm_base_listener = {
	.ping = xdg_wm_base_ping,
};

static void
ptr_enter(void *d, struct wl_pointer *p, uint32_t s, struct wl_surface *sf,
    wl_fixed_t x, wl_fixed_t y)
{
	(void)d;
	(void)p;
	(void)s;
	(void)sf;
	ptr_x = wl_fixed_to_double(x);
	ptr_y = wl_fixed_to_double(y);
}

static void
ptr_leave(void *d, struct wl_pointer *p, uint32_t s, struct wl_surface *sf)
{
	(void)d;
	(void)p;
	(void)s;
	(void)sf;
}

static void
ptr_motion(void *d, struct wl_pointer *p, uint32_t t, wl_fixed_t x,
    wl_fixed_t y)
{
	(void)d;
	(void)p;
	(void)t;
	ptr_x = wl_fixed_to_double(x);
	ptr_y = wl_fixed_to_double(y);
}

static void
ptr_button(void *d, struct wl_pointer *p, uint32_t s, uint32_t t, uint32_t btn,
    uint32_t state)
{
	(void)d;
	(void)p;
	(void)s;
	(void)t;
	(void)btn;
	if (state != WL_POINTER_BUTTON_STATE_PRESSED)
		return;

	pthread_mutex_lock(&g_lock);
	state_t st = g_state;
	pthread_mutex_unlock(&g_lock);

	int global = strcmp(st.desktop_config, "global") == 0;

	/* Expand toggle button — bottom-right */
	if (!global) {
		int bx = win_w - MARGIN - BTN_SIZE;
		int by = win_h - MARGIN - BTN_SIZE;
		if (ptr_x >= bx && ptr_x < bx + BTN_SIZE && ptr_y >= by &&
		    ptr_y < by + BTN_SIZE) {
			expanded = !expanded;
			request_resize(&st);
			do_render(&st);
			return;
		}
	}

	/* Try window miniature first */
	const char *wid = hit_test_window(&st, ptr_x, ptr_y);
	if (wid && wid[0]) {
		char cmd[256];
		snprintf(cmd, sizeof(cmd), "focus -i %s", wid);
		ipc_send(cmd);
		return;
	}

	/* Page / desk hit */
	int desk_nr, pc, pr;
	if (!hit_test(&st, ptr_x, ptr_y, &desk_nr, &pc, &pr))
		return;

	/* Switch desk if needed */
	if (desk_nr >= 0) {
		const pout_t *o = tracked_output(&st);
		if (!o && st.n_outs)
			o = &st.outs[0];
		if (o && desk_nr != o->current_desk) {
			char cmd[64];
			snprintf(cmd, sizeof(cmd), "desk -d %d", desk_nr);
			ipc_send(cmd);
		}
	}

	/* Switch page */
	if (pc >= 0 && pr >= 0) {
		const pout_t *o = tracked_output(&st);
		if (!o && st.n_outs)
			o = &st.outs[0];
		char cmd[128];
		if (o && o->name[0] && strcmp(st.desktop_config, "global") != 0)
			snprintf(cmd, sizeof(cmd), "page -t %s -c %d %d",
			    o->name, pc, pr);
		else
			snprintf(cmd, sizeof(cmd), "page -c %d %d", pc, pr);
		ipc_send(cmd);
	}
}

static void
ptr_axis(void *d, struct wl_pointer *p, uint32_t t, uint32_t a, wl_fixed_t v)
{
	(void)d;
	(void)p;
	(void)t;
	(void)a;
	(void)v;
}
static void
ptr_frame(void *d, struct wl_pointer *p)
{
	(void)d;
	(void)p;
}
static void
ptr_axis_source(void *d, struct wl_pointer *p, uint32_t s)
{
	(void)d;
	(void)p;
	(void)s;
}
static void
ptr_axis_stop(void *d, struct wl_pointer *p, uint32_t t, uint32_t a)
{
	(void)d;
	(void)p;
	(void)t;
	(void)a;
}
static void
ptr_axis_discrete(void *d, struct wl_pointer *p, uint32_t a, int32_t v)
{
	(void)d;
	(void)p;
	(void)a;
	(void)v;
}

static const struct wl_pointer_listener ptr_listener = {
	.enter	       = ptr_enter,
	.leave	       = ptr_leave,
	.motion	       = ptr_motion,
	.button	       = ptr_button,
	.axis	       = ptr_axis,
	.frame	       = ptr_frame,
	.axis_source   = ptr_axis_source,
	.axis_stop     = ptr_axis_stop,
	.axis_discrete = ptr_axis_discrete,
};

static void
seat_caps(void *d, struct wl_seat *s, uint32_t caps)
{
	(void)d;
	if ((caps & WL_SEAT_CAPABILITY_POINTER) && !pointer) {
		pointer = wl_seat_get_pointer(s);
		wl_pointer_add_listener(pointer, &ptr_listener, NULL);
	}
}

static void
seat_name(void *d, struct wl_seat *s, const char *n)
{
	(void)d;
	(void)s;
	(void)n;
}

static const struct wl_seat_listener seat_listener = {
	.capabilities = seat_caps,
	.name	      = seat_name,
};

static void
reg_global(void *d, struct wl_registry *reg, uint32_t name, const char *iface,
    uint32_t ver)
{
	(void)d;
	(void)ver;
	if (!strcmp(iface, wl_compositor_interface.name))
		compositor = wl_registry_bind(reg, name,
		    &wl_compositor_interface, 4);
	else if (!strcmp(iface, wl_shm_interface.name))
		wl_shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
	else if (!strcmp(iface, xdg_wm_base_interface.name)) {
		xdg_wm_base = wl_registry_bind(reg, name,
		    &xdg_wm_base_interface, 1);
		xdg_wm_base_add_listener(xdg_wm_base, &xdg_wm_base_listener,
		    NULL);
	} else if (!strcmp(iface, wl_seat_interface.name)) {
		seat = wl_registry_bind(reg, name, &wl_seat_interface, 5);
		wl_seat_add_listener(seat, &seat_listener, NULL);
	}
}

static void
reg_remove(void *d, struct wl_registry *r, uint32_t n)
{
	(void)d;
	(void)r;
	(void)n;
}

static const struct wl_registry_listener reg_listener = {
	.global	       = reg_global,
	.global_remove = reg_remove,
};

static void
parse_status(const char *json)
{
	cJSON *root = cJSON_Parse(json);
	if (!root)
		return;

	state_t st;
	memset(&st, 0, sizeof(st));

	/* desktop_configuration */
	cJSON *dcfg = cJSON_GetObjectItem(root, "desktop_configuration");
	if (cJSON_IsString(dcfg))
		strncpy(st.desktop_config, dcfg->valuestring,
		    sizeof(st.desktop_config) - 1);
	else
		strncpy(st.desktop_config, "global",
		    sizeof(st.desktop_config) - 1);

	/* outputs array */
	cJSON *outs = cJSON_GetObjectItem(root, "outputs");
	cJSON *out;
	int    oi = 0;
	cJSON_ArrayForEach(out, outs)
	{
		if (oi >= MAX_OUTS)
			break;
		pout_t *po = &st.outs[oi++];
		cJSON  *v;
#define GN(k, f)                         \
	v = cJSON_GetObjectItem(out, k); \
	if (cJSON_IsNumber(v))           \
		po->f = (int)v->valuedouble;
#define GS(k, f, z)                      \
	v = cJSON_GetObjectItem(out, k); \
	if (cJSON_IsString(v))           \
		strncpy(po->f, v->valuestring, z - 1);
		GS("name", name, 64)
		GN("current_desk", current_desk)
		GN("page_cols", page_cols)
		GN("page_rows", page_rows)
		GN("page_col", page_col)
		GN("page_row", page_row)
		GN("page_vx", page_vx)
		GN("page_vy", page_vy)
		GN("output_w", output_w)
		GN("output_h", output_h)
#undef GN
#undef GS
		cJSON *desks = cJSON_GetObjectItem(out, "desks");
		cJSON *dk;
		int    di = 0;
		cJSON_ArrayForEach(dk, desks)
		{
			if (di >= MAX_DESKS)
				break;
			pdesk_t *pd = &po->desks[di++];
			v	    = cJSON_GetObjectItem(dk, "nr");
			if (cJSON_IsNumber(v))
				pd->nr = (int)v->valuedouble;
			v = cJSON_GetObjectItem(dk, "windows");
			if (cJSON_IsNumber(v))
				pd->window_count = (int)v->valuedouble;
			pd->active = cJSON_IsTrue(
			    cJSON_GetObjectItem(dk, "active"));
		}
		po->n_desks = di;
	}
	st.n_outs = oi;

	/* focused window id */
	cJSON *foc = cJSON_GetObjectItem(root, "focused");
	if (foc) {
		cJSON *fid = cJSON_GetObjectItem(foc, "id");
		if (cJSON_IsString(fid))
			strncpy(st.focused_id, fid->valuestring,
			    sizeof(st.focused_id) - 1);
	}
	cJSON_Delete(root);

	pthread_mutex_lock(&g_lock);

	/* Preserve window list across status updates */
	st.n_wins = g_state.n_wins;
	memcpy(st.wins, g_state.wins, sizeof(st.wins));
	g_state = st;
	g_dirty = 1;
	pthread_mutex_unlock(&g_lock);
}

static void
fetch_windows(void)
{
	char *resp = ipc_query("show windows");
	if (!resp)
		return;

	cJSON *root = cJSON_Parse(resp);
	free(resp);
	if (!root)
		return;

	cJSON *data = cJSON_GetObjectItem(root, "data");
	if (!cJSON_IsArray(data)) {
		cJSON_Delete(root);
		return;
	}

	pwin_t wins[MAX_WINS];
	int    nw = 0;
	cJSON *w;
	cJSON_ArrayForEach(w, data)
	{
		if (nw >= MAX_WINS)
			break;
		pwin_t *pw = &wins[nw++];
		memset(pw, 0, sizeof(*pw));
		cJSON *v;
#define GN(k, f)                       \
	v = cJSON_GetObjectItem(w, k); \
	if (cJSON_IsNumber(v))         \
		pw->f = (int)v->valuedouble;
#define GS(k, f, z)                    \
	v = cJSON_GetObjectItem(w, k); \
	if (cJSON_IsString(v))         \
		strncpy(pw->f, v->valuestring, z - 1);
		GS("id", id, 128)
		GS("output", output, 64)
		GN("desk", desk)
		GN("vx", vx)
		GN("vy", vy)
		GN("width", w)
		GN("height", h)
		pw->focused = cJSON_IsTrue(cJSON_GetObjectItem(w, "focused"));
#undef GN
#undef GS
	}
	cJSON_Delete(root);

	pthread_mutex_lock(&g_lock);
	g_state.n_wins = nw;
	memcpy(g_state.wins, wins, sizeof(pwin_t) * (size_t)nw);
	g_dirty = 1;
	pthread_mutex_unlock(&g_lock);
}

/* -----------------------------------------------------------------------
 * Status listener thread
 * ----------------------------------------------------------------------- */

static void *
status_thread(void *arg)
{
	(void)arg;
	int fd = -1;
	for (int r = 0; r < 30; r++) {
		fd = socket(AF_UNIX, SOCK_STREAM, 0);
		if (fd < 0)
			return NULL;
		struct sockaddr_un a = { .sun_family = AF_UNIX };
		if (strlen(status_sock) >= sizeof(a.sun_path)) {
			fprintf(stderr, "cowpager: socket path too long: %s\n",
			    status_sock);
			close(fd);
			return NULL;
		}
		strncpy(a.sun_path, status_sock, sizeof(a.sun_path) - 1);
		if (connect(fd, (struct sockaddr *)&a, sizeof(a)) == 0)
			break;
		close(fd);
		fd = -1;
		sleep(1);
	}
	if (fd < 0) {
		fprintf(stderr, "cowpager: cannot connect to %s\n",
		    status_sock);
		return NULL;
	}
	FILE *f = fdopen(fd, "r");
	if (!f) {
		close(fd);
		return NULL;
	}
	setlinebuf(f);
	char line[131072];
	while (opt_running && fgets(line, sizeof(line), f)) {
		size_t len = strlen(line);
		if (len > 0 && line[len - 1] == '\n')
			line[len - 1] = '\0';
		parse_status(line);
		fetch_windows();
		/* Single wake covers both updates above */
		char b = 1;
		write(wake_pipe[1], &b, 1);
	}
	fclose(f);
	return NULL;
}

int
main(int argc, char *argv[])
{
	int opt;
	while ((opt = getopt(argc, argv, "o:h")) != -1) {
		switch (opt) {
		case 'o':
			strncpy(opt_output, optarg, sizeof(opt_output) - 1);
			break;
		case 'h':
			fprintf(stderr,
			    "usage: cowpager [-o output] [-h]\n"
			    "  -o NAME   pin to output with given name\n"
			    "  -h        show this help\n");
			return 0;
		default:
			return 1;
		}
	}

	const char *rt = getenv("XDG_RUNTIME_DIR");
	if (!rt) {
		fprintf(stderr, "cowpager: XDG_RUNTIME_DIR not set\n");
		return 1;
	}

	/* FIXME: these should be discoverable. */
	snprintf(status_sock, sizeof(status_sock), "%s/cow-status.sock", rt);
	snprintf(cmd_sock, sizeof(cmd_sock), "%s/cow-cmd.sock", rt);

	if (pipe(wake_pipe) < 0) {
		perror("pipe");
		return 1;
	}
	fcntl(wake_pipe[0], F_SETFL, O_NONBLOCK);

	display = wl_display_connect(NULL);
	if (!display) {
		fprintf(stderr, "cowpager: no Wayland display\n");
		return 1;
	}

	struct wl_registry *reg = wl_display_get_registry(display);
	wl_registry_add_listener(reg, &reg_listener, NULL);
	wl_display_roundtrip(display);
	wl_display_roundtrip(display);

	if (!compositor || !wl_shm || !xdg_wm_base) {
		fprintf(stderr,
		    "cowpager: missing Wayland globals "
		    "(compositor=%p shm=%p xdg=%p)\n",
		    (void *)compositor, (void *)wl_shm, (void *)xdg_wm_base);
		return 1;
	}

	/* Initial size — will be updated once status arrives */
	win_w = MARGIN * 2 + CELL_W;
	win_h = MARGIN * 2 + CELL_H + BTN_SIZE;

	surface	    = wl_compositor_create_surface(compositor);
	xdg_surface = xdg_wm_base_get_xdg_surface(xdg_wm_base, surface);
	xdg_surface_add_listener(xdg_surface, &xdg_surface_listener, NULL);
	xdg_toplevel = xdg_surface_get_toplevel(xdg_surface);
	xdg_toplevel_add_listener(xdg_toplevel, &xdg_toplevel_listener, NULL);
	xdg_toplevel_set_title(xdg_toplevel, "cowpager");
	xdg_toplevel_set_app_id(xdg_toplevel, "cowpager");

	/* Fixed size: tell the compositor our desired dimensions */
	xdg_toplevel_set_min_size(xdg_toplevel, win_w, win_h);
	xdg_toplevel_set_max_size(xdg_toplevel, win_w, win_h);
	wl_surface_commit(surface);

	/* Start status listener before the event loop so we populate state */
	pthread_t tid;
	pthread_create(&tid, NULL, status_thread, NULL);
	pthread_detach(tid);

	int wlfd = wl_display_get_fd(display);
	while (opt_running) {
		if (wl_display_flush(display) < 0 && errno != EAGAIN)
			break;

		struct pollfd fds[2] = {
			{ wlfd,		POLLIN, 0 },
			{ wake_pipe[0], POLLIN, 0 },
		};
		int r = poll(fds, 2, 500);
		if (r < 0 && errno != EINTR)
			break;

		if (fds[0].revents & POLLIN)
			if (wl_display_dispatch(display) < 0)
				break;

		if (fds[1].revents & POLLIN) {
			char buf[64];
			read(wake_pipe[0], buf, sizeof(buf));
			pthread_mutex_lock(&g_lock);

			int dirty  = g_dirty;
			g_dirty	   = 0;
			state_t st = g_state;
			pthread_mutex_unlock(&g_lock);

			if (dirty && surface_configured) {
				request_resize(&st);
				do_render(&st);
			}
		}
	}

	xdg_toplevel_destroy(xdg_toplevel);
	xdg_surface_destroy(xdg_surface);
	wl_surface_destroy(surface);
	wl_display_disconnect(display);

	return 0;
}
