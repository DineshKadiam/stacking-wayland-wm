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

#include "cow/attrs.h"
#include "cow/colour.h"
#include "cow/image_cache.h"
#include "cow/module.h"
#include "cow/module_config.h"
#include "cow/module_cursor.h"
#include "cow/module_draw.h"
#include "cow/module_theme.h"
#include "cow/text.h"
#include "xdg-shell-client-protocol.h"

#define PROG "cowpager"
#define SHOW_INDEX_ENV "COWPAGER_SHOW_INDEX"

#define MAX_DESKS 32
#define MAX_WINS 512
#define MAX_OUTS 8
#define MAX_FIELDS 8
#define MAX_SELECTED_PAGERS 16

#define INIT_W 420
#define INIT_H 120

typedef struct {
	int  desk;
	int  vx, vy, w, h;
	int  icon_vx, icon_vy, icon_w, icon_h;
	int  focused;
	int  iconified;
	char id[128];
	char output[64];
	char app_id[64];
	char icon[1024];
} pwin_t;

typedef struct {
	int nr;
	int window_count;
	int active;
	char name[64];
} pdesk_t;

typedef struct {
	char name[64];
	int  number;  /* 1-based ordinal from cow (raster-scan order). */
	int  current; /* 1 if this is cow's "current_output". */
	int  current_desk;
	int  output_x, output_y;
	int  page_cols, page_rows;
	int  page_col, page_row;
	/*
	 * Raw sub-page viewport offsets in desk pixel space.  page_col
	 * and page_row are truncated indexes; page_vx / page_vy give
	 * the viewport's exact top-left.
	 */
	int	page_vx, page_vy;
	int	output_w, output_h;
	int	has_background;
	float	background[4];
	pdesk_t desks[MAX_DESKS];
	int	n_desks;
} pout_t;

typedef struct {
	int x, y, w, h;
	int page_cols, page_rows;
	int tracked;
	const pout_t *tracked_output;
} canvas_t;

typedef struct {
	pout_t outs[MAX_OUTS];
	int    n_outs;
	pwin_t wins[MAX_WINS];
	int    n_wins;
	char   desktop_config[32]; /* "global" | "per-output" | "Shared" */
	char   user_output[64];
	struct cow_module_theme theme;
} state_t;

struct pager_config {
	int rows;
	int cols;
	int current_only;
	int desk_range_set;
	int desk_first;
	int desk_last;
	int verbose;
	int desktop_labels;
	int monitor_labels;
	int label_highlight;
	int desktop_scale;
	int window_icons;
	int window_icon_size;
	float background[4];
	float desk_bg[4];
	float page_bg[4];
	float page_active_bg[4];
	float label_bg[4];
	float label_active_bg[4];
	float label_fg[4];
	float label_inactive_fg[4];
	float window_focused_bg[4];
	int   background_set;
	int   desk_bg_set;
	int   page_bg_set;
	int   page_active_bg_set;
	int   label_bg_set;
	int   label_active_bg_set;
	int   label_fg_set;
	int   label_inactive_fg_set;
	int   window_focused_bg_set;
};

struct pager_selection {
	char *name;
	int   seen;
};

static struct wl_display    *display;
static struct wl_compositor *compositor;
static struct wl_shm	    *wl_shm;
static struct xdg_wm_base   *xdg_wm_base;
static struct wl_seat	    *seat;
static struct wl_pointer    *pointer;
static struct cow_module_cursor cursor;
static struct wl_surface    *surface;
static struct xdg_surface   *xdg_surface;
static struct xdg_toplevel  *xdg_toplevel;

/*
 * Tracked wl_outputs.  We bind every output the compositor advertises
 * so that wl_surface.enter / wl_surface.leave events on cowpager's
 * own toplevel.
 */
struct cp_output {
	struct wl_output *wl_output;
	uint32_t	  global_name;
	char		  name[64];
	int		  on_surface; /* 1 if cowpager's surface entered it */
};

#define CP_MAX_TRACKED_OUTPUTS 16
static struct cp_output cp_outputs[CP_MAX_TRACKED_OUTPUTS];
static int		cp_n_outputs;

/*
 * Sticky cache of the last "user-relevant" current output observed.
 * Updated only when cow reports current=true
 * for an output that cowpager's surface is NOT currently sitting on.
 */
static char cp_user_output[64];

static int    surface_configured;
static int    win_w, win_h; /* current window size from configure */
static int    configured_w_seen, configured_h_seen;
static double ptr_x, ptr_y;
static int    opt_running = 1;
static int    opt_rows = 0;	/* -r ROWS; 0 = auto */
static int    opt_cols = 0;	/* -c COLS; 0 = auto */
static int    opt_current_only; /* -1: only show the active desk */
static int    opt_desk_range_set;
static int    opt_desk_first;
static int    opt_desk_last;
static int    initial_size_done;
static struct pager_config cfg;

static struct cow_image_cache icon_cache;

/*
 * Drag-an-overlay-plate handles.  The page-grid drawing publishes one
 * record per monitor name plate it renders; press-based hit-testing
 * uses these to decide whether a press starts a monitor drag (rather
 * than a plain page click or a window-thumbnail click).
 */
struct overlay_handle {
	char   name[64];       /* monitor name (output->name) */
	double px, py, pw, ph; /* plate rectangle in cowpager pixels */
};

#define MAX_OVERLAY_HANDLES (MAX_OUTS * MAX_DESKS)
static struct overlay_handle overlay_handles[MAX_OVERLAY_HANDLES];
static int		     n_overlay_handles;

/* Drag state.  When dragging != 0 the press initially hit a plate; on
 * release we resolve the destination and dispatch.  The `start_*`
 * fields capture the drag origin so the rendering code can sanity-check
 * threshold (small motions don't open a drag).
 */
static int    dragging;
static char   drag_monitor[64];
static double drag_start_x, drag_start_y;
static double drag_cur_x, drag_cur_y;
static int    opt_verbose; /* -v: log click decisions to stderr */

/*
 * -m NAME[,NAME,...] restricts the pager to a subset of monitors.
 *
 *   - No -m: every monitor.  The page canvas is the physical bounding
 *     box of those monitors, and monitor labels are sorted by cow's
 *     `number` field.
 *   - -m MON: single monitor.  The pager tracks that monitor, so the
 *     page canvas is that monitor alone and no monitor-label row is
 *     drawn.
 *   - -m MON1,MON2,...: multiple monitors.  The canvas and monitor
 *     labels are restricted to the listed set, in the order given on
 *     the CLI.
 */
#define MAX_OPT_MONITORS 16
static char opt_monitors[MAX_OPT_MONITORS][64];
static int  opt_n_monitors;
static struct pager_selection selected_pagers[MAX_SELECTED_PAGERS];
static int n_selected_pagers;
static int instance_index;
static int show_group_count;
static bool spawn_extra_instances = true;
static char instance_title[256] = PROG;

struct pager_load_ctx {
	int depth;
	int in_selected_pager;
};

static void
config_colour(float out[4], uint32_t rgb)
{
	cow_colour_convert(out, rgb);
}

static void
pager_config_init(struct pager_config *pc)
{
	memset(pc, 0, sizeof(*pc));
	pc->desktop_labels = 1;
	pc->monitor_labels = 1;
	pc->label_highlight = 1;
	pc->desktop_scale = 32;
	pc->window_icons = 1;
	pc->window_icon_size = 16;
	config_colour(pc->background, 0xffffff);
	memcpy(pc->desk_bg, pc->background, sizeof(pc->desk_bg));
	memcpy(pc->page_bg, pc->background, sizeof(pc->page_bg));
	memcpy(pc->page_active_bg, pc->background, sizeof(pc->page_active_bg));
	config_colour(pc->label_bg, 0xd8d8d8);
	config_colour(pc->label_active_bg, 0x3366b3);
	config_colour(pc->label_fg, 0x000000);
	config_colour(pc->label_inactive_fg, 0x000000);
	config_colour(pc->window_focused_bg, 0xf7c733);
}

static void
apply_theme_defaults(const state_t *st)
{
	if (st == NULL)
		return;

	if (!cfg.background_set) {
		if (st->theme.has_background) {
			memcpy(cfg.background, st->theme.background,
			    sizeof(cfg.background));
		} else if (st->theme.has_inactive_bg) {
			memcpy(cfg.background, st->theme.inactive_bg,
			    sizeof(cfg.background));
		}
	}

	if (!cfg.desk_bg_set)
		memcpy(cfg.desk_bg, cfg.background, sizeof(cfg.desk_bg));
	if (!cfg.page_bg_set)
		memcpy(cfg.page_bg, cfg.background, sizeof(cfg.page_bg));
	if (!cfg.page_active_bg_set) {
		if (!cfg.background_set && st->theme.has_active_bg) {
			memcpy(cfg.page_active_bg, st->theme.active_bg,
			    sizeof(cfg.page_active_bg));
		} else {
			memcpy(cfg.page_active_bg, cfg.background,
			    sizeof(cfg.page_active_bg));
		}
	}

	if (!cfg.label_bg_set && st->theme.has_inactive_bg)
		memcpy(cfg.label_bg, st->theme.inactive_bg,
		    sizeof(cfg.label_bg));
	if (!cfg.label_active_bg_set && st->theme.has_active_bg)
		memcpy(cfg.label_active_bg, st->theme.active_bg,
		    sizeof(cfg.label_active_bg));
	if (!cfg.label_fg_set && st->theme.has_active_fg)
		memcpy(cfg.label_fg, st->theme.active_fg,
		    sizeof(cfg.label_fg));
	if (!cfg.label_inactive_fg_set && st->theme.has_inactive_fg)
		memcpy(cfg.label_inactive_fg, st->theme.inactive_fg,
		    sizeof(cfg.label_inactive_fg));
	if (!cfg.window_focused_bg_set && st->theme.has_active_bg)
		memcpy(cfg.window_focused_bg, st->theme.active_bg,
		    sizeof(cfg.window_focused_bg));
}

static int
parse_monitor_list(const char *list, int replace)
{
	if (replace)
		opt_n_monitors = 0;
	char *dup = strdup(list);
	if (dup == NULL)
		return 0;
	char *save = NULL;
	char *tok = strtok_r(dup, ",", &save);
	while (tok != NULL && opt_n_monitors < MAX_OPT_MONITORS) {
		while (*tok == ' ' || *tok == '\t')
			tok++;
		size_t L = strlen(tok);
		while (L > 0 && (tok[L - 1] == ' ' || tok[L - 1] == '\t'))
			tok[--L] = '\0';
		if (*tok != '\0') {
			strlcpy(opt_monitors[opt_n_monitors], tok,
			    sizeof(opt_monitors[0]));
			opt_n_monitors++;
		}
		tok = strtok_r(NULL, ",", &save);
	}
	free(dup);
	return 1;
}

static bool
set_desk_range(int first, int last)
{
	if (first < 0 || last < first)
		return false;

	opt_desk_range_set = 1;
	opt_desk_first = cfg.desk_first = first;
	opt_desk_last = cfg.desk_last = last;
	cfg.desk_range_set = 1;
	opt_current_only = cfg.current_only = 0;
	return true;
}

static void
set_current_only(bool enabled)
{
	opt_current_only = cfg.current_only = enabled ? 1 : 0;
	if (enabled) {
		opt_desk_range_set = cfg.desk_range_set = 0;
		opt_desk_first = cfg.desk_first = 0;
		opt_desk_last = cfg.desk_last = 0;
	}
}

static int
pager_selection_find(const char *name)
{
	for (int i = 0; i < n_selected_pagers; i++)
		if (strcmp(selected_pagers[i].name, name) == 0)
			return i;
	return -1;
}

static bool
pager_selection_add(const char *name)
{
	if (pager_selection_find(name) != -1)
		return true;
	if (n_selected_pagers >= MAX_SELECTED_PAGERS)
		return false;

	selected_pagers[n_selected_pagers].name = strdup(name);
	if (selected_pagers[n_selected_pagers].name == NULL)
		return false;
	selected_pagers[n_selected_pagers].seen = 0;
	n_selected_pagers++;
	return true;
}

static void
pager_selections_free(void)
{
	for (int i = 0; i < n_selected_pagers; i++) {
		free(selected_pagers[i].name);
		selected_pagers[i].name = NULL;
		selected_pagers[i].seen = 0;
	}
	n_selected_pagers = 0;
}

static state_t	       g_state;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int	       g_dirty;
static int	       wake_pipe[2];

static void do_render(const state_t *);

static void
handle_wake(void)
{
	char buf[64];
	while (read(wake_pipe[0], buf, sizeof(buf)) > 0)
		;

	pthread_mutex_lock(&g_lock);
	int dirty = g_dirty;
	g_dirty	  = 0;
	state_t st = g_state;
	pthread_mutex_unlock(&g_lock);

	if (dirty && surface_configured) {
		apply_theme_defaults(&st);
		do_render(&st);
	}
}

static void
wayland_cb(__unused evutil_socket_t fd, __unused short events,
    __unused void *data)
{
	if (wl_display_dispatch(display) < 0)
		opt_running = 0;
}

static void
wake_cb(__unused evutil_socket_t fd, __unused short events,
    __unused void *data)
{
	handle_wake();
}

static void
ipc_send(const char *cmd)
{
	if (opt_verbose)
		fprintf(stderr, "cowpager: ipc_send: %s\n", cmd);

	int fd = cow_module_cmd_connect("cowpager");
	if (fd < 0)
		return;

	if (cow_module_cmd_send(fd, cmd, true)) {
		char b[COW_MODULE_ACK_BUFFER_SIZE];
		read(fd, b, sizeof(b));
	}
	close(fd);
}

static char *
ipc_query(const char *cmd)
{
	return cow_module_cmd_request("cowpager", cmd, true);
}

static struct wl_buffer *
shm_buf(int w, int h, void **px)
{
	int size = w * 4 * h;
    #if HAVE_MEMFD_CREATE
	int fd	 = memfd_create("cp", MFD_CLOEXEC);
    #else
    /* POSIX fallback for systems without memfd_create. */
    char name[64];
    snprintf(name, sizeof(name), "/cp-%d", getpid());
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

/*
 * Stable colour per monitor name.  Used to colour-code the
 * per-monitor label in the chrome strip and the viewport overlay drawn
 * in each desk's page grid, so the user can read the monitor->page
 * relationship at a glance.
 */
static void
monitor_hue(const char *name, double *hue_out)
{
	if (name == NULL || name[0] == '\0') {
		*hue_out = -1.0;
		return;
	}
	uint32_t h = str_hash(name);
	*hue_out   = (double)((h * 7919u) % 360);
}

static void
monitor_colour(const char *name, double *r, double *g, double *b)
{
	double hue;
	monitor_hue(name, &hue);
	if (hue < 0.0) {
		*r = 0.85;
		*g = 0.85;
		*b = 0.85;
		return;
	}
	hsv_to_rgb(hue, 0.70, 0.92, r, g, b);
}

static void
monitor_colour_muted(const char *name, double *r, double *g, double *b)
{
	double hue;
	monitor_hue(name, &hue);
	if (hue < 0.0) {
		*r = 0.50;
		*g = 0.50;
		*b = 0.55;
		return;
	}
	/* Same hue as monitor_colour, but lower saturation and
	 * darker -- the border becomes a quiet edge rather than a
	 * loud frame.  Still legible against any per-window fill
	 * because we keep enough chroma for hue identity.
	 */
	hsv_to_rgb(hue, 0.45, 0.55, r, g, b);
}

static const char *single_pinned_output(void);
static const char *cp_host_output(void);
static int	   cp_surface_on_output(const char *name);
static int hit_test_page(const state_t *st, double x, double y, int *desk_nr,
    int *pc_out, int *pr_out, int *tab_out, const char **out_for_cmd);

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
 * the monitor labels and any single desk-level highlight would be at
 * best arbitrary and at worst misleading.
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
 * Build the list of outputs the pager should consider "visible", in
 * display order.  This drives monitor labels, canvas bounds, and the
 * window-placement filter.
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

/* Number of monitor labels we would draw. */
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

static const pout_t *
output_by_name(const state_t *st, const char *name)
{
	if (name == NULL || name[0] == '\0')
		return NULL;
	for (int i = 0; i < st->n_outs; i++)
		if (strcmp(st->outs[i].name, name) == 0)
			return &st->outs[i];
	return NULL;
}

static const pout_t *
current_only_output(const state_t *st)
{
	const char *pinned = single_pinned_output();
	const pout_t *o = output_by_name(st, pinned);
	if (o != NULL)
		return o;

	o = output_by_name(st, st->user_output);
	if (o != NULL && monitor_visible(o->name))
		return o;

	for (int i = 0; i < st->n_outs; i++) {
		if (st->outs[i].current && monitor_visible(st->outs[i].name))
			return &st->outs[i];
	}

	return reference_output(st);
}

/*
 * Is this window a cowpager window?
 */
static int
is_self_window(const pwin_t *w)
{
	return w != NULL && strcmp(w->app_id, "cowpager") == 0;
}

/*
 * Look up a window's own output by name.  Falls back to NULL when the
 * window's output field is empty or the named output isn't tracked.
 */
static const pout_t *
output_for_window(const state_t *st, const pwin_t *w)
{
	if (w == NULL || w->output[0] == '\0')
		return NULL;
	for (int i = 0; i < st->n_outs; i++)
		if (strcmp(st->outs[i].name, w->output) == 0)
			return &st->outs[i];
	return NULL;
}

static int
canvas_from_outputs(const state_t *st, canvas_t *cv)
{
	memset(cv, 0, sizeof(*cv));

	const pout_t *outs[MAX_OUTS];
	int n = tab_outputs(st, outs, MAX_OUTS);
	if (n <= 0)
		return 0;

	const pout_t *ref = outs[0];
	cv->page_cols = ref->page_cols > 0 ? ref->page_cols : 1;
	cv->page_rows = ref->page_rows > 0 ? ref->page_rows : 1;

	if (n == 1) {
		cv->tracked = 1;
		cv->tracked_output = outs[0];
		cv->x = outs[0]->output_x;
		cv->y = outs[0]->output_y;
		cv->w = outs[0]->output_w > 0 ? outs[0]->output_w : 1;
		cv->h = outs[0]->output_h > 0 ? outs[0]->output_h : 1;
		return 1;
	}

	int min_x = outs[0]->output_x;
	int min_y = outs[0]->output_y;
	int max_x = outs[0]->output_x +
	    (outs[0]->output_w > 0 ? outs[0]->output_w : 1);
	int max_y = outs[0]->output_y +
	    (outs[0]->output_h > 0 ? outs[0]->output_h : 1);

	for (int i = 1; i < n; i++) {
		int ow = outs[i]->output_w > 0 ? outs[i]->output_w : 1;
		int oh = outs[i]->output_h > 0 ? outs[i]->output_h : 1;
		if (outs[i]->output_x < min_x)
			min_x = outs[i]->output_x;
		if (outs[i]->output_y < min_y)
			min_y = outs[i]->output_y;
		if (outs[i]->output_x + ow > max_x)
			max_x = outs[i]->output_x + ow;
		if (outs[i]->output_y + oh > max_y)
			max_y = outs[i]->output_y + oh;
	}

	cv->x = min_x;
	cv->y = min_y;
	cv->w = max_x - min_x;
	cv->h = max_y - min_y;
	if (cv->w < 1)
		cv->w = 1;
	if (cv->h < 1)
		cv->h = 1;
	return 1;
}

static int
output_in_canvas(const canvas_t *cv, const pout_t *out)
{
	if (cv->tracked_output != NULL && out != cv->tracked_output)
		return 0;
	return out != NULL;
}

/*
 * Convert an output-local virtual-desktop coordinate to the combined
 * output canvas. Each virtual page repeats the complete output layout;
 * coordinates within a page retain their owning output's offset.
 */
static double
canvas_page_axis(int position, int output_size, int canvas_size,
    int output_offset)
{
	if (output_size < 1)
		output_size = 1;
	double page = floor((double)position / output_size);
	double local = position - page * output_size;
	return page * canvas_size + output_offset + local;
}

static double
canvas_page_x(const canvas_t *cv, const pout_t *out, int vx)
{
	return canvas_page_axis(vx, out->output_w, cv->w,
	    out->output_x - cv->x);
}

static double
canvas_page_y(const canvas_t *cv, const pout_t *out, int vy)
{
	return canvas_page_axis(vy, out->output_h, cv->h,
	    out->output_y - cv->y);
}

static const pout_t *
output_at_canvas_point(const state_t *st, const canvas_t *cv, double x,
    double y)
{
	if (cv->tracked_output != NULL)
		return cv->tracked_output;

	for (int i = 0; i < st->n_outs; i++) {
		const pout_t *o = &st->outs[i];
		if (!monitor_visible(o->name))
			continue;
		if (!output_in_canvas(cv, o))
			continue;
		int ox = o->output_x - cv->x;
		int oy = o->output_y - cv->y;
		int ow = o->output_w > 0 ? o->output_w : 1;
		int oh = o->output_h > 0 ? o->output_h : 1;
		if (x >= ox && x < ox + ow && y >= oy && y < oy + oh)
			return o;
	}
	return NULL;
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
	int mon_tab_h;	     /* monitor-label row height (0 if only
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
 * FvwmPager uses the configured module font at every window size: the
 * pager geometry shrinks and grows, but labels do not scale with it.
 * Keep cowpager's chrome on the same model so small pager windows stay
 * readable instead of becoming a scaled-down preview.
 */
#define DESK_FONT_PX 10.0
#define TAB_FONT_PX 10.0

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

static void
choose_label_heights(int has_mon_tabs, double *desk_font,
    int *desk_h, double *tab_font, int *tab_h)
{
	*desk_font = DESK_FONT_PX;
	*desk_h	   = band_h_for_font(*desk_font);

	if (has_mon_tabs) {
		*tab_font = TAB_FONT_PX;
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
		if (opt_desk_range_set) {
			int n = opt_desk_last - opt_desk_first + 1;
			if (n < 1)
				n = 1;
			n_desks = n;
		}
		compute_grid(n_desks, &L->rows, &L->cols);
	}

	/* Draw monitor labels only when there's more than one effective
	 * monitor to distinguish.  With `-m NAME` or a single-output
	 * setup there is no useful information in the monitor-label row. */
	int n_tab_mons = mon_tab_count(st);
	choose_label_heights(cfg.monitor_labels && n_tab_mons > 1,
	    &L->desk_font_px, &L->desk_label_h, &L->tab_font_px,
	    &L->mon_tab_h);
	if (!cfg.desktop_labels) {
		L->desk_font_px = 0;
		L->desk_label_h = 0;
	}
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
maybe_apply_initial_size(const state_t *st, const layout_t *L)
{
	if (initial_size_done)
		return;

	canvas_t cv;
	if (!canvas_from_outputs(st, &cv))
		return;

	if (configured_w_seen && configured_h_seen) {
		initial_size_done = 1;
		return;
	}

	const pout_t *ref = reference_output(st);
	int pcols = ref && ref->page_cols > 0 ? ref->page_cols : 1;
	int prows = ref && ref->page_rows > 0 ? ref->page_rows : 1;
	const int vscale = cfg.desktop_scale > 0 ? cfg.desktop_scale : 32;

	int label_h = L->chrome_h;
	int w = win_w;
	int h = win_h;

	if (configured_w_seen) {
		h = (w * cv.h) / cv.w + label_h * L->rows + L->rows;
	} else if (configured_h_seen && h > label_h * L->rows) {
		w = ((h - label_h * L->rows + L->rows) * cv.w) / cv.h +
		    L->cols;
	} else {
		w = (pcols * cv.w * L->cols) / vscale + L->cols;
		h = (prows * cv.h * L->rows) / vscale +
		    label_h * L->rows + L->rows;
	}

	win_w = w;
	win_h = h;
	initial_size_done = 1;
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

static PangoFontDescription *
get_font(const char * base_font, double size, int bold)
{
	PangoFontDescription *desc = pango_font_description_from_string(
		base_font);	
	/* The old "Cairo Toy Font" rendering was slightly smaller than
	 * Pango's default scaling, so make the font 80% of size */
	pango_font_description_set_size(desc, size * PANGO_SCALE * 0.8);
	pango_font_description_set_weight(desc, 
		(bold ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_MEDIUM));
	return desc;
}

static void
draw_text_centered(cairo_t *cr, PangoFontDescription *font, double x, 
	double y, double w, double h, const char *txt)
{
	int outwidth;
	y += (h - cow_text_baseline(cr, font, txt)) / 2;
	cow_text_render(cr, font, x, y, w, h, txt, PANGO_ALIGN_CENTER, 
		&outwidth);
}

static void
draw_text_centered_clipped(cairo_t *cr, PangoFontDescription *font,
	double x, double y, double w, double h,
	const char *txt)
{
	if (w <= 1 || h <= 1)
		return;

	cairo_save(cr);
	cairo_rectangle(cr, x, y, w, h);
	cairo_clip(cr);
	draw_text_centered(cr, font, x, y, w, h, txt);
	cairo_restore(cr);
}

static void
stroke_page_grid_lines(cairo_t *cr, double sx, double sy, double sw,
    double sh, int pcols, int prows, double cw, double ch)
{
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
}

static void
draw_icon_at(cairo_t *cr, const char *path, double cx, double cy, int size)
{
	if (path == NULL || *path == '\0' || size < 3)
		return;

	int ix = (int)(cx - size / 2.0);
	int iy = (int)(cy - size / 2.0);
	cairo_surface_t *surface = cairo_get_target(cr);
	cairo_surface_flush(surface);
	struct cow_pixbuf pixbuf = {
		.pixels = (uint32_t *)cairo_image_surface_get_data(surface),
		.stride_px = cairo_image_surface_get_stride(surface) / 4,
		.width = cairo_image_surface_get_width(surface),
		.height = cairo_image_surface_get_height(surface),
	};
	(void)cow_image_cache_blit(&icon_cache, path,
	    COW_DATADIR "/icons/default.png", &pixbuf, ix, iy, size);
	cairo_surface_mark_dirty(surface);
}

static void
draw_window_icon(cairo_t *cr, const char *path, double x, double y, double w,
    double h)
{
	if (!cfg.window_icons || path == NULL || *path == '\0' || w < 7 || h < 7)
		return;

	int available = (int)(w < h ? w : h) - 4;
	int size = cfg.window_icon_size;
	if (size > available)
		size = available;
	draw_icon_at(cr, path, x + w / 2.0, y + h / 2.0, size);
}

static void
fill_output_backgrounds(cairo_t *cr, const state_t *st, const canvas_t *cv,
    double cell_x, double cell_y, double cw, double ch)
{
	if (cfg.background_set || cfg.page_bg_set)
		return;

	for (int oi = 0; oi < st->n_outs; oi++) {
		const pout_t *o = &st->outs[oi];
		if (!o->has_background)
			continue;
		if (!monitor_visible(o->name))
			continue;
		if (!output_in_canvas(cv, o))
			continue;

		int oow = o->output_w > 0 ? o->output_w : 1;
		int ooh = o->output_h > 0 ? o->output_h : 1;
		double ax = cell_x + ((double)(o->output_x - cv->x) / cv->w) *
		    cw;
		double ay = cell_y + ((double)(o->output_y - cv->y) / cv->h) *
		    ch;
		double aw = ((double)oow / cv->w) * cw;
		double ah = ((double)ooh / cv->h) * ch;
		if (aw <= 0.0 || ah <= 0.0)
			continue;

		cow_module_cairo_fill(cr, o->background, ax, ay, aw, ah);
	}
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
    const pout_t *ref, const canvas_t *cv, const pdesk_t *d, int sx, int sy,
    int sw, int sh)
{
	int pcols = ref->page_cols > 0 ? ref->page_cols : 1;
	int prows = ref->page_rows > 0 ? ref->page_rows : 1;

	double cw = (double)sw / pcols;
	double ch = (double)sh / prows;
	struct window_icon_rec {
		const char *path;
		double x, y, w, h;
		int iconified;
	} icons[MAX_WINS];
	int n_icons = 0;

	PangoFontDescription *font = get_font(st->theme.font, 10.0, 1);
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
				if (!monitor_visible(o->name))
					continue;
				if (!output_in_canvas(cv, o))
					continue;
				if (o->current_desk == d->nr &&
				    o->page_col == pc && o->page_row == pr) {
					is_cur = 1;
					break;
				}
			}

			cow_module_cairo_fill(cr,
			    is_cur ? cfg.page_active_bg : cfg.page_bg, cx, cy,
			    cw, ch);
			fill_output_backgrounds(cr, st, cv, cx, cy, cw, ch);
		}
	}

	/*
	 * In per-output mode with multiple monitors visible, every page
	 * cell can target a monitor by its physical rectangle in the
	 * combined monitor canvas.  Draw faint footprints in each page so
	 * empty areas still communicate where a click will apply.
	 */
	if (is_per_output_mode(st) && !cv->tracked && mon_tab_count(st) > 1) {
		double dashes[2] = { 4.0, 2.0 };
		for (int pr = 0; pr < prows; pr++) {
			for (int pc = 0; pc < pcols; pc++) {
				double cell_x = sx + pc * cw;
				double cell_y = sy + pr * ch;

				for (int oi = 0; oi < st->n_outs; oi++) {
					const pout_t *o = &st->outs[oi];
					if (!monitor_visible(o->name))
						continue;
					if (!output_in_canvas(cv, o))
						continue;
					if (d->nr == o->current_desk &&
					    pc == o->page_col && pr == o->page_row)
						continue;

					int oow = o->output_w > 0 ?
					    o->output_w : 1;
					int ooh = o->output_h > 0 ?
					    o->output_h : 1;
					double ax = cell_x +
					    ((double)(o->output_x - cv->x) /
					    cv->w) * cw;
					double ay = cell_y +
					    ((double)(o->output_y - cv->y) /
					    cv->h) * ch;
					double aw = ((double)oow / cv->w) * cw;
					double ah = ((double)ooh / cv->h) * ch;
					if (aw <= 1.0 || ah <= 1.0)
						continue;

					const double inset = 3.0;
					double rx = ax + inset + 0.5;
					double ry = ay + inset + 0.5;
					double rw = aw - 2 * inset - 1.0;
					double rh = ah - 2 * inset - 1.0;
					if (rw <= 1.0 || rh <= 1.0)
						continue;

					double mr, mg, mb;
					monitor_colour(o->name, &mr, &mg, &mb);
					cairo_set_source_rgba(cr, mr, mg, mb,
					    0.68);
					cairo_set_line_width(cr, 1.5);
					cairo_set_dash(cr, dashes, 2, 0);
					cairo_rectangle(cr, rx, ry, rw, rh);
					cairo_stroke(cr);
				}
			}
		}
		cairo_set_dash(cr, NULL, 0, 0);
	}

	/*
	 * Each window's thumbnail is sized in terms of its OWN
	 * monitor's pixel dimensions, not the reference monitor's.
	 */
	/* Paint the focused window last so overlapping thumbnails cannot
	 * obscure its highlight. */
	const pwin_t *draw_wins[MAX_WINS];
	int n_draw_wins = 0;
	for (int focus_pass = 0; focus_pass < 2; focus_pass++) {
		for (int wi = 0; wi < st->n_wins; wi++) {
			const pwin_t *w = &st->wins[wi];
			if ((w->focused != 0) == focus_pass)
				draw_wins[n_draw_wins++] = w;
		}
	}
	for (int wi = 0; wi < n_draw_wins; wi++) {
		const pwin_t *w = draw_wins[wi];
		int foc = w->focused != 0;
		if (w->desk != d->nr)
			continue;
		if (!monitor_visible(w->output))
			continue;
		if (is_self_window(w))
			continue;

		const pout_t *wo = output_for_window(st, w);
		if (!output_in_canvas(cv, wo))
			continue;
		int wow		 = (wo && wo->output_w > 0) ? wo->output_w : 1;
		int woh		 = (wo && wo->output_h > 0) ? wo->output_h : 1;
		if (wow <= 0 || woh <= 0)
			continue;
		int desk_w = pcols * wow;
		int desk_h = prows * woh;
		int vx = w->iconified ? w->icon_vx : w->vx;
		int vy = w->iconified ? w->icon_vy : w->vy;
		int width = w->iconified ? w->icon_w : w->w;
		int height = w->iconified ? w->icon_w : w->h;
		if (vx < 0 || vx >= desk_w)
			continue;
		if (vy < 0 || vy >= desk_h)
			continue;

		double wx = sx + (canvas_page_x(cv, wo, vx) / cv->w) * cw;
		double wy = sy + (canvas_page_y(cv, wo, vy) / cv->h) * ch;
		double ww = ((double)width / wow) *
		    (cw * wo->output_w / cv->w);
		double wh = ((double)height / woh) *
		    (ch * wo->output_h / cv->h);

		if (ww < 3)
			ww = 3;
		if (wh < 3)
			wh = 3;
		if (wx + ww > sx + sw)
			ww = sx + sw - wx;
		if (wy + wh > sy + sh)
			wh = sy + sh - wy;

		double r, g, b, a = 1.0;
		if (foc) {
			r = cfg.window_focused_bg[0];
			g = cfg.window_focused_bg[1];
			b = cfg.window_focused_bg[2];
			a = cfg.window_focused_bg[3];
		} else {
			window_colour(w->id, &r, &g, &b);
		}
		if (w->iconified) {
			if (cfg.window_icons && w->icon[0] != '\0' &&
			    n_icons < MAX_WINS) {
				icons[n_icons].path = w->icon;
				icons[n_icons].x = wx;
				icons[n_icons].y = wy;
				icons[n_icons].w = ww;
				icons[n_icons].h = wh;
				icons[n_icons].iconified = 1;
				n_icons++;
			} else {
				double marker = 7.0;
				double mx = wx + (ww - marker) / 2.0;
				double my = wy + (wh - marker) / 2.0;
				cairo_set_source_rgba(cr, r, g, b, a);
				cairo_rectangle(cr, mx, my, marker, marker);
				cairo_fill(cr);
				cairo_set_source_rgba(cr, 0.08, 0.08, 0.10, 1.0);
				cairo_set_line_width(cr, 1.0);
				cairo_rectangle(cr, mx + 0.5, my + 0.5,
				    marker - 1.0, marker - 1.0);
				cairo_stroke(cr);
			}
			continue;
		}
		cairo_set_source_rgba(cr, r, g, b, a);
		cairo_rectangle(cr, wx, wy, ww, wh);
		cairo_fill(cr);

		double br, bg_, bb;
		if (wo != NULL && wo->name[0] != '\0') {
			monitor_colour_muted(wo->name, &br, &bg_, &bb);
		} else {
			br  = 0.08;
			bg_ = 0.08;
			bb  = 0.10;
		}
		cairo_set_source_rgba(cr, br, bg_, bb, 1.0);
		cairo_set_line_width(cr, 1.0);
		cairo_set_dash(cr, NULL, 0, 0);
		cairo_rectangle(cr, wx + 0.5, wy + 0.5, ww - 1, wh - 1);
		cairo_stroke(cr);
		if (cfg.window_icons && w->icon[0] != '\0' &&
		    n_icons < MAX_WINS) {
			icons[n_icons].path = w->icon;
			icons[n_icons].x = wx;
			icons[n_icons].y = wy;
			icons[n_icons].w = ww;
			icons[n_icons].h = wh;
			icons[n_icons].iconified = 0;
			n_icons++;
		}
	}

	/*
	 * Sub-page viewport overlays -- one per monitor currently on
	 * this desk -- coloured by the monitor's stable palette entry
	 * and labelled with the monitor's name when there's room.
	 */
	struct overlay_rec {
		const pout_t *o;
		double	      ax, ay, aw, ah;
	};
	struct overlay_rec ov[MAX_OUTS];
	int		   n_ov = 0;

	for (int oi = 0; oi < st->n_outs; oi++) {
		const pout_t *o = &st->outs[oi];
		if (o->current_desk != d->nr)
			continue;
		if (!monitor_visible(o->name))
			continue;
		if (!output_in_canvas(cv, o))
			continue;
		int oow = o->output_w > 0 ? o->output_w : 1;
		int ooh = o->output_h > 0 ? o->output_h : 1;
		if (oow <= 0 || ooh <= 0)
			continue;
		double ax = sx + (canvas_page_x(cv, o, o->page_vx) / cv->w) * cw;
		double ay = sy + (canvas_page_y(cv, o, o->page_vy) / cv->h) * ch;
		double aw = ((double)oow / cv->w) * cw;
		double ah = ((double)ooh / cv->h) * ch;

		double mr, mg, mb;
		monitor_colour(o->name, &mr, &mg, &mb);

		cairo_set_source_rgba(cr, mr, mg, mb, 0.22);
		cairo_rectangle(cr, ax, ay, aw, ah);
		cairo_fill(cr);
		cairo_set_source_rgba(cr, mr, mg, mb, 0.95);
		cairo_set_line_width(cr, 1.5);
		cairo_set_dash(cr, NULL, 0, 0);
		cairo_rectangle(cr, ax + 0.75, ay + 0.75, aw - 1.5, ah - 1.5);
		cairo_stroke(cr);

		if (n_ov < (int)(sizeof(ov) / sizeof(ov[0]))) {
			ov[n_ov].o  = o;
			ov[n_ov].ax = ax;
			ov[n_ov].ay = ay;
			ov[n_ov].aw = aw;
			ov[n_ov].ah = ah;
			n_ov++;
		}
	}

	/*
	 * Caption pass.  Skip wh*/
	if (mon_tab_count(st) > 1) {
		const double label_pad_x = 3.0;
		const double label_pad_y = 3.0;
		const double inter_gap	 = 2.0;

		for (int i = 0; i < n_ov; i++) {
			struct overlay_rec *ovi = &ov[i];
			if (ovi->o->name[0] == '\0')
				continue;
			if (ovi->aw < 28 || ovi->ah < 14)
				continue;
			int twidth, theight;
			cow_text_measure(cr, font, ovi->o->name, &twidth, &theight);
			if (twidth > ovi->aw - 2 * label_pad_x)
				continue;
			if (theight > ovi->ah - 2 * label_pad_y)
				continue;

			double plate_w = twidth + 2 * label_pad_x;
			double plate_h = theight + 4;

			/* Count earlier captions sharing this overlay
			 * position so this one slides below them. */
			int stack = 0;
			for (int j = 0; j < i; j++) {
				if (fabs(ov[j].ax - ovi->ax) < 0.5 &&
				    fabs(ov[j].ay - ovi->ay) < 0.5)
					stack++;
			}

			double plate_x = ovi->ax + label_pad_x;
			double plate_y = ovi->ay + label_pad_y +
			    stack * (plate_h + inter_gap);

			/* If the stacked caption would fall outside the
			 * overlay, give up rather than overlap a window
			 * thumbnail or the next page cell. */
			if (plate_y + plate_h > ovi->ay + ovi->ah - 1)
				continue;

			int baseline_adjust = (theight - cow_text_baseline(cr, font,
				ovi->o->name)) / 2;
			cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.6);
			cairo_rectangle(cr, plate_x, plate_y, plate_w, plate_h);
			cairo_fill(cr);
			cairo_set_source_rgba(cr, 0.98, 0.98, 0.98, 0.98);
			int outwidth;
			cow_text_render(cr, font, plate_x, plate_y + baseline_adjust,
				plate_w, plate_h, ovi->o->name,
				PANGO_ALIGN_CENTER, &outwidth);

			/*
			 * Publish a draggable handle for this plate.
			 */
			if (n_overlay_handles < MAX_OVERLAY_HANDLES) {
				struct overlay_handle *h =
				    &overlay_handles[n_overlay_handles++];
				strlcpy(h->name, ovi->o->name,
				    sizeof(h->name));
				const double slack	     = 2.0;
				h->px			     = plate_x - slack;
				h->py			     = plate_y - slack;
				h->pw = plate_w + 2 * slack;
				h->ph = plate_h + 2 * slack;
			}
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
		cairo_set_dash(cr, dashes, 2, 0);

		/* Contrast against both the light page fill and the
		 * coloured monitor-target outlines. */
		cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.72);
		cairo_set_line_width(cr, 2.0);
		stroke_page_grid_lines(cr, sx, sy, sw, sh, pcols, prows, cw,
		    ch);

		cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.92);
		cairo_set_line_width(cr, 1.0);
		stroke_page_grid_lines(cr, sx, sy, sw, sh, pcols, prows, cw,
		    ch);
		/* Reset to solid for any subsequent strokes. */
		cairo_set_dash(cr, NULL, 0, 0);
	}

	/* Icons are the final pager overlay so viewport tinting and page
	 * gridlines cannot obscure their pixels.
	 */
	for (int i = 0; i < n_icons; i++) {
		if (icons[i].iconified) {
			draw_icon_at(cr, icons[i].path,
			    icons[i].x + icons[i].w / 2.0,
			    icons[i].y + icons[i].h / 2.0,
			    cfg.window_icon_size);
		} else {
			draw_window_icon(cr, icons[i].path, icons[i].x,
			    icons[i].y, icons[i].w, icons[i].h);
		}
	}
}

/*
 * Draw the chrome (label band + monitor-label row) at the top of a
 * single desk cell.
 *
 * The monitor labels are evenly spaced left-to-right in monitor order
 * (filtered by -m).  A label is highlighted iff its monitor's
 * current_desk equals this desk -- a quick visual indicator of which
 * monitor is currently "on" each desk.  Match FvwmPager's
 * MonitorLabels look.
 */
static void
draw_desk_chrome(cairo_t *cr, const state_t *st, const layout_t *L,
    const pdesk_t *d, int dx, int dy, int is_active_desk)
{
	int desk_hilight = cfg.label_highlight && is_active_desk &&
	    L->mon_tab_h <= 0;

	if (L->desk_label_h > 0) {
		cow_module_cairo_fill(cr,
		    desk_hilight ? cfg.label_active_bg : cfg.label_bg, dx,
		    dy, L->cell_w, L->desk_label_h);

		cow_module_cairo_set_source(cr, cfg.label_fg);
		PangoFontDescription *font = get_font(st->theme.font, L->desk_font_px, 1);

		char nr_label[16];
		snprintf(nr_label, sizeof(nr_label), "%d", d->nr);
		const char *text = nr_label;
		int twidth, theight;
		if (d->name[0] != '\0') {
			cow_text_measure(cr, font, d->name, &twidth, &theight);
			if (twidth <= L->cell_w - 4)
				text = d->name;
		}
		draw_text_centered_clipped(cr, font, dx, dy, L->cell_w,
		    L->desk_label_h, text);
	}

	if (L->mon_tab_h <= 0)
		return;

	const pout_t *tab[MAX_OUTS];
	int	      n_tab = tab_outputs(st, tab, MAX_OUTS);
	if (n_tab <= 0)
		return;

	int    tab_y = dy + L->desk_label_h;
	double tw    = (double)L->cell_w / n_tab;
	PangoFontDescription * font = get_font(st->theme.font,
		L->tab_font_px, 1);

	for (int i = 0; i < n_tab; i++) {
		double tx = dx + i * tw;
		int active = cfg.label_highlight && tab[i]->current_desk == d->nr;
		cow_module_cairo_fill(cr,
		    active ? cfg.label_active_bg : cfg.label_bg, tx, tab_y,
		    tw, L->mon_tab_h);

		cairo_set_source_rgba(cr, 0.04, 0.04, 0.06, 1.0);
		cairo_set_line_width(cr, 1.0);
		cairo_set_dash(cr, NULL, 0, 0);
		cairo_rectangle(cr, tx + 0.5, tab_y + 0.5, tw - 1,
		    L->mon_tab_h - 1);
		cairo_stroke(cr);

		double mr, mg, mb;
		monitor_colour(tab[i]->name, &mr, &mg, &mb);
		cairo_set_source_rgba(cr, mr, mg, mb, 1.0);
		cairo_rectangle(cr, tx + 1, tab_y + 1, tw - 2, 2.5);
		cairo_fill(cr);

		const char	    *name = tab[i]->name;
		char		     short_name[16];
		int twidth, theight;
		cow_text_measure(cr, font, d->name, &twidth, &theight);
		if (twidth > tw - 4) {
			int n = tab[i]->number > 0 ? tab[i]->number : i + 1;
			snprintf(short_name, sizeof(short_name), "%d", n);
			name = short_name;
		}

		if (active)
			cow_module_cairo_set_source(cr, cfg.label_fg);
		else
			cow_module_cairo_set_source(cr, cfg.label_inactive_fg);
		draw_text_centered_clipped(cr, font, tx, tab_y, tw, L->mon_tab_h, name);
	}
}

static void
build_visible_desks(const pout_t *ref, int desk_indices[MAX_DESKS],
    int max_visible, int *n_visible)
{
	*n_visible = 0;
	if (ref == NULL || max_visible <= 0)
		return;

	if (opt_current_only) {
		for (int i = 0; i < ref->n_desks; i++) {
			if (ref->desks[i].nr == ref->current_desk) {
				desk_indices[(*n_visible)++] = i;
				return;
			}
		}
		return;
	}

	for (int i = 0; i < ref->n_desks && *n_visible < max_visible; i++) {
		if (opt_desk_range_set &&
		    (ref->desks[i].nr < opt_desk_first ||
			ref->desks[i].nr > opt_desk_last))
			continue;
		desk_indices[(*n_visible)++] = i;
	}
}

static const pdesk_t *
visible_desk_at(const pout_t *ref, int vis_idx, int max_visible)
{
	int desk_indices[MAX_DESKS];
	int n_visible;

	build_visible_desks(ref, desk_indices, max_visible, &n_visible);
	if (vis_idx < 0 || vis_idx >= n_visible)
		return NULL;
	return &ref->desks[desk_indices[vis_idx]];
}

static int
visible_index_for_desk(const pout_t *ref, int desk_nr, int max_visible)
{
	int desk_indices[MAX_DESKS];
	int n_visible;

	build_visible_desks(ref, desk_indices, max_visible, &n_visible);
	for (int i = 0; i < n_visible; i++) {
		if (ref->desks[desk_indices[i]].nr == desk_nr)
			return i;
	}
	return -1;
}

static void
do_render(const state_t *st)
{
	if (!surface_configured || win_w <= 0 || win_h <= 0)
		return;

	if (st->n_outs == 0)
		return;

	layout_t L;
	compute_layout(st, &L);
	maybe_apply_initial_size(st, &L);
	if (initial_size_done)
		compute_layout(st, &L);

	canvas_t cv;
	if (!canvas_from_outputs(st, &cv))
		return;

	/*
	 * Reset the overlay-handle registry; the page-grid pass
	 * republishes plate hit-rects as it draws them.  This makes
	 * press-based hit-testing always agree with what's currently
	 * on screen, even when the layout reflows mid-session.
	 */
	n_overlay_handles = 0;

	void		 *px  = NULL;
	struct wl_buffer *buf = shm_buf(win_w, win_h, &px);
	if (!buf)
		return;

	cairo_surface_t *cs = cairo_image_surface_create_for_data(px,
	    CAIRO_FORMAT_ARGB32, win_w, win_h, win_w * 4);
	cairo_t		*cr = cairo_create(cs);

	/* Outer background */
	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
	cow_module_cairo_set_source(cr, cfg.background);
	cairo_paint(cr);
	cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

	const pout_t *ref = reference_output(st);
	if (!ref)
		goto commit;

	/* Build the list of desk indices we will draw, in grid order. */
	int desk_indices[MAX_DESKS];
	int n_visible = 0;
	if (opt_current_only) {
		const pout_t *cur = current_only_output(st);
		if (cur != NULL)
			ref = cur;
	}
	build_visible_desks(ref, desk_indices, L.rows * L.cols, &n_visible);

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
		 * info is conveyed through the monitor labels; a blanket
		 * desk-level highlight there would be arbitrary.
		 */
		int is_active = has_canonical_current_desk(st) &&
		    d->nr == ref->current_desk;

		/* Desk cell background */
		cow_module_cairo_fill(cr, cfg.desk_bg, dx, dy, L.cell_w,
		    L.cell_h);

		/* Chrome: desk label + monitor-label row. */
		draw_desk_chrome(cr, st, &L, d, dx, dy, is_active);

		/* Page grid: one unified canvas per desk, windows from
		 * all monitors. */
		int gy = dy + L.chrome_h;
		int gh = L.cell_h - L.chrome_h;
		if (gh < 1)
			gh = 1;
		draw_desk_pages(cr, st, &L, ref, &cv, d, dx, gy, L.cell_w, gh);

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

	/*
	 * Drag ghost: while a monitor-plate drag is in flight, paint
	 * a translucent rectangle in the dragged monitor's hue at
	 * the cursor's current page-cell.
	 */
	if (dragging && drag_monitor[0] != '\0') {
		int	    desk_nr, pc, pr, tab_idx;
		const char *out_dummy = NULL;
		if (hit_test_page(st, drag_cur_x, drag_cur_y, &desk_nr, &pc,
			&pr, &tab_idx, &out_dummy) &&
		    desk_nr >= 0 && pc >= 0 && pr >= 0) {
			int vis_idx = visible_index_for_desk(ref, desk_nr,
			    L.rows * L.cols);
			if (vis_idx < 0)
				goto commit_drag_ghost;
			int row, col;
			desk_index_to_rc(&L, vis_idx, &row, &col);
			int gdx, gdy;
			desk_cell_origin(&L, row, col, &gdx, &gdy);

			int    gx     = gdx;
			int    gy     = gdy + L.chrome_h;
			int    gw     = L.cell_w;
			int    gh     = L.cell_h - L.chrome_h;
			int    gpcols = ref->page_cols > 0 ? ref->page_cols : 1;
			int    gprows = ref->page_rows > 0 ? ref->page_rows : 1;
			double cw     = (double)gw / gpcols;
			double ch     = (double)gh / gprows;

			double mr, mg, mb;
			monitor_colour(drag_monitor, &mr, &mg, &mb);
			cairo_set_source_rgba(cr, mr, mg, mb, 0.35);
			cairo_rectangle(cr, gx + pc * cw, gy + pr * ch, cw, ch);
			cairo_fill(cr);

			cairo_set_source_rgba(cr, mr, mg, mb, 1.0);
			cairo_set_line_width(cr, 2.0);
			cairo_set_dash(cr, NULL, 0, 0);
			cairo_rectangle(cr, gx + pc * cw + 1, gy + pr * ch + 1,
			    cw - 2, ch - 2);
			cairo_stroke(cr);
		}
	}
commit_drag_ghost:

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
 * piece of chrome (label band, monitor-label row, page grid) the
 * pointer is over.  All "out" values are -1 / 0 on miss.
 *
 *   *vis_idx    : index into the visible-desks array.
 *   *tab_idx    : if the pointer is in the monitor-label row, the
 *                 label slot index (0..mon_tab_count-1).  -1 otherwise.
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
		/* Inside the monitor-label row. */
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
resolve_visible_desk(const pout_t *ref, const layout_t *L, int vis_idx)
{
	if (ref == NULL)
		return NULL;
	if (L == NULL)
		return NULL;
	return visible_desk_at(ref, vis_idx, L->rows * L->cols);
}

/*
 * `prefer_output`, when non-NULL, is the user's currently-active
 * monitor (cowpager's sticky `cp_user_output`).  When several stacked
 * thumbnails contain the click point, a hit on that monitor wins over
 * hits on others.  
 */
static const pwin_t *
hit_test_window(const state_t *st, double x, double y,
    const char *prefer_output)
{
	if (!st->n_outs)
		return NULL;
	layout_t L;
	compute_layout(st, &L);

	const pout_t *ref = opt_current_only ? current_only_output(st) :
	    reference_output(st);
	if (!ref)
		return NULL;
	canvas_t cv;
	if (!canvas_from_outputs(st, &cv))
		return NULL;

	int vis_idx, tab_idx, in_page;
	locate(st, x, y, &L, &vis_idx, &tab_idx, &in_page);
	if (!in_page)
		return NULL;

	const pdesk_t *d = resolve_visible_desk(ref, &L, vis_idx);
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

	int pcols = ref->page_cols > 0 ? ref->page_cols : 1;
	int prows = ref->page_rows > 0 ? ref->page_rows : 1;
	if (sw <= 0 || sh <= 0)
		return NULL;

	double cell_w = (double)sw / pcols;
	double cell_h = (double)sh / prows;

	/*
	 * Collect every window whose thumbnail rect contains the
	 * click.  Stacked maximized windows on the same desk all draw
	 * at the same coordinates, so the loop typically yields more
	 * than one hit; cycling between them on repeat clicks at the
	 * same point (below) lets the user pick a specific one.
	 */
	const pwin_t *hits[64];
	int	      n_hits = 0;
	for (int wi = 0; wi < st->n_wins; wi++) {
		const pwin_t *w = &st->wins[wi];
		if (w->desk != d->nr)
			continue;
		if (!monitor_visible(w->output))
			continue;
		if (is_self_window(w))
			continue;

		const pout_t *wo = output_for_window(st, w);
		if (!output_in_canvas(&cv, wo))
			continue;
		int wow		 = (wo && wo->output_w > 0) ? wo->output_w : 1;
		int woh		 = (wo && wo->output_h > 0) ? wo->output_h : 1;
		if (wow <= 0 || woh <= 0)
			continue;
		int desk_w = pcols * wow;
		int desk_h = prows * woh;
		int vx = w->iconified ? w->icon_vx : w->vx;
		int vy = w->iconified ? w->icon_vy : w->vy;
		int width = w->iconified ? w->icon_w : w->w;
		int height = w->iconified ? w->icon_w : w->h;
		if (vx < 0 || vx >= desk_w)
			continue;
		if (vy < 0 || vy >= desk_h)
			continue;

		double wx_ = sx + (canvas_page_x(&cv, wo, vx) / cv.w) * cell_w;
		double wy_ = sy + (canvas_page_y(&cv, wo, vy) / cv.h) * cell_h;
		double ww = ((double)width / wow) *
		    (cell_w * wo->output_w / cv.w);
		double wh = ((double)height / woh) *
		    (cell_h * wo->output_h / cv.h);
		if (ww < 3)
			ww = 3;
		if (wh < 3)
			wh = 3;
		if (w->iconified) {
			double size = cfg.window_icons ? cfg.window_icon_size : 7;
			wx_ += (ww - size) / 2.0;
			wy_ += (wh - size) / 2.0;
			ww = size;
			wh = size;
		}
		if (opt_verbose) {
			fprintf(stderr,
			    "cowpager:   win id=%s desk=%d out=%s vx=%d "
			    "vy=%d w=%d h=%d -> rect [%.0f,%.0f %.0fx%.0f] "
			    "click=(%.0f,%.0f)\n",
			    w->id, w->desk, w->output, vx, vy, width, height,
			    wx_, wy_, ww, wh, x, y);
		}
		if (x >= wx_ && x < wx_ + ww && y >= wy_ && y < wy_ + wh) {
			if (n_hits < (int)(sizeof(hits) / sizeof(hits[0])))
				hits[n_hits++] = w;
		}
	}

	if (n_hits == 0)
		return NULL;
	if (n_hits == 1)
		return hits[0];

	/*
	 * Stacked-thumbnail tie-breaking.  Several windows on the
	 * same desk routinely have identical thumbnail rectangles
	 * (e.g. all maximized on different outputs but on the same
	 * desk -- they overlap pixel-for-pixel in the page grid).
	 */
	static double last_x = -1, last_y = -1;
	static int    last_cycle;
	const int     JITTER = 4;

	int same_point = (last_x >= 0 && fabs(x - last_x) <= JITTER &&
	    fabs(y - last_y) <= JITTER);

	int pick;
	if (same_point) {
		pick = (last_cycle + 1) % n_hits;
	} else {
		pick = -1;
		if (prefer_output != NULL && prefer_output[0] != '\0') {
			for (int i = 0; i < n_hits; i++) {
				if (strcmp(hits[i]->output, prefer_output) ==
				    0) {
					pick = i;
					break;
				}
			}
		}
		if (pick < 0) {
			for (int i = 0; i < n_hits; i++) {
				if (hits[i]->focused) {
					pick = i;
					break;
				}
			}
		}
		if (pick < 0)
			pick = n_hits - 1;
	}
	last_x	   = x;
	last_y	   = y;
	last_cycle = pick;

	if (opt_verbose) {
		fprintf(stderr,
		    "cowpager:   stacked %d hits; %s pick=%d id=%s "
		    "out=%s prefer=%s\n",
		    n_hits, same_point ? "cycling" : "fresh", pick,
		    hits[pick]->id, hits[pick]->output,
		    prefer_output ? prefer_output : "(none)");
	}
	return hits[pick];
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

	const pout_t *ref = opt_current_only ? current_only_output(st) :
	    reference_output(st);
	if (!ref)
		return 0;
	canvas_t cv;
	if (!canvas_from_outputs(st, &cv))
		return 0;

	int vis_idx, tab_idx, in_page;
	locate(st, x, y, &L, &vis_idx, &tab_idx, &in_page);
	if (vis_idx < 0)
		return 0;

	const pdesk_t *d = resolve_visible_desk(ref, &L, vis_idx);
	if (d == NULL)
		return 0;
	*desk_nr = d->nr;

	if (tab_idx >= 0) {
		/* Click in a monitor label.  Look up which monitor that
		 * label corresponds to and return its name. */
		const pout_t *tab[MAX_OUTS];
		int	      n_tab = tab_outputs(st, tab, MAX_OUTS);
		if (tab_idx < n_tab) {
			*tab_out     = tab_idx;
			*out_for_cmd = tab[tab_idx]->name;
		}
		return 1;
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
		/* Match FvwmPager: a page click targets the monitor whose
		 * physical rectangle contains that point in the pager canvas. */
		double page_w = (double)sw / pcols;
		double page_h = (double)sh / prows;
		double cx = ((x - sx) - pc * page_w) / page_w * cv.w;
		double cy = ((y - sy) - pr * page_h) / page_h * cv.h;
		const pout_t *out = output_at_canvas_point(st, &cv, cx, cy);
		if (out != NULL)
			*out_for_cmd = out->name;
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
	apply_theme_defaults(&st);
	do_render(&st);
}

static const struct xdg_surface_listener xdg_surface_listener = {
	.configure = xdg_surface_configure,
};

static void
xdg_toplevel_configure(__unused void *data, __unused struct xdg_toplevel *tl,
    int32_t w, int32_t h, __unused struct wl_array *states)
{
	if (w > 0) {
		win_w = w;
		configured_w_seen = 1;
	}
	if (h > 0) {
		win_h = h;
		configured_h_seen = 1;
	}
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
ptr_enter(__unused void *d, __unused struct wl_pointer *p, uint32_t serial,
    __unused struct wl_surface *sf, wl_fixed_t x, wl_fixed_t y)
{
	cow_module_cursor_set_default(&cursor, serial);
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

	/*
	 * If a monitor-drag is in flight, ask the main loop to
	 * re-render so the ghost rectangle tracks the cursor.  This
	 * is cheap because do_render() is the only renderer and it
	 * already runs on g_dirty.
	 */
	if (dragging) {
		drag_cur_x = ptr_x;
		drag_cur_y = ptr_y;
		g_dirty	   = 1;
		char b	   = 1;
		(void)write(wake_pipe[1], &b, 1);
	}
}

/*
 * Hit-test the overlay-plate registry.  Returns the monitor name of
 * the plate under (x,y), or NULL.
 */
static const char *
hit_overlay_plate(double x, double y)
{
	for (int i = 0; i < n_overlay_handles; i++) {
		const struct overlay_handle *h = &overlay_handles[i];
		if (x >= h->px && x < h->px + h->pw && y >= h->py &&
		    y < h->py + h->ph)
			return h->name;
	}
	return NULL;
}

static void
ptr_button(__unused void *d, __unused struct wl_pointer *p, __unused uint32_t s,
    __unused uint32_t t, __unused uint32_t btn, uint32_t state)
{
	pthread_mutex_lock(&g_lock);
	state_t st = g_state;
	char	user_out[64];
	strlcpy(user_out, cp_user_output, sizeof(user_out));
	pthread_mutex_unlock(&g_lock);

	/*
	 * Press path: maybe start a monitor-drag from an overlay
	 * plate; otherwise fall through to the click-handling code
	 * below.
	 *
	 * Release path: if a drag is in flight, resolve the drop
	 * target and emit the IPC commands; consume the release.
	 * Otherwise do nothing (the press already did the work).
	 */
	if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
		const char *plate_mon = hit_overlay_plate(ptr_x, ptr_y);
		if (plate_mon != NULL) {
			dragging     = 1;
			drag_start_x = ptr_x;
			drag_start_y = ptr_y;
			drag_cur_x   = ptr_x;
			drag_cur_y   = ptr_y;
			strlcpy(drag_monitor, plate_mon,
			    sizeof(drag_monitor));
			if (opt_verbose)
				fprintf(stderr,
				    "cowpager: drag-start mon=%s at "
				    "(%.0f,%.0f)\n",
				    drag_monitor, ptr_x, ptr_y);
			g_dirty = 1;
			char b	= 1;
			(void)write(wake_pipe[1], &b, 1);
			return;
		}
		/* not a plate press: fall through to plain-click logic */
	} else {
		/* RELEASED */
		if (dragging) {
			int desk_nr = -1, pc = -1, pr = -1, tab_idx = -1;
			const char *out_dummy = NULL;
			(void)hit_test_page(&st, ptr_x, ptr_y, &desk_nr, &pc,
			    &pr, &tab_idx, &out_dummy);

			int dx	  = (int)(ptr_x - drag_start_x);
			int dy	  = (int)(ptr_y - drag_start_y);
			int moved = (dx * dx + dy * dy) > 9; /* >3px */

			if (opt_verbose)
				fprintf(stderr,
				    "cowpager: drag-end mon=%s -> desk=%d "
				    "pc=%d pr=%d (moved=%d)\n",
				    drag_monitor, desk_nr, pc, pr, moved);

			if (moved && desk_nr >= 0 && drag_monitor[0] != '\0') {
				/* desk -t <mon> -d <desk> */
				char cmd[160];
				snprintf(cmd, sizeof(cmd), "desk -t %s -d %d",
				    drag_monitor, desk_nr);
				ipc_send(cmd);

				/* scroll only if a specific page was hit
				 * (not just the desk-label band). */
				if (pc >= 0 && pr >= 0) {
					snprintf(cmd, sizeof(cmd),
					    "scroll -t %s -a %d,%d",
					    drag_monitor, pc, pr);
					ipc_send(cmd);
				}
			}

			dragging	= 0;
			drag_monitor[0] = '\0';
			g_dirty		= 1;
			char b		= 1;
			(void)write(wake_pipe[1], &b, 1);
			return;
		}
		/* Plain release with no drag: nothing else to do.  The
		 * actual click was handled on press. */
		return;
	}

	/*
	 * Pick a default target output for click commands that don't
	 * already point at a specific monitor.
	 */
	const char *default_out = cp_host_output();
	if (default_out == NULL && user_out[0] != '\0')
		default_out = user_out;
	if (default_out == NULL) {
		const pout_t *r = reference_output(&st);
		if (r != NULL)
			default_out = r->name;
	}

	if (opt_verbose)
		fprintf(stderr,
		    "cowpager: click at (%.0f, %.0f) win=%dx%d "
		    "default_out=%s\n",
		    ptr_x, ptr_y, win_w, win_h,
		    default_out ? default_out : "(none)");

	/* Window miniature wins over page click.  Navigate to the
	 * window's desk/page first so fullscreen/maximized windows on a
	 * different page actually become visible; then focus it.
	 */
	const pwin_t *hw = hit_test_window(&st, ptr_x, ptr_y, default_out);
	if (opt_verbose)
		fprintf(stderr, "cowpager: hit_test_window -> %s\n",
		    (hw && hw->id[0]) ? hw->id : "(none)");
	if (hw && hw->id[0]) {
		/* Find which output the window lives on.  Fall back to
		 * the user's sticky output if the IPC didn't tell us.
		 */
		const pout_t *target = NULL;
		if (hw->output[0]) {
			for (int i = 0; i < st.n_outs; i++)
				if (strcmp(st.outs[i].name, hw->output) == 0) {
					target = &st.outs[i];
					break;
				}
		}
		if (!target && default_out != NULL) {
			for (int i = 0; i < st.n_outs; i++)
				if (strcmp(st.outs[i].name, default_out) == 0) {
					target = &st.outs[i];
					break;
				}
		}
		if (!target)
			target = reference_output(&st);

		/*
		 * Always pass -t to desk and scroll: even in global
		 * mode, scroll is per-output, and naming the target
		 * explicitly insulates us from cow's current_output
		 * having become cowpager's own monitor as a side
		 * effect of this very click.
		 */
		const char *tname = (target != NULL) ? target->name : NULL;

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
		snprintf(cmd, sizeof(cmd), "focus -t '#%s'", hw->id);
		ipc_send(cmd);
		return;
	}

	int	    desk_nr, pc, pr, tab_idx;
	const char *out_name = NULL;
	if (!hit_test_page(&st, ptr_x, ptr_y, &desk_nr, &pc, &pr, &tab_idx,
		&out_name))
		return;

	if (opt_verbose)
		fprintf(stderr,
		    "cowpager: hit_test_page -> desk=%d pc=%d pr=%d "
		    "tab=%d out=%s\n",
		    desk_nr, pc, pr, tab_idx, out_name ? out_name : "(none)");

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
	 * Page-area click: switch the active output to this desk/page.
	 */
	const char *target_name = out_name;
	if (target_name == NULL || target_name[0] == '\0')
		target_name = default_out;

	const pout_t *target = NULL;
	if (target_name != NULL) {
		for (int i = 0; i < st.n_outs; i++)
			if (strcmp(st.outs[i].name, target_name) == 0) {
				target = &st.outs[i];
				break;
			}
	}
	if (target == NULL)
		target = reference_output(&st);
	if (target != NULL)
		target_name = target->name;

	if (desk_nr >= 0) {
		if (target && desk_nr != target->current_desk) {
			char cmd[128];
			if (target_name != NULL)
				snprintf(cmd, sizeof(cmd), "desk -t %s -d %d",
				    target_name, desk_nr);
			else
				snprintf(cmd, sizeof(cmd), "desk -d %d",
				    desk_nr);
			ipc_send(cmd);
		}
	}

	if (pc >= 0 && pr >= 0) {
		char cmd[128];
		if (target_name != NULL)
			snprintf(cmd, sizeof(cmd), "scroll -t %s -a %d,%d",
			    target_name, pc, pr);
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
		cow_module_cursor_set_pointer(&cursor, pointer);
	} else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && pointer) {
		cow_module_cursor_clear_pointer(&cursor, pointer);
		wl_pointer_destroy(pointer);
		pointer = NULL;
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

/* ----------------------------------------------------------------------
 * wl_output: track each advertised output and its name.
 *
 * We only need wl_output's `name` event (v4); the rest of the
 * geometry/mode/scale stream is irrelevant for the pager.
 * ---------------------------------------------------------------------- */

static struct cp_output *
cp_output_for_wl(const struct wl_output *wo)
{
	for (int i = 0; i < cp_n_outputs; i++)
		if (cp_outputs[i].wl_output == wo)
			return &cp_outputs[i];
	return NULL;
}

static struct cp_output *
cp_output_by_name(const char *n)
{
	if (n == NULL || n[0] == '\0')
		return NULL;
	for (int i = 0; i < cp_n_outputs; i++)
		if (strcmp(cp_outputs[i].name, n) == 0)
			return &cp_outputs[i];
	return NULL;
}

static void
out_geometry(__unused void *d, __unused struct wl_output *wo,
    __unused int32_t x, __unused int32_t y, __unused int32_t pw,
    __unused int32_t ph, __unused int32_t sub, __unused const char *make,
    __unused const char *model, __unused int32_t transform)
{
}

static void
out_mode(__unused void *d, __unused struct wl_output *wo,
    __unused uint32_t flags, __unused int32_t w, __unused int32_t h,
    __unused int32_t refresh)
{
}

static void
out_done(__unused void *d, __unused struct wl_output *wo)
{
}

static void
out_scale(__unused void *d, __unused struct wl_output *wo,
    __unused int32_t scale)
{
}

static void
out_name(__unused void *d, struct wl_output *wo, const char *name)
{
	struct cp_output *o = cp_output_for_wl(wo);
	if (o == NULL || name == NULL)
		return;
	strlcpy(o->name, name, sizeof(o->name));
}

static void
out_description(__unused void *d, __unused struct wl_output *wo,
    __unused const char *desc)
{
}

static const struct wl_output_listener out_listener = {
	.geometry    = out_geometry,
	.mode	     = out_mode,
	.done	     = out_done,
	.scale	     = out_scale,
	.name	     = out_name,
	.description = out_description,
};

static void
reg_global(__unused void *d, struct wl_registry *reg, uint32_t name,
    const char *iface, uint32_t ver)
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
	} else if (!strcmp(iface, wl_output_interface.name)) {
		if (cp_n_outputs >= CP_MAX_TRACKED_OUTPUTS)
			return;
		/* Bind v4 (so we get wl_output.name); fall back to
		 * lower if the compositor doesn't support it.  cow
		 * advertises wl_output via river so v4 is expected. */
		uint32_t	  bind_ver = ver >= 4 ? 4 : ver;
		struct cp_output *po	   = &cp_outputs[cp_n_outputs];
		po->wl_output		   = wl_registry_bind(reg, name,
				 &wl_output_interface, bind_ver);
		po->global_name		   = name;
		po->name[0]		   = '\0';
		po->on_surface		   = 0;
		if (po->wl_output != NULL) {
			wl_output_add_listener(po->wl_output, &out_listener,
			    NULL);
			cp_n_outputs++;
		}
	} else
		cow_module_cursor_handle_global(&cursor, reg, name, iface, ver);
}

static void
reg_remove(__unused void *d, __unused struct wl_registry *r, uint32_t n)
{
	for (int i = 0; i < cp_n_outputs; i++) {
		if (cp_outputs[i].global_name != n)
			continue;
		if (cp_outputs[i].wl_output != NULL)
			wl_output_destroy(cp_outputs[i].wl_output);
		/* Compact the array so the index slots stay dense. */
		int last = cp_n_outputs - 1;
		if (i != last)
			cp_outputs[i] = cp_outputs[last];
		memset(&cp_outputs[last], 0, sizeof(cp_outputs[last]));
		cp_n_outputs--;
		break;
	}
}

static const struct wl_registry_listener reg_listener = {
	.global	       = reg_global,
	.global_remove = reg_remove,
};

static void
surface_enter(__unused void *d, __unused struct wl_surface *s,
    struct wl_output *wo)
{
	struct cp_output *po = cp_output_for_wl(wo);
	if (po != NULL)
		po->on_surface = 1;
}

static void
surface_leave(__unused void *d, __unused struct wl_surface *s,
    struct wl_output *wo)
{
	struct cp_output *po = cp_output_for_wl(wo);
	if (po != NULL)
		po->on_surface = 0;
}

static const struct wl_surface_listener surface_listener = {
	.enter = surface_enter,
	.leave = surface_leave,
};

/*
 * Is cowpager's own surface currently sitting on the named output?
 */
static int
cp_surface_on_output(const char *name)
{
	struct cp_output *po = cp_output_by_name(name);
	return po != NULL && po->on_surface;
}

/*
 * Return the name of the output cowpager's surface is currently
 * sitting on, or NULL if no enter event has arrived yet (or every
 * tracked output is unnamed).
 */
static const char *
cp_host_output(void)
{
	for (int i = 0; i < cp_n_outputs; i++) {
		if (cp_outputs[i].on_surface && cp_outputs[i].name[0] != '\0')
			return cp_outputs[i].name;
	}
	return NULL;
}

static void
parse_status(const char *json)
{
	cJSON *root = cJSON_Parse(json);
	if (!root)
		return;

	static state_t st;
	memset(&st, 0, sizeof(st));

	cJSON *dcfg = cJSON_GetObjectItem(root, "desktop_configuration");
	if (cJSON_IsString(dcfg))
		strlcpy(st.desktop_config, dcfg->valuestring,
		    sizeof(st.desktop_config));
	else
		strlcpy(st.desktop_config, "global",
		    sizeof(st.desktop_config));

	cJSON *theme = cJSON_GetObjectItem(root, "theme");
	(void)cow_module_theme_parse(theme, &st.theme);
		
	cJSON *titlebar = cJSON_GetObjectItem(theme, "titlebar");
	cJSON  *v = cJSON_GetObjectItem(titlebar, "font");
	if (cJSON_IsString(v))
		strlcpy(st.theme.font, v->valuestring,
			sizeof(st.theme.font));
	else
		strlcpy(st.theme.font, "Sans 8",
			sizeof(st.theme.font));

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
		strlcpy(po->f, v->valuestring, z);
		GS("name", name, 64)
		GN("number", number)
		GN("x", output_x)
		GN("y", output_y)
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
		cJSON *background = cJSON_GetObjectItem(out, "background");
		po->has_background = cJSON_IsString(background) &&
		    background->valuestring != NULL &&
		    cow_colour_parse(background->valuestring, po->background);
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
			v = cJSON_GetObjectItem(dk, "name");
			if (cJSON_IsString(v))
				strlcpy(pd->name, v->valuestring,
				    sizeof(pd->name));
			v = cJSON_GetObjectItem(dk, "windows");
			if (cJSON_IsNumber(v))
				pd->window_count = (int)v->valuedouble;
			pd->active = cJSON_IsTrue(
			    cJSON_GetObjectItem(dk, "active"));
		}
		po->n_desks = di;
	}
	st.n_outs = oi;

	cJSON_Delete(root);

	/*
	 * Maintain the "user current output" sticky.  Pick the output
	 * cow has flagged as current=true UNLESS that output is the
	 * one cowpager's own surface is currently sitting on -- in
	 * which case the user just clicked the pager and we want to
	 * preserve the previous value so subsequent desk/scroll
	 * commands target the user's actual workspace, not cowpager's.
	 *
	 * If no output is current (rare, transient), or every current
	 * output is cowpager's own, leave the sticky alone -- the
	 * previously cached value is the best guess.
	 */
	pthread_mutex_lock(&g_lock);
	for (int i = 0; i < st.n_outs; i++) {
		if (!st.outs[i].current)
			continue;
		if (cp_surface_on_output(st.outs[i].name))
			continue;
		strlcpy(cp_user_output, st.outs[i].name,
		    sizeof(cp_user_output));
		break;
	}
	strlcpy(st.user_output, cp_user_output, sizeof(st.user_output));
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

	static pwin_t wins[MAX_WINS];
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
		strlcpy(pw->f, v->valuestring, z);
		GS("id", id, 128)
		GS("output", output, 64)
		GS("app_id", app_id, 64)
		GS("icon", icon, sizeof(pw->icon))
		GN("desk", desk)
		GN("vx", vx)
		GN("vy", vy)
		GN("width", w)
		GN("height", h)
		GN("icon_vx", icon_vx)
		GN("icon_vy", icon_vy)
		GN("icon_width", icon_w)
		GN("icon_height", icon_h)
		pw->focused = cJSON_IsTrue(cJSON_GetObjectItem(w, "focused"));
		pw->iconified = cJSON_IsTrue(cJSON_GetObjectItem(w, "iconified"));
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
	int fd = cow_module_status_connect("cowpager",
	    COW_MODULE_STATUS_RETRIES, COW_MODULE_STATUS_RETRY_DELAY);
	if (fd < 0)
		return NULL;

	FILE *f = fdopen(fd, "r");
	if (!f) {
		close(fd);
		return NULL;
	}
	setlinebuf(f);
	static char line[COW_MODULE_STATUS_LINE_MAX];
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

static bool
load_pager_style(char **fields, int n, const char **why)
{
	if (n != 3) {
		*why = "bad style";
		return false;
	}

	float *slot = NULL;
	if (strcmp(fields[1], "background") == 0) {
		slot = cfg.background;
		cfg.background_set = 1;
	} else if (strcmp(fields[1], "desk.bg") == 0) {
		slot = cfg.desk_bg;
		cfg.desk_bg_set = 1;
	} else if (strcmp(fields[1], "page.bg") == 0) {
		slot = cfg.page_bg;
		cfg.page_bg_set = 1;
	} else if (strcmp(fields[1], "page.active.bg") == 0) {
		slot = cfg.page_active_bg;
		cfg.page_active_bg_set = 1;
	} else if (strcmp(fields[1], "label.bg") == 0) {
		slot = cfg.label_bg;
		cfg.label_bg_set = 1;
	} else if (strcmp(fields[1], "label.active.bg") == 0) {
		slot = cfg.label_active_bg;
		cfg.label_active_bg_set = 1;
	} else if (strcmp(fields[1], "label.fg") == 0) {
		slot = cfg.label_fg;
		cfg.label_fg_set = 1;
	} else if (strcmp(fields[1], "label.inactive.fg") == 0) {
		slot = cfg.label_inactive_fg;
		cfg.label_inactive_fg_set = 1;
	} else if (strcmp(fields[1], "window.focused.bg") == 0) {
		slot = cfg.window_focused_bg;
		cfg.window_focused_bg_set = 1;
	} else {
		*why = "unknown style";
		return false;
	}
	if (!cow_colour_parse(fields[2], slot)) {
		*why = "bad colour";
		return false;
	}
	if (strcmp(fields[1], "background") == 0) {
		if (!cfg.desk_bg_set)
			memcpy(cfg.desk_bg, cfg.background, sizeof(cfg.desk_bg));
		if (!cfg.page_bg_set)
			memcpy(cfg.page_bg, cfg.background, sizeof(cfg.page_bg));
		if (!cfg.page_active_bg_set)
			memcpy(cfg.page_active_bg, cfg.background,
			    sizeof(cfg.page_active_bg));
	}
	return true;
}

static bool
load_show_pager(char **fields, int n, const char **why, bool collect)
{
	if (n < 3 || strcmp(fields[1], "pager") != 0) {
		*why = "bad show";
		return false;
	}
	if (!collect)
		return true;

	for (int i = 2; i < n; i++) {
		if (!pager_selection_add(fields[i])) {
			*why = "too many pagers";
			return false;
		}
	}
	return true;
}

static bool
scan_pager_config_line(const struct cow_module_config_line *line,
    const char **why, void *data)
{
	struct pager_load_ctx *ctx = data;
	char **fields = line->fields;
	int n = line->nfields;

	if (ctx->depth == 0 && strcmp(fields[0], "show") == 0) {
		bool collect = show_group_count == instance_index;

		if (!load_show_pager(fields, n, why, collect))
			return false;
		show_group_count++;
	}

	if (strcmp(fields[0], "}") == 0 && n == 1) {
		if (ctx->depth > 0)
			ctx->depth--;
	} else if (strcmp(fields[n - 1], "{") == 0)
		ctx->depth++;

	return true;
}

static bool
load_pager_option(char **fields, int n, const char **why)
{
	int v;
	bool b;

	if (strcmp(fields[0], "rows") == 0) {
		if (n != 2 || !cow_module_config_parse_int(fields[1], &v) ||
		    v < 1) {
			*why = "bad rows";
			return false;
		}
		opt_rows = cfg.rows = v;
	} else if (strcmp(fields[0], "columns") == 0) {
		if (n != 2 || !cow_module_config_parse_int(fields[1], &v) ||
		    v < 1) {
			*why = "bad columns";
			return false;
		}
		opt_cols = cfg.cols = v;
	} else if (strcmp(fields[0], "monitors") == 0) {
		if (n != 2 || !parse_monitor_list(fields[1], 1)) {
			*why = "bad monitors";
			return false;
		}
	} else if (strcmp(fields[0], "desks") == 0) {
		int first, last;
		if (n != 3 ||
		    !cow_module_config_parse_int(fields[1], &first) ||
		    !cow_module_config_parse_int(fields[2], &last) ||
		    !set_desk_range(first, last)) {
			*why = "bad desks";
			return false;
		}
	} else if (strcmp(fields[0], "current-only") == 0) {
		if (n != 2 || !cow_module_config_parse_bool(fields[1], &b)) {
			*why = "bad current-only";
			return false;
		}
		set_current_only(b);
	} else if (strcmp(fields[0], "desktop-labels") == 0) {
		if (n != 2 || !cow_module_config_parse_bool(fields[1], &b)) {
			*why = "bad desktop-labels";
			return false;
		}
		cfg.desktop_labels = b ? 1 : 0;
	} else if (strcmp(fields[0], "monitor-labels") == 0) {
		if (n != 2 || !cow_module_config_parse_bool(fields[1], &b)) {
			*why = "bad monitor-labels";
			return false;
		}
		cfg.monitor_labels = b ? 1 : 0;
	} else if (strcmp(fields[0], "label-highlight") == 0) {
		if (n != 2 || !cow_module_config_parse_bool(fields[1], &b)) {
			*why = "bad label-highlight";
			return false;
		}
		cfg.label_highlight = b ? 1 : 0;
	} else if (strcmp(fields[0], "desktop-scale") == 0) {
		if (n != 2 || !cow_module_config_parse_int(fields[1], &v) ||
		    v < 1) {
			*why = "bad desktop-scale";
			return false;
		}
		cfg.desktop_scale = v;
	} else if (strcmp(fields[0], "window-icons") == 0) {
		if (n != 2 || !cow_module_config_parse_bool(fields[1], &b)) {
			*why = "bad window-icons";
			return false;
		}
		cfg.window_icons = b ? 1 : 0;
	} else if (strcmp(fields[0], "window-icon-size") == 0) {
		if (n != 2 || !cow_module_config_parse_int(fields[1], &v) ||
		    v < 1 || v > 256) {
			*why = "bad window-icon-size";
			return false;
		}
		cfg.window_icon_size = v;
	} else if (strcmp(fields[0], "style") == 0) {
		return load_pager_style(fields, n, why);
	} else {
		*why = "unknown command";
		return false;
	}
	return true;
}

static bool
load_pager_config_line(const struct cow_module_config_line *line,
    const char **why, void *data)
{
	struct pager_load_ctx *ctx = data;
	char **fields = line->fields;
	int n = line->nfields;

	if (strcmp(fields[0], "}") == 0) {
		if (n != 1 || ctx->depth == 0) {
			*why = "bad block close";
			return false;
		}
		ctx->depth--;
		if (ctx->depth == 0)
			ctx->in_selected_pager = 0;
		return true;
	}

	if (strcmp(fields[0], "show") == 0)
		return load_show_pager(fields, n, why, false);

	if (strcmp(fields[0], "pager") == 0) {
		if (n != 3 || strcmp(fields[2], "{") != 0) {
			*why = "bad pager";
			return false;
		}
		if (ctx->depth != 0) {
			*why = "nested pager";
			return false;
		}

		int idx = pager_selection_find(fields[1]);
		ctx->depth = 1;
		ctx->in_selected_pager = idx != -1;
		if (idx != -1)
			selected_pagers[idx].seen = 1;
		return true;
	}

	if (ctx->depth > 0 && !ctx->in_selected_pager)
		return true;

	return load_pager_option(fields, n, why);
}

static bool
load_pager_config(const char *path)
{
	struct pager_load_ctx scan_ctx = { 0 };
	struct pager_load_ctx load_ctx = { 0 };

	pager_selections_free();
	show_group_count = 0;
	if (!cow_module_config_load(PROG, path, MAX_FIELDS,
		scan_pager_config_line, &scan_ctx))
		return false;
	if (show_group_count > 0 && n_selected_pagers == 0) {
		fprintf(stderr, "%s: %s: no show pager line %d\n", PROG, path,
		    instance_index);
		return false;
	}

	if (!cow_module_config_load(PROG, path, MAX_FIELDS,
		load_pager_config_line, &load_ctx))
		return false;

	for (int i = 0; i < n_selected_pagers; i++) {
		if (selected_pagers[i].seen)
			continue;
		fprintf(stderr, "%s: %s: pager not found: %s\n", PROG, path,
		    selected_pagers[i].name);
		return false;
	}

	return true;
}

static bool
parse_instance_index(void)
{
	const char *value = getenv(SHOW_INDEX_ENV);
	int idx;

	if (value == NULL || *value == '\0')
		return true;
	spawn_extra_instances = false;
	if (!cow_module_config_parse_int(value, &idx) || idx < 0) {
		fprintf(stderr, "%s: bad %s\n", PROG, SHOW_INDEX_ENV);
		return false;
	}
	instance_index = idx;
	return true;
}

static void
build_instance_title(void)
{
	size_t len;

	if (n_selected_pagers == 0 && opt_n_monitors == 1) {
		snprintf(instance_title, sizeof(instance_title), "%s:%s", PROG,
		    opt_monitors[0]);
		return;
	}

	if (n_selected_pagers == 0) {
		strlcpy(instance_title, PROG, sizeof(instance_title));
		return;
	}

	snprintf(instance_title, sizeof(instance_title), "%s:%s", PROG,
	    selected_pagers[0].name);
	len = strlen(instance_title);
	for (int i = 1; i < n_selected_pagers; i++) {
		int n = snprintf(instance_title + len, sizeof(instance_title) - len,
		    "+%s", selected_pagers[i].name);
		if (n < 0 || (size_t)n >= sizeof(instance_title) - len)
			break;
		len += (size_t)n;
	}
}

static void
spawn_config_instances(char *argv[])
{
	if (!spawn_extra_instances)
		return;
	for (int i = 1; i < show_group_count; i++) {
		pid_t pid = fork();

		if (pid != 0)
			continue;
		char idx[32];
		snprintf(idx, sizeof(idx), "%d", i);
		setenv(SHOW_INDEX_ENV, idx, 1);
		execvp(argv[0], argv);
		_exit(127);
	}
}

static char *
find_config_path(int argc, char *argv[], bool *explicit_path)
{
	*explicit_path = false;
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-f") == 0 && i + 1 < argc) {
			*explicit_path = true;
			return strdup(argv[i + 1]);
		}
		if (strncmp(argv[i], "-f", 2) == 0 && argv[i][2] != '\0') {
			*explicit_path = true;
			return strdup(argv[i] + 2);
		}
	}
	return cow_module_config_default_path("cowpager");
}

static void
usage(void)
{
	fprintf(stderr,
	    "usage: cowpager [-r rows] [-c cols] "
	    "[-m mon[,mon...]] [-1] [-v] [-h] [first last]\n"
	    "       cowpager [-f path]\n"
	    "  -r ROWS   arrange desks in ROWS rows\n"
	    "  -c COLS   arrange desks in COLS columns\n"
	    "  -m LIST   restrict pager to the listed\n"
	    "            monitors (comma-separated). A\n"
	    "            single monitor is tracked; otherwise\n"
	    "            the page canvas uses the monitor\n"
	    "            layout and labels use LIST order\n"
	    "  -1        show only the currently active desk\n"
	    "  FIRST LAST\n"
	    "            show desks FIRST through LAST, inclusive\n"
	    "  -v        log click decisions to stderr\n"
	    "  -f PATH   load cowpager config from PATH\n"
	    "  -h        show this help\n");
}

int
main(int argc, char *argv[])
{
	int opt;
	bool explicit_config = false;
	char *config_path = NULL;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-h") == 0) {
			usage();
			return 0;
		}
	}

	if (!parse_instance_index())
		return 1;

	pager_config_init(&cfg);
	config_path = find_config_path(argc, argv, &explicit_config);
	if (config_path != NULL) {
		if (access(config_path, R_OK) == 0) {
			if (!load_pager_config(config_path)) {
				free(config_path);
				return 1;
			}
			spawn_config_instances(argv);
		} else if (explicit_config) {
			fprintf(stderr, "%s: %s: %s\n", PROG, config_path,
			    strerror(errno));
			free(config_path);
			return 1;
		}
		free(config_path);
	}
	optind = 1;
	while ((opt = getopt(argc, argv, "r:c:f:m:1vh")) != -1) {
		switch (opt) {
		case 'r':
			opt_rows = atoi(optarg);
			if (opt_rows < 1)
				opt_rows = 1;
			cfg.rows = opt_rows;
			break;
		case 'c':
			opt_cols = atoi(optarg);
			if (opt_cols < 1)
				opt_cols = 1;
			cfg.cols = opt_cols;
			break;
		case 'f':
			break;
		case 'm': {
			if (!parse_monitor_list(optarg, 1))
				return 1;
			break;
		}
		case '1':
			set_current_only(true);
			break;
		case 'v':
			opt_verbose = 1;
			cfg.verbose = 1;
			break;
		case 'h':
			usage();
			return 0;
		default:
			return 1;
		}
	}
	if (argc - optind != 0) {
		int first, last;
		if (argc - optind != 2 ||
		    !cow_module_config_parse_int(argv[optind], &first) ||
		    !cow_module_config_parse_int(argv[optind + 1], &last) ||
		    !set_desk_range(first, last)) {
			usage();
			return 1;
		}
	}
	build_instance_title();

	if (pipe(wake_pipe) < 0) {
		perror("pipe");
		return 1;
	}
	fcntl(wake_pipe[0], F_SETFL, O_NONBLOCK);

	display = wl_display_connect(NULL);
	if (!display) {
		fprintf(stderr, "%s: no Wayland display\n", PROG);
		return 1;
	}

	struct wl_registry *reg = wl_display_get_registry(display);
	wl_registry_add_listener(reg, &reg_listener, NULL);
	wl_display_roundtrip(display);
	wl_display_roundtrip(display);

	if (!compositor || !wl_shm || !xdg_wm_base) {
		fprintf(stderr,
		    "%s: missing Wayland globals "
		    "(compositor=%p shm=%p xdg=%p)\n",
		    PROG, (void *)compositor, (void *)wl_shm,
		    (void *)xdg_wm_base);
		return 1;
	}

	win_w = INIT_W;
	win_h = INIT_H;

	surface = wl_compositor_create_surface(compositor);
	wl_surface_add_listener(surface, &surface_listener, NULL);
	xdg_surface = xdg_wm_base_get_xdg_surface(xdg_wm_base, surface);
	xdg_surface_add_listener(xdg_surface, &xdg_surface_listener, NULL);
	xdg_toplevel = xdg_surface_get_toplevel(xdg_surface);
	xdg_toplevel_add_listener(xdg_toplevel, &xdg_toplevel_listener, NULL);
	xdg_toplevel_set_title(xdg_toplevel, instance_title);
	xdg_toplevel_set_app_id(xdg_toplevel, PROG);

	/* No min/max -- cowpager reflows to whatever size the user picks. */
	wl_surface_commit(surface);

	pthread_t tid;
	pthread_create(&tid, NULL, status_thread, NULL);
	pthread_detach(tid);

	struct event_base *base = event_base_new();
	struct event	  *wl_ev = NULL, *wake_ev = NULL;
	if (base == NULL) {
		fprintf(stderr, "%s: failed to initialise event loop\n", PROG);
		return 1;
	}

	wl_ev = event_new(base, wl_display_get_fd(display), EV_READ | EV_PERSIST,
	    wayland_cb, NULL);
	wake_ev = event_new(base, wake_pipe[0], EV_READ | EV_PERSIST,
	    wake_cb, NULL);
	if (wl_ev == NULL || wake_ev == NULL ||
	    event_add(wl_ev, NULL) < 0 || event_add(wake_ev, NULL) < 0) {
		fprintf(stderr, "%s: failed to add events\n", PROG);
		opt_running = 0;
	}

	while (opt_running) {
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

	xdg_toplevel_destroy(xdg_toplevel);
	xdg_surface_destroy(xdg_surface);
	wl_surface_destroy(surface);
	cow_module_cursor_fini(&cursor);
	if (pointer != NULL)
		wl_pointer_destroy(pointer);
	if (seat != NULL)
		wl_seat_destroy(seat);
	wl_display_disconnect(display);
	cow_image_cache_clear(&icon_cache);
	pager_selections_free();

	return 0;
}
