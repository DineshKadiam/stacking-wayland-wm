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
 *     +---------------------------------------+
 *     | Desk 0    | Desk 1    | Desk 2    |...|  <- desk label band
 *     +---------------------------------------+
 *     | HDMI |DP-1| HDMI |DP-1| HDMI |DP-1|...|  <- monitor label band
 *     +---------------------------------------+
 *     |      |    |      |    |      |    |...|
 *     |      |    |      |    |      |    |...|  <- page grids (one
 *     |      |    |      |    |      |    |...|     per desk x monitor)
 *     +---------------------------------------+
 *
 * The monitor label band is only drawn when desktop_configuration is
 * "per-output" or "shared", since in "global" mode each desk has only
 * one shared view across all monitors.
 */

#include <cairo.h>
#include <cJSON.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <wayland-client.h>

#include "cow/attrs.h"
#include "xdg-shell-client-protocol.h"

#define MAX_DESKS 32
#define MAX_WINS 512
#define MAX_OUTS 8

#define INIT_W 420
#define INIT_H 120

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
	int  number;  /* 1-based ordinal from cow (raster-scan order). */
	int  current; /* 1 iff this is cow's "current_output". */
	int  current_desk;
	int  page_cols, page_rows;
	int  page_col, page_row;
	/*
	 * Raw sub-page viewport offsets in desk pixel space.  page_col
	 * and page_row are truncated indexes; page_vx / page_vy give
	 * the viewport's exact top-left.
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
	char   desktop_config[32]; /* "global" | "per-output" | "Shared" */
} state_t;

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
static int    opt_rows = 0;	/* -r ROWS; 0 = auto */
static int    opt_cols = 0;	/* -c COLS; 0 = auto */
static int    opt_current_only; /* -1: only show the active desk */

/*
 * -m NAME[,NAME,...] restricts the pager to a subset of monitors.
 *
 *   - No -m: every monitor.  Tabs sorted by cow's `number` field;
 *     windows on the page grid come from every monitor.
 *   - -m MON: single monitor.  No tab strip is drawn (it would have
 *     only one slot); windows on the page grid are filtered to that
 *     one monitor and click commands target it explicitly.
 *   - -m MON1,MON2,...: multiple monitors.  Tabs and windows are both
 *     restricted to the listed set, in the order given on the CLI.
 */
#define MAX_OPT_MONITORS 16
static char opt_monitors[MAX_OPT_MONITORS][64];
static int  opt_n_monitors;

static state_t	       g_state;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int	       g_dirty;
static int	       wake_pipe[2];

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

static void
hsv_to_rgb(double h, double s, double v, double *r, double *g, double *b)
{
	double c  = v * s;
	double hp = h / 60.0;
	double x  = c * (1.0 - fabs(fmod(hp, 2.0) - 1.0));
	double m  = v - c;
	double rr = 0, gg = 0, bb = 0;
	if (hp < 1) {
		rr = c;
		gg = x;
		bb = 0;
	} else if (hp < 2) {
		rr = x;
		gg = c;
		bb = 0;
	} else if (hp < 3) {
		rr = 0;
		gg = c;
		bb = x;
	} else if (hp < 4) {
		rr = 0;
		gg = x;
		bb = c;
	} else if (hp < 5) {
		rr = x;
		gg = 0;
		bb = c;
	} else {
		rr = c;
		gg = 0;
		bb = x;
	}
	*r = rr + m;
	*g = gg + m;
	*b = bb + m;
}

static uint32_t
str_hash(const char *s)
{
	uint32_t h = 2166136261u;
	while (*s)
		h = (h ^ (uint8_t)*s++) * 16777619u;
	return h;
}

/* Stable, muted colour per window id. */
static void
window_colour(const char *id, double *r, double *g, double *b)
{
	uint32_t h   = str_hash(id);
	double	 hue = (double)(h % 360);
	hsv_to_rgb(hue, 0.55, 0.78, r, g, b);
}

static const char *single_pinned_output(void);

/* True if the desktop configuration gives each output its own desks
 * (i.e. monitor sub-columns are meaningful). */
static int
is_per_output_mode(const state_t *st)
{
	return strcasecmp(st->desktop_config, "per-output") == 0 ||
	    strcasecmp(st->desktop_config, "shared") == 0;
}

/*
 * True when there's exactly one canonical "current desk" worth
 * highlighting on the pager.  That's the case when:
 *
 *   - the compositor runs in global desktop mode (all monitors share
 *     a single current_desk), or
 *   - the pager is restricted to a single monitor via -m, so its
 *     current_desk is unambiguous.
 *
 * When neither holds (multiple monitors each with their own
 * current_desk), the per-monitor information is already conveyed by
 * the tab strip and any single desk-level highlight would be at best
 * arbitrary and at worst misleading.
 */
static int
has_canonical_current_desk(const state_t *st)
{
	if (!is_per_output_mode(st))
		return 1;
	if (single_pinned_output() != NULL)
		return 1;
	return 0;
}

static int
tab_outputs_cmp(const void *a, const void *b)
{
	const pout_t *const *pa = a;
	const pout_t *const *pb = b;
	int		     na = (*pa)->number;
	int		     nb = (*pb)->number;
	if (na == 0 && nb == 0)
		return 0;
	/* Outputs with no number (talking to older cow) sort after
	 * numbered ones. */
	if (na == 0)
		return 1;
	if (nb == 0)
		return -1;
	return na - nb;
}

/*
 * Build the list of outputs the pager should consider "visible",
 * in display order.  This drives both the tab strip and the window-
 * placement filter.
 */
static int
tab_outputs(const state_t *st, const pout_t **out_arr, int max)
{
	int n = 0;
	if (opt_n_monitors > 0) {
		for (int j = 0; j < opt_n_monitors && n < max; j++) {
			for (int i = 0; i < st->n_outs; i++) {
				if (strcmp(st->outs[i].name, opt_monitors[j]) ==
				    0) {
					out_arr[n++] = &st->outs[i];
					break;
				}
			}
		}
	} else {
		for (int i = 0; i < st->n_outs && n < max; i++)
			out_arr[n++] = &st->outs[i];
		qsort(out_arr, (size_t)n, sizeof(*out_arr), tab_outputs_cmp);
	}
	return n;
}

static int
monitor_visible(const char *name)
{
	if (opt_n_monitors == 0)
		return 1;
	if (name == NULL || name[0] == '\0')
		return 1; /* IPC didn't tell us; don't drop the window. */
	for (int j = 0; j < opt_n_monitors; j++)
		if (strcmp(opt_monitors[j], name) == 0)
			return 1;
	return 0;
}

static const char *
single_pinned_output(void)
{
	if (opt_n_monitors == 1)
		return opt_monitors[0];
	return NULL;
}

/* Number of monitor tabs we would draw. */
static int
mon_tab_count(const state_t *st)
{
	const pout_t *tmp[MAX_OUTS];
	return tab_outputs(st, tmp, MAX_OUTS);
}

/* Return the "reference" output used for desk count, page grid, etc. */
static const pout_t *
reference_output(const state_t *st)
{
	const pout_t *tmp[MAX_OUTS];
	int	      n = tab_outputs(st, tmp, MAX_OUTS);
	if (n > 0)
		return tmp[0];
	return st->n_outs ? &st->outs[0] : NULL;
}

static void
compute_grid(int n_desks, int *rows, int *cols)
{
	int r = opt_rows;
	int c = opt_cols;

	if (n_desks < 1)
		n_desks = 1;

	if (r > 0 && c > 0) {
		/* both pinned */
	} else if (r > 0) {
		c = (n_desks + r - 1) / r;
	} else if (c > 0) {
		r = (n_desks + c - 1) / c;
	} else {
		/* Default: single row, all desks side-by-side. */
		r = 1;
		c = n_desks;
	}
	if (r < 1)
		r = 1;
	if (c < 1)
		c = 1;
	*rows = r;
	*cols = c;
}

typedef struct {
	int rows, cols;	     /* grid of desks */
	int desk_label_h;    /* "Desk N" band height */
	int mon_tab_h;	     /* monitor-tab strip height (0 if only
				one monitor and so the strip would be
				redundant) */
	int cell_w;	     /* width  of one desk cell */
	int cell_h;	     /* height of one desk cell */
	int chrome_h;	     /* desk_label_h + mon_tab_h */
	int page_grid_h;     /* height of the page grid area within a
				desk cell (= cell_h - chrome_h) */
	double page_cw;	     /* width  of one page cell */
	double page_ch;	     /* height of one page cell */
	double desk_font_px; /* font size for the "Desk N" label */
	double tab_font_px;  /* font size for monitor-tab labels */
} layout_t;

/*
 * Minimum / maximum font sizes (px).  The actual band height is
 * driven by these so the text remains legible at any window size --
 * if the user shrinks cowpager, the chrome simply takes up
 * proportionally more of the window rather than the labels becoming
 * unreadable.
 */
#define DESK_FONT_MIN 11.0
#define DESK_FONT_MAX 16.0
#define TAB_FONT_MIN 10.0
#define TAB_FONT_MAX 14.0

/* Convert a font size to the band height needed to host it with a
 * little vertical padding.  Mirrors the (font / band_h * 0.75)
 * relationship used at draw time so the band stays just tall enough. */
static int
band_h_for_font(double font_px)
{
	int h = (int)(font_px / 0.75 + 0.5);
	if (h < (int)font_px + 2)
		h = (int)font_px + 2;
	return h;
}

/* Pick a font size for a band of label-text, clamped to the readable
 * window.  At larger window heights the font grows proportionally up
 * to MAX; at very small heights it stops at MIN even if that means
 * the band eats a larger share of the window. */
static double
pick_font_px(int win_h_local, int divisor, double min_px, double max_px)
{
	double f = (double)win_h_local / divisor;
	if (f < min_px)
		f = min_px;
	if (f > max_px)
		f = max_px;
	return f;
}

/* Choose chrome band heights based on the readable-font floor.  The
 * resulting heights honour the floor even when the window is small. */
static void
choose_label_heights(int win_h_local, int has_mon_tabs, double *desk_font,
    int *desk_h, double *tab_font, int *tab_h)
{
	*desk_font = pick_font_px(win_h_local, 32, DESK_FONT_MIN,
	    DESK_FONT_MAX);
	*desk_h	   = band_h_for_font(*desk_font);

	if (has_mon_tabs) {
		*tab_font = pick_font_px(win_h_local, 40, TAB_FONT_MIN,
		    TAB_FONT_MAX);
		*tab_h	  = band_h_for_font(*tab_font);
	} else {
		*tab_font = 0;
		*tab_h	  = 0;
	}
}

static void
compute_layout(const state_t *st, layout_t *L)
{
	memset(L, 0, sizeof(*L));

	const pout_t *o	      = reference_output(st);
	int	      n_desks = o ? o->n_desks : 1;
	if (n_desks < 1)
		n_desks = 1;

	if (opt_current_only) {
		L->rows = 1;
		L->cols = 1;
	} else {
		compute_grid(n_desks, &L->rows, &L->cols);
	}

	/* Draw monitor tabs only when there's more than one effective
	 * monitor to distinguish.  With `-o NAME` or a single-output
	 * setup there is no useful information in the tab strip. */
	int n_tab_mons = mon_tab_count(st);
	choose_label_heights(win_h, n_tab_mons > 1, &L->desk_font_px,
	    &L->desk_label_h, &L->tab_font_px, &L->mon_tab_h);
	L->chrome_h = L->desk_label_h + L->mon_tab_h;

	L->cell_w = win_w / L->cols;
	L->cell_h = win_h / L->rows;

	L->page_grid_h = L->cell_h - L->chrome_h;
	if (L->page_grid_h < 1)
		L->page_grid_h = 1;

	int pcols = (o && o->page_cols > 0) ? o->page_cols : 1;
	int prows = (o && o->page_rows > 0) ? o->page_rows : 1;

	L->page_cw = (double)L->cell_w / pcols;
	L->page_ch = (double)L->page_grid_h / prows;
}

static void
desk_cell_origin(const layout_t *L, int row, int col, int *x, int *y)
{
	*x = col * L->cell_w;
	*y = row * L->cell_h;
}

static void
desk_index_to_rc(const layout_t *L, int idx, int *row, int *col)
{
	*row = idx / L->cols;
	*col = idx % L->cols;
}

static void
set_font(cairo_t *cr, double size, int bold)
{
	cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
	    bold ? CAIRO_FONT_WEIGHT_BOLD : CAIRO_FONT_WEIGHT_NORMAL);
	cairo_set_font_size(cr, size);
}

static void
draw_text_centered(cairo_t *cr, double x, double w, double baseline_y,
    const char *txt)
{
	cairo_text_extents_t te;
	cairo_text_extents(cr, txt, &te);
	double tx = x + (w - te.width) / 2.0 - te.x_bearing;
	cairo_move_to(cr, tx, baseline_y);
	cairo_show_text(cr, txt);
}

/*
 * Draw one desk's page-grid: fill page cells, draw window miniatures
 * for windows on this desk from every monitor, mark the current page
 * of each monitor that is on this desk, then overlay dashed interior
 * page gridlines.
 *
 *   sx/sy: top-left of the page-grid area (just below the chrome).
 *   sw/sh: size of the page-grid area.
 *
 * Window placement uses the desk-pixel coordinates (vx, vy) emitted in
 * the IPC `show windows` data; the page grid here is defined by the
 * reference output's pcols/prows/output_w/h (cow guarantees these are
 * uniform across outputs).
 */
static void
draw_desk_pages(cairo_t *cr, const state_t *st, __unused const layout_t *L,
    const pout_t *ref, const pdesk_t *d, int sx, int sy, int sw, int sh)
{
	int pcols = ref->page_cols > 0 ? ref->page_cols : 1;
	int prows = ref->page_rows > 0 ? ref->page_rows : 1;
	int ow	  = ref->output_w > 0 ? ref->output_w : 1920;
	int oh	  = ref->output_h > 0 ? ref->output_h : 1080;

	double cw = (double)sw / pcols;
	double ch = (double)sh / prows;

	/*
	 * Fill page-cell backgrounds.  A cell is "current" if any
	 * monitor whose current_desk == this desk has its page_col /
	 * page_row pointing at it.
	 */
	for (int pr = 0; pr < prows; pr++) {
		for (int pc = 0; pc < pcols; pc++) {
			double cx = sx + pc * cw;
			double cy = sy + pr * ch;

			int is_cur = 0;
			for (int oi = 0; oi < st->n_outs; oi++) {
				const pout_t *o = &st->outs[oi];
				if (o->current_desk == d->nr &&
				    o->page_col == pc && o->page_row == pr) {
					is_cur = 1;
					break;
				}
			}

			if (is_cur)
				cairo_set_source_rgba(cr, 0.22, 0.45, 0.22,
				    1.0);
			else
				cairo_set_source_rgba(cr, 0.28, 0.28, 0.31,
				    1.0);
			cairo_rectangle(cr, cx, cy, cw, ch);
			cairo_fill(cr);
		}
	}

	/*
	 * Window miniatures.  Windows on this desk are drawn for the
	 * set of monitors the user opted into via -m (or all monitors
	 * if -m isn't given).  This is the FvwmPager "everywhere"
	 * view across the visible monitor set.
	 */
	for (int wi = 0; wi < st->n_wins; wi++) {
		const pwin_t *w = &st->wins[wi];
		if (w->desk != d->nr)
			continue;
		if (!monitor_visible(w->output))
			continue;
		/* Desk pixel-space dimensions. */
		int desk_w = pcols * ow;
		int desk_h = prows * oh;
		if (desk_w <= 0 || desk_h <= 0)
			continue;
		if (w->vx < 0 || w->vx >= desk_w)
			continue;
		if (w->vy < 0 || w->vy >= desk_h)
			continue;

		double wx = sx + (double)w->vx / desk_w * sw;
		double wy = sy + (double)w->vy / desk_h * sh;
		double ww = (double)w->w / desk_w * sw;
		double wh = (double)w->h / desk_h * sh;

		if (ww < 3)
			ww = 3;
		if (wh < 3)
			wh = 3;
		if (wx + ww > sx + sw)
			ww = sx + sw - wx;
		if (wy + wh > sy + sh)
			wh = sy + sh - wy;

		int    foc = strcmp(w->id, st->focused_id) == 0;
		double r, g, b;
		if (foc) {
			r = 0.97;
			g = 0.78;
			b = 0.20;
		} else {
			window_colour(w->id, &r, &g, &b);
		}
		cairo_set_source_rgba(cr, r, g, b, 1.0);
		cairo_rectangle(cr, wx, wy, ww, wh);
		cairo_fill(cr);

		cairo_set_source_rgba(cr, 0.08, 0.08, 0.10, 1.0);
		cairo_set_line_width(cr, 1.0);
		cairo_set_dash(cr, NULL, 0, 0);
		cairo_rectangle(cr, wx + 0.5, wy + 0.5, ww - 1, wh - 1);
		cairo_stroke(cr);
	}

	/*
	 * Sub-page viewport overlays.  Draw a translucent yellow rect
	 * at each monitor's viewport position within this desk -- but
	 * only for monitors currently on this desk.
	 */
	double desk_w = (double)pcols * ow;
	double desk_h = (double)prows * oh;
	if (desk_w > 0 && desk_h > 0) {
		for (int oi = 0; oi < st->n_outs; oi++) {
			const pout_t *o = &st->outs[oi];
			if (o->current_desk != d->nr)
				continue;
			if (!monitor_visible(o->name))
				continue;
			double ax = sx + ((double)o->page_vx / desk_w) * sw;
			double ay = sy + ((double)o->page_vy / desk_h) * sh;
			double aw = ((double)ow / desk_w) * sw;
			double ah = ((double)oh / desk_h) * sh;
			cairo_set_source_rgba(cr, 0.95, 0.85, 0.30, 0.18);
			cairo_rectangle(cr, ax, ay, aw, ah);
			cairo_fill(cr);
			cairo_set_source_rgba(cr, 0.95, 0.85, 0.30, 0.95);
			cairo_set_line_width(cr, 1.0);
			cairo_set_dash(cr, NULL, 0, 0);
			cairo_rectangle(cr, ax + 0.5, ay + 0.5, aw - 1, ah - 1);
			cairo_stroke(cr);
		}
	}

	/*
	 * Interior page gridlines.  Dashed thin lines mark the
	 * boundaries between adjacent pages within this desk's page
	 * grid (cosmetic only -- the page cells themselves were filled
	 * above).  We draw them only on the interior so the outer
	 * desk-cell border remains a clean solid divider drawn by the
	 * caller in do_render().
	 */
	if (pcols > 1 || prows > 1) {
		double dashes[2] = { 2.0, 2.0 };
		/* Brighter than the page-cell background
		 * (rgba(0.28,0.28,0.31)) and fully opaque so the dashes
		 * are clearly visible without washing into the fill. */
		cairo_set_source_rgba(cr, 0.78, 0.78, 0.82, 1.0);
		cairo_set_line_width(cr, 1.0);
		cairo_set_dash(cr, dashes, 2, 0);
		for (int pc = 1; pc < pcols; pc++) {
			double x = sx + pc * cw + 0.5;
			cairo_move_to(cr, x, sy);
			cairo_line_to(cr, x, sy + sh);
			cairo_stroke(cr);
		}
		for (int pr = 1; pr < prows; pr++) {
			double y = sy + pr * ch + 0.5;
			cairo_move_to(cr, sx, y);
			cairo_line_to(cr, sx + sw, y);
			cairo_stroke(cr);
		}
		/* Reset to solid for any subsequent strokes. */
		cairo_set_dash(cr, NULL, 0, 0);
	}
}

/*
 * Draw the chrome (label band + monitor tab strip) at the top of a
 * single desk cell.
 *
 * The monitor tabs are evenly spaced left-to-right in monitor order
 * (filtered by -o / -m).  A tab is highlighted iff its monitor's
 * current_desk equals this desk -- a quick visual indicator of which
 * monitor is currently "on" each desk.  Match FvwmPager's
 * MonitorLabels look.
 */
static void
draw_desk_chrome(cairo_t *cr, const state_t *st, const layout_t *L,
    const pdesk_t *d, int dx, int dy, int is_active_desk)
{
	/* Desk-label band. */
	if (is_active_desk)
		cairo_set_source_rgba(cr, 0.20, 0.40, 0.70, 1.0);
	else
		cairo_set_source_rgba(cr, 0.22, 0.22, 0.26, 1.0);
	cairo_rectangle(cr, dx, dy, L->cell_w, L->desk_label_h);
	cairo_fill(cr);

	cairo_set_source_rgba(cr, 0.94, 0.94, 0.96, 1.0);
	set_font(cr, L->desk_font_px, 1);

	char lbl[32];
	snprintf(lbl, sizeof(lbl), "Desk %d", d->nr);
	draw_text_centered(cr, dx, L->cell_w,
	    dy + L->desk_label_h - L->desk_label_h * 0.22, lbl);

	/* Monitor-tab strip. */
	if (L->mon_tab_h <= 0)
		return;

	const pout_t *tab[MAX_OUTS];
	int	      n_tab = tab_outputs(st, tab, MAX_OUTS);
	if (n_tab <= 0)
		return;

	int    tab_y = dy + L->desk_label_h;
	double tw    = (double)L->cell_w / n_tab;

	set_font(cr, L->tab_font_px, 1);

	/*
	 * Work out the (monitor, desk) cell that should carry the
	 * green "you are here" highlight.
	 *
	 *   - If a window is focused, use its (output, desk): this
	 *     captures the user's actual locus of attention.
	 *   - Otherwise fall back to cow's current_output (the monitor
	 *     where the pointer is) and that output's current_desk.
	 *     This keeps the highlight visible even after the user
	 *     switches to a desk with no windows on it, which is when
	 *     no `focused` field is emitted.
	 *
	 * Either lookup populates `foc_out_name` and `foc_desk`; when
	 * neither yields a result the FOCUSED state is unreachable.
	 */
	const char *foc_out_name = NULL;
	int	    foc_desk	 = -1;

	if (st->focused_id[0]) {
		for (int wi = 0; wi < st->n_wins; wi++) {
			if (strcmp(st->wins[wi].id, st->focused_id) == 0) {
				foc_out_name = st->wins[wi].output;
				foc_desk     = st->wins[wi].desk;
				break;
			}
		}
	}
	if (foc_out_name == NULL || foc_out_name[0] == '\0') {
		for (int oi = 0; oi < st->n_outs; oi++) {
			if (st->outs[oi].current) {
				foc_out_name = st->outs[oi].name;
				foc_desk     = st->outs[oi].current_desk;
				break;
			}
		}
	}

	for (int i = 0; i < n_tab; i++) {
		double tx = dx + i * tw;

		/*
		 * Four-state tab highlight, brightest to dimmest:
		 *
		 *   FOCUSED this monitor holds the focused window AND
		 *           that window is on this desk (the "you are
		 *           here" cell -- bright green).
		 *   ACTIVE  monitor's current_desk == this desk (but
		 *           focus is elsewhere).
		 *   PRESENT monitor is elsewhere, but this desk has at
		 *           least one window belonging to that monitor.
		 *   EMPTY   monitor is elsewhere AND has no windows on
		 *           this desk.
		 */
		enum {
			TAB_FOCUSED,
			TAB_ACTIVE,
			TAB_PRESENT,
			TAB_EMPTY
		} tab_state;

		int has_focus = foc_desk == d->nr && foc_out_name != NULL &&
		    foc_out_name[0] && strcmp(foc_out_name, tab[i]->name) == 0;

		if (has_focus) {
			tab_state = TAB_FOCUSED;
		} else if (tab[i]->current_desk == d->nr) {
			tab_state = TAB_ACTIVE;
		} else {
			tab_state = TAB_EMPTY;
			for (int wi = 0; wi < st->n_wins; wi++) {
				const pwin_t *w = &st->wins[wi];
				if (w->desk != d->nr)
					continue;
				if (w->output[0] &&
				    strcmp(w->output, tab[i]->name) == 0) {
					tab_state = TAB_PRESENT;
					break;
				}
			}
		}

		switch (tab_state) {
		case TAB_FOCUSED:
			cairo_set_source_rgba(cr, 0.24, 0.55, 0.28, 1.0);
			break;
		case TAB_ACTIVE:
			cairo_set_source_rgba(cr, 0.42, 0.42, 0.48, 1.0);
			break;
		case TAB_PRESENT:
			cairo_set_source_rgba(cr, 0.28, 0.30, 0.36, 1.0);
			break;
		case TAB_EMPTY:
			cairo_set_source_rgba(cr, 0.14, 0.14, 0.17, 1.0);
			break;
		}
		cairo_rectangle(cr, tx, tab_y, tw, L->mon_tab_h);
		cairo_fill(cr);

		/* Solid 1px borders around each tab so they read as
		 * distinct slots even when adjacent tabs are both
		 * highlighted. */
		cairo_set_source_rgba(cr, 0.04, 0.04, 0.06, 1.0);
		cairo_set_line_width(cr, 1.0);
		cairo_set_dash(cr, NULL, 0, 0);
		cairo_rectangle(cr, tx + 0.5, tab_y + 0.5, tw - 1,
		    L->mon_tab_h - 1);
		cairo_stroke(cr);

		/* Subtle accent stripe along the bottom of "present"
		 * tabs -- a quiet hint that this monitor has work
		 * parked on this desk without competing with the bold
		 * active highlight. */
		if (tab_state == TAB_PRESENT) {
			cairo_set_source_rgba(cr, 0.55, 0.70, 0.95, 1.0);
			cairo_rectangle(cr, tx + 1, tab_y + L->mon_tab_h - 2.5,
			    tw - 2, 1.5);
			cairo_fill(cr);
		}

		/*
		 * Label.  Prefer the monitor's cow-assigned ordinal
		 * (the `number` field) when the full name would
		 * overflow the slot, so the digits cowpager shows are
		 * the same ones `show output` reports.  Fall back to
		 * the tab index if the IPC didn't deliver a number
		 * (e.g. talking to an older cow).
		 */
		const char	    *name = tab[i]->name;
		char		     short_name[16];
		cairo_text_extents_t te;
		cairo_text_extents(cr, name, &te);
		if (te.width > tw - 4) {
			int n = tab[i]->number > 0 ? tab[i]->number : i + 1;
			snprintf(short_name, sizeof(short_name), "%d", n);
			name = short_name;
		}

		switch (tab_state) {
		case TAB_FOCUSED:
			cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 1.0);
			break;
		case TAB_ACTIVE:
			cairo_set_source_rgba(cr, 0.98, 0.98, 1.0, 1.0);
			break;
		case TAB_PRESENT:
			cairo_set_source_rgba(cr, 0.85, 0.88, 0.95, 1.0);
			break;
		case TAB_EMPTY:
			cairo_set_source_rgba(cr, 0.55, 0.55, 0.60, 1.0);
			break;
		}
		draw_text_centered(cr, tx, tw,
		    tab_y + L->mon_tab_h - L->mon_tab_h * 0.22, name);
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

	/* Outer background */
	cairo_set_source_rgba(cr, 0.06, 0.06, 0.08, 1.0);
	cairo_paint(cr);

	if (st->n_outs == 0)
		goto commit;

	layout_t L;
	compute_layout(st, &L);

	const pout_t *ref = reference_output(st);
	if (!ref)
		goto commit;

	/* Build the list of desk indices we will draw, in grid order. */
	int desk_indices[MAX_DESKS];
	int n_visible = 0;
	if (opt_current_only) {
		for (int i = 0; i < ref->n_desks; i++) {
			if (ref->desks[i].nr == ref->current_desk) {
				desk_indices[n_visible++] = i;
				break;
			}
		}
	} else {
		for (int i = 0; i < ref->n_desks && n_visible < L.rows * L.cols;
		    i++)
			desk_indices[n_visible++] = i;
	}

	for (int v = 0; v < n_visible; v++) {
		int row, col;
		desk_index_to_rc(&L, v, &row, &col);
		int dx, dy;
		desk_cell_origin(&L, row, col, &dx, &dy);

		const pdesk_t *d = &ref->desks[desk_indices[v]];
		/*
		 * Highlight a desk as "active" only when there's a
		 * single canonical current desk to point at.  In
		 * per-output mode with multiple monitors visible, each
		 * monitor has its own current_desk and the per-monitor
		 * info is conveyed through the tab strip; a blanket
		 * desk-level highlight there would be arbitrary.
		 */
		int is_active = has_canonical_current_desk(st) &&
		    d->nr == ref->current_desk;

		/* Desk cell background */
		cairo_set_source_rgba(cr, 0.16, 0.16, 0.18, 1.0);
		cairo_rectangle(cr, dx, dy, L.cell_w, L.cell_h);
		cairo_fill(cr);

		/* Chrome: desk label + monitor tab strip. */
		draw_desk_chrome(cr, st, &L, d, dx, dy, is_active);

		/* Page grid: one unified canvas per desk, windows from
		 * all monitors. */
		int gy = dy + L.chrome_h;
		int gh = L.cell_h - L.chrome_h;
		if (gh < 1)
			gh = 1;
		draw_desk_pages(cr, st, &L, ref, d, dx, gy, L.cell_w, gh);

		/* Active-desk highlight frame */
		if (is_active) {
			cairo_set_source_rgba(cr, 0.30, 0.62, 0.95, 1.0);
			cairo_set_line_width(cr, 2.0);
			cairo_rectangle(cr, dx + 1, dy + 1, L.cell_w - 2,
			    L.cell_h - 2);
			cairo_stroke(cr);
		}

		/*
		 * Solid desk dividers.  These visually separate one desk
		 * from the next and contrast with the dashed page
		 * gridlines drawn inside each desk's page area.
		 */
		cairo_set_dash(cr, NULL, 0, 0);
		cairo_set_source_rgba(cr, 0.02, 0.02, 0.04, 1.0);
		cairo_set_line_width(cr, 1.5);
		if (col > 0) {
			cairo_move_to(cr, dx + 0.5, dy);
			cairo_line_to(cr, dx + 0.5, dy + L.cell_h);
			cairo_stroke(cr);
		}
		if (row > 0) {
			cairo_move_to(cr, dx, dy + 0.5);
			cairo_line_to(cr, dx + L.cell_w, dy + 0.5);
			cairo_stroke(cr);
		}
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

/*
 * Convert pointer coords to a visible-desk index, and identify which
 * piece of chrome (label band, monitor tab strip, page grid) the
 * pointer is over.  All "out" values are -1 / 0 on miss.
 *
 *   *vis_idx    : index into the visible-desks array.
 *   *tab_idx    : if the pointer is in the monitor-tab strip, the
 *                 tab slot index (0..mon_tab_count-1).  -1 otherwise.
 *   *in_page    : 1 if the pointer is in the page-grid area, else 0.
 */
static void
locate(const state_t *st, double x, double y, const layout_t *L, int *vis_idx,
    int *tab_idx, int *in_page)
{
	*vis_idx = -1;
	*tab_idx = -1;
	*in_page = 0;
	if (x < 0 || y < 0 || x >= win_w || y >= win_h)
		return;

	int col = (int)(x / L->cell_w);
	int row = (int)(y / L->cell_h);
	if (col < 0 || col >= L->cols || row < 0 || row >= L->rows)
		return;
	*vis_idx = row * L->cols + col;

	int    dx_local = col * L->cell_w;
	int    dy_local = row * L->cell_h;
	double ly	= y - dy_local;

	if (ly < L->desk_label_h) {
		/* Inside the desk label band -- treat as desk-only hit. */
		return;
	}
	if (ly < L->chrome_h) {
		/* Inside the monitor tab strip. */
		int n_tab = mon_tab_count(st);
		if (n_tab > 0) {
			double tw = (double)L->cell_w / n_tab;
			int    ti = (int)((x - dx_local) / tw);
			if (ti < 0)
				ti = 0;
			if (ti >= n_tab)
				ti = n_tab - 1;
			*tab_idx = ti;
		}
		return;
	}
	*in_page = 1;
}

/* Resolve the visible-desk index to a real desk_t pointer in ref's
 * desks[].  Returns NULL on miss. */
static const pdesk_t *
resolve_visible_desk(const pout_t *ref, int vis_idx)
{
	if (vis_idx < 0)
		return NULL;
	if (opt_current_only) {
		if (vis_idx != 0)
			return NULL;
		for (int i = 0; i < ref->n_desks; i++)
			if (ref->desks[i].nr == ref->current_desk)
				return &ref->desks[i];
		return NULL;
	}
	if (vis_idx >= ref->n_desks)
		return NULL;
	return &ref->desks[vis_idx];
}

static const pwin_t *
hit_test_window(const state_t *st, double x, double y)
{
	if (!st->n_outs)
		return NULL;
	layout_t L;
	compute_layout(st, &L);

	const pout_t *ref = reference_output(st);
	if (!ref)
		return NULL;

	int vis_idx, tab_idx, in_page;
	locate(st, x, y, &L, &vis_idx, &tab_idx, &in_page);
	if (!in_page)
		return NULL;

	const pdesk_t *d = resolve_visible_desk(ref, vis_idx);
	if (d == NULL)
		return NULL;

	int row, col;
	desk_index_to_rc(&L, vis_idx, &row, &col);
	int dx, dy_local;
	desk_cell_origin(&L, row, col, &dx, &dy_local);

	int sx = dx;
	int sy = dy_local + L.chrome_h;
	int sw = L.cell_w;
	int sh = L.cell_h - L.chrome_h;

	int pcols  = ref->page_cols > 0 ? ref->page_cols : 1;
	int prows  = ref->page_rows > 0 ? ref->page_rows : 1;
	int ow	   = ref->output_w > 0 ? ref->output_w : 1920;
	int oh	   = ref->output_h > 0 ? ref->output_h : 1080;
	int desk_w = pcols * ow;
	int desk_h = prows * oh;
	if (desk_w <= 0 || desk_h <= 0)
		return NULL;

	const pwin_t *found = NULL;
	for (int wi = 0; wi < st->n_wins; wi++) {
		const pwin_t *w = &st->wins[wi];
		if (w->desk != d->nr)
			continue;
		if (!monitor_visible(w->output))
			continue;
		if (w->vx < 0 || w->vx >= desk_w)
			continue;
		if (w->vy < 0 || w->vy >= desk_h)
			continue;
		double wx_ = sx + (double)w->vx / desk_w * sw;
		double wy_ = sy + (double)w->vy / desk_h * sh;
		double ww  = (double)w->w / desk_w * sw;
		double wh  = (double)w->h / desk_h * sh;
		if (ww < 3)
			ww = 3;
		if (wh < 3)
			wh = 3;
		if (x >= wx_ && x < wx_ + ww && y >= wy_ && y < wy_ + wh)
			found = w;
	}
	return found;
}

/*
 * Page-area click hit test.  Returns 1 on a desk-area hit, with:
 *   *desk_nr   : the desk number under the pointer (may be set even
 *                when the click was in the chrome).
 *   *pc, *pr   : page column/row within the desk's page grid, or -1
 *                if the click wasn't in the page-grid area.
 *   *tab_idx   : if the click landed on a monitor-tab in the chrome,
 *                the tab index (0..mon_tab_count-1); -1 otherwise.
 *   *out_for_cmd : if a per-monitor command should be emitted (tab
 *                  click targets that monitor; otherwise NULL).
 */
static int
hit_test_page(const state_t *st, double x, double y, int *desk_nr, int *pc_out,
    int *pr_out, int *tab_out, const char **out_for_cmd)
{
	*desk_nr = -1;
	*pc_out = *pr_out = -1;
	*tab_out	  = -1;
	*out_for_cmd	  = NULL;

	if (!st->n_outs)
		return 0;

	layout_t L;
	compute_layout(st, &L);

	const pout_t *ref = reference_output(st);
	if (!ref)
		return 0;

	int vis_idx, tab_idx, in_page;
	locate(st, x, y, &L, &vis_idx, &tab_idx, &in_page);
	if (vis_idx < 0)
		return 0;

	const pdesk_t *d = resolve_visible_desk(ref, vis_idx);
	if (d == NULL)
		return 0;
	*desk_nr = d->nr;

	if (tab_idx >= 0) {
		/* Click in a monitor tab.  Look up which monitor that
		 * tab corresponds to and return its name. */
		const pout_t *tab[MAX_OUTS];
		int	      n_tab = tab_outputs(st, tab, MAX_OUTS);
		if (tab_idx < n_tab) {
			*tab_out     = tab_idx;
			*out_for_cmd = tab[tab_idx]->name;
		}
		return 1;
	}

	/*
	 * In per-output mode, pin click commands to the single -m
	 * monitor when one is specified.  With multiple -m monitors
	 * there's no unambiguous target so we leave it unset and let
	 * cow apply commands to the focused output.
	 */
	if (is_per_output_mode(st)) {
		const char *pin = single_pinned_output();
		if (pin)
			*out_for_cmd = pin;
	}

	if (!in_page)
		return 1;

	int pcols = ref->page_cols > 0 ? ref->page_cols : 1;
	int prows = ref->page_rows > 0 ? ref->page_rows : 1;

	int row, col;
	desk_index_to_rc(&L, vis_idx, &row, &col);

	int dx, dy_local;
	desk_cell_origin(&L, row, col, &dx, &dy_local);

	int sx = dx;
	int sy = dy_local + L.chrome_h;
	int sw = L.cell_w;
	int sh = L.cell_h - L.chrome_h;
	if (sw <= 0 || sh <= 0)
		return 1;

	int pc = (int)((x - sx) / ((double)sw / pcols));
	int pr = (int)((y - sy) / ((double)sh / prows));
	if (pc >= 0 && pc < pcols && pr >= 0 && pr < prows) {
		*pc_out = pc;
		*pr_out = pr;
	}
	return 1;
}

static void
xdg_surface_configure(__unused void *data, struct xdg_surface *xs,
    uint32_t serial)
{
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
xdg_toplevel_configure(__unused void *data, __unused struct xdg_toplevel *tl,
    int32_t w, int32_t h, __unused struct wl_array *states)
{
	if (w > 0)
		win_w = w;
	if (h > 0)
		win_h = h;
}

static void
xdg_toplevel_close(__unused void *data, __unused struct xdg_toplevel *tl)
{
	opt_running = 0;
}

static const struct xdg_toplevel_listener xdg_toplevel_listener = {
	.configure = xdg_toplevel_configure,
	.close	   = xdg_toplevel_close,
};

static void
xdg_wm_base_ping(__unused void *data, struct xdg_wm_base *base, uint32_t serial)
{
	xdg_wm_base_pong(base, serial);
}

static const struct xdg_wm_base_listener xdg_wm_base_listener = {
	.ping = xdg_wm_base_ping,
};

static void
ptr_enter(__unused void *d, __unused struct wl_pointer *p, __unused uint32_t s,
    __unused struct wl_surface *sf, wl_fixed_t x, wl_fixed_t y)
{
	ptr_x = wl_fixed_to_double(x);
	ptr_y = wl_fixed_to_double(y);
}

static void
ptr_leave(__unused void *d, __unused struct wl_pointer *p, __unused uint32_t s,
    __unused struct wl_surface *sf)
{
	return;
}

static void
ptr_motion(__unused void *d, __unused struct wl_pointer *p, __unused uint32_t t,
    wl_fixed_t x, wl_fixed_t y)
{
	ptr_x = wl_fixed_to_double(x);
	ptr_y = wl_fixed_to_double(y);
}

static void
ptr_button(__unused void *d, __unused struct wl_pointer *p, __unused uint32_t s,
    __unused uint32_t t, __unused uint32_t btn, uint32_t state)
{
	if (state != WL_POINTER_BUTTON_STATE_PRESSED)
		return;

	pthread_mutex_lock(&g_lock);
	state_t st = g_state;
	pthread_mutex_unlock(&g_lock);

	/* Window miniature wins over page click.  Navigate to the
	 * window's desk/page first so fullscreen/maximized windows on a
	 * different page actually become visible; then focus it.
	 */
	const pwin_t *hw = hit_test_window(&st, ptr_x, ptr_y);
	if (hw && hw->id[0]) {
		/* Find which output the window lives on.  Fall back to
		 * the pinned/reference output if the IPC didn't tell us.
		 */
		const pout_t *target = NULL;
		if (hw->output[0]) {
			for (int i = 0; i < st.n_outs; i++)
				if (strcmp(st.outs[i].name, hw->output) == 0) {
					target = &st.outs[i];
					break;
				}
		}
		if (!target)
			target = reference_output(&st);

		int	    per_out = is_per_output_mode(&st);
		const char *tname   = (target && per_out) ? target->name : NULL;

		/* Switch desk if the window isn't on the current one. */
		if (target && hw->desk != target->current_desk) {
			char cmd[128];
			if (tname)
				snprintf(cmd, sizeof(cmd), "desk -t %s -d %d",
				    tname, hw->desk);
			else
				snprintf(cmd, sizeof(cmd), "desk -d %d",
				    hw->desk);
			ipc_send(cmd);
		}

		/* Scroll to the window's page within that desk. */
		if (target) {
			int ow	= target->output_w > 0 ? target->output_w : 1;
			int oh	= target->output_h > 0 ? target->output_h : 1;
			int wpc = hw->vx / ow;
			int wpr = hw->vy / oh;
			if (wpc < 0)
				wpc = 0;
			if (wpr < 0)
				wpr = 0;

			int need_scroll = (hw->desk != target->current_desk) ||
			    (wpc != target->page_col) ||
			    (wpr != target->page_row);
			if (need_scroll) {
				char cmd[128];
				if (tname)
					snprintf(cmd, sizeof(cmd),
					    "scroll -t %s -a %d,%d", tname, wpc,
					    wpr);
				else
					snprintf(cmd, sizeof(cmd),
					    "scroll -a %d,%d", wpc, wpr);
				ipc_send(cmd);
			}
		}

		char cmd[256];
		snprintf(cmd, sizeof(cmd), "focus -i %s", hw->id);
		ipc_send(cmd);
		return;
	}

	int	    desk_nr, pc, pr, tab_idx;
	const char *out_name = NULL;
	if (!hit_test_page(&st, ptr_x, ptr_y, &desk_nr, &pc, &pr, &tab_idx,
		&out_name))
		return;

	/*
	 * Tab click: move that monitor to the clicked desk (and no
	 * scroll).  This matches FvwmPager's monitor-label
	 * interaction in `per-monitor` mode -- clicking a monitor's
	 * label moves only that monitor between virtual desks.
	 */
	if (tab_idx >= 0 && out_name && desk_nr >= 0) {
		const pout_t *target = NULL;
		for (int i = 0; i < st.n_outs; i++)
			if (strcmp(st.outs[i].name, out_name) == 0) {
				target = &st.outs[i];
				break;
			}
		if (target && desk_nr != target->current_desk) {
			char cmd[128];
			if (is_per_output_mode(&st))
				snprintf(cmd, sizeof(cmd), "desk -t %s -d %d",
				    out_name, desk_nr);
			else
				snprintf(cmd, sizeof(cmd), "desk -d %d",
				    desk_nr);
			ipc_send(cmd);
		}
		return;
	}

	/*
	 * Page-area click: switch the active output (or the -o pinned
	 * output) to this desk/page.
	 */
	if (desk_nr >= 0) {
		const pout_t *target = NULL;
		if (out_name) {
			for (int i = 0; i < st.n_outs; i++)
				if (strcmp(st.outs[i].name, out_name) == 0) {
					target = &st.outs[i];
					break;
				}
		}
		if (!target)
			target = reference_output(&st);
		if (target && desk_nr != target->current_desk) {
			char cmd[128];
			if (out_name && is_per_output_mode(&st))
				snprintf(cmd, sizeof(cmd), "desk -t %s -d %d",
				    out_name, desk_nr);
			else
				snprintf(cmd, sizeof(cmd), "desk -d %d",
				    desk_nr);
			ipc_send(cmd);
		}
	}

	if (pc >= 0 && pr >= 0) {
		char cmd[128];
		if (out_name && is_per_output_mode(&st))
			snprintf(cmd, sizeof(cmd), "scroll -t %s -a %d,%d",
			    out_name, pc, pr);
		else
			snprintf(cmd, sizeof(cmd), "scroll -a %d,%d", pc, pr);
		ipc_send(cmd);
	}
}

static void
ptr_axis(__unused void *d, __unused struct wl_pointer *p, __unused uint32_t t,
    __unused uint32_t a, __unused wl_fixed_t v)
{
	return;
}
static void
ptr_frame(__unused void *d, __unused struct wl_pointer *p)
{
	return;
}
static void
ptr_axis_source(__unused void *d, __unused struct wl_pointer *p,
    __unused uint32_t s)
{
	return;
}
static void
ptr_axis_stop(__unused void *d, __unused struct wl_pointer *p,
    __unused uint32_t t, __unused uint32_t a)
{
	return;
}
static void
ptr_axis_discrete(__unused void *d, __unused struct wl_pointer *p,
    __unused uint32_t a, __unused int32_t v)
{
	return;
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
seat_caps(__unused void *d, struct wl_seat *s, uint32_t caps)
{
	if ((caps & WL_SEAT_CAPABILITY_POINTER) && !pointer) {
		pointer = wl_seat_get_pointer(s);
		wl_pointer_add_listener(pointer, &ptr_listener, NULL);
	}
}

static void
seat_name(__unused void *d, __unused struct wl_seat *s, __unused const char *n)
{
	return;
}

static const struct wl_seat_listener seat_listener = {
	.capabilities = seat_caps,
	.name	      = seat_name,
};

static void
reg_global(__unused void *d, struct wl_registry *reg, uint32_t name,
    const char *iface, __unused uint32_t ver)
{
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
reg_remove(__unused void *d, __unused struct wl_registry *r,
    __unused uint32_t n)
{
	return;
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

	cJSON *dcfg = cJSON_GetObjectItem(root, "desktop_configuration");
	if (cJSON_IsString(dcfg))
		strncpy(st.desktop_config, dcfg->valuestring,
		    sizeof(st.desktop_config) - 1);
	else
		strncpy(st.desktop_config, "global",
		    sizeof(st.desktop_config) - 1);

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
		GN("number", number)
		GN("current_desk", current_desk)
		v = cJSON_GetObjectItem(out, "current");
		if (cJSON_IsBool(v))
			po->current = cJSON_IsTrue(v) ? 1 : 0;
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

	cJSON *foc = cJSON_GetObjectItem(root, "focused");
	if (foc) {
		cJSON *fid = cJSON_GetObjectItem(foc, "id");
		if (cJSON_IsString(fid))
			strncpy(st.focused_id, fid->valuestring,
			    sizeof(st.focused_id) - 1);
	}
	cJSON_Delete(root);

	pthread_mutex_lock(&g_lock);
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

static void *
status_thread(__unused void *arg)
{
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
	while ((opt = getopt(argc, argv, "r:c:m:1h")) != -1) {
		switch (opt) {
		case 'r':
			opt_rows = atoi(optarg);
			if (opt_rows < 1)
				opt_rows = 1;
			break;
		case 'c':
			opt_cols = atoi(optarg);
			if (opt_cols < 1)
				opt_cols = 1;
			break;
		case 'm': {
			/* Comma-separated list of monitor names.  Preserved
			 * order defines display order in the pager.
			 */
			char *dup = strdup(optarg);
			if (dup == NULL) {
				perror("strdup");
				return 1;
			}
			char *save = NULL;
			char *tok  = strtok_r(dup, ",", &save);
			while (
			    tok != NULL && opt_n_monitors < MAX_OPT_MONITORS) {
				while (*tok == ' ' || *tok == '\t')
					tok++;
				size_t L = strlen(tok);
				while (L > 0 &&
				    (tok[L - 1] == ' ' || tok[L - 1] == '\t'))
					tok[--L] = '\0';
				if (*tok != '\0') {
					strncpy(opt_monitors[opt_n_monitors],
					    tok, sizeof(opt_monitors[0]) - 1);
					opt_n_monitors++;
				}
				tok = strtok_r(NULL, ",", &save);
			}
			free(dup);
			break;
		}
		case '1':
			opt_current_only = 1;
			break;
		case 'h':
			fprintf(stderr,
			    "usage: cowpager [-r rows] [-c cols] "
			    "[-m mon[,mon...]] [-1] [-h]\n"
			    "  -r ROWS   arrange desks in ROWS rows\n"
			    "  -c COLS   arrange desks in COLS columns\n"
			    "  -m LIST   restrict pager to the listed\n"
			    "            monitors (comma-separated); both\n"
			    "            the tab strip and windows on the\n"
			    "            page grid are filtered.  A single\n"
			    "            monitor suppresses the tab strip\n"
			    "  -1        show only the currently active desk\n"
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

	win_w = INIT_W;
	win_h = INIT_H;

	surface	    = wl_compositor_create_surface(compositor);
	xdg_surface = xdg_wm_base_get_xdg_surface(xdg_wm_base, surface);
	xdg_surface_add_listener(xdg_surface, &xdg_surface_listener, NULL);
	xdg_toplevel = xdg_surface_get_toplevel(xdg_surface);
	xdg_toplevel_add_listener(xdg_toplevel, &xdg_toplevel_listener, NULL);
	xdg_toplevel_set_title(xdg_toplevel, "cowpager");
	xdg_toplevel_set_app_id(xdg_toplevel, "cowpager");

	/* No min/max -- cowpager reflows to whatever size the user picks. */
	wl_surface_commit(surface);

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

			if (dirty && surface_configured)
				do_render(&st);
		}
	}

	xdg_toplevel_destroy(xdg_toplevel);
	xdg_surface_destroy(xdg_surface);
	wl_surface_destroy(surface);
	wl_display_disconnect(display);

	return 0;
}
