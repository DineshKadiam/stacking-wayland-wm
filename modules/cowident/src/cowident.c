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
#include <cairo.h>
#include <fcntl.h>
#include <pango/pangocairo.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>
#include <wayland-cursor.h>
#include <xkbcommon/xkbcommon.h>

#include "cow/module.h"
#include "cow/module_cursor.h"
#include "cow/memory.h"
#include "cow/text.h"

#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "xdg-output-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

#define MAX_OUTPUTS 20
#define MAX_VIEWS 256
#define ID_LEN 64
#define MAX_VALUE_WIDTH 480

/*
 * One output the compositor advertises.  We track wl_output's logical
 * geometry from xdg_output, falling back to wl_output's own geometry
 * event when xdg_output is unavailable.  cowident's layer surface is
 * created per-output.
 */
struct out {
	struct wl_output	     *wl_output;
	struct zxdg_output_v1	     *xdg_output;
	uint32_t		      name;
	int			      x, y;   /* logical position */
	int			      width;  /* logical width */
	int			      height; /* logical height */
	int			      scale;
	struct zwlr_layer_surface_v1 *layer_surface;
	struct wl_surface	     *surface;
	int			      surf_w; /* configured */
	int			      surf_h;
	bool			      configured;
	bool			      have_geom;
};

struct view_info {
	char	 id[ID_LEN];
	char	*app_id;
	char	*title;
	char	*output;
	int	 desk;
	int	 x, y;
	int	 vx, vy;
	int	 width, height;
	bool	 focused;
	bool	 maximized;
	bool	 shaded;
	bool	 iconified;
	bool	 circulate_skip;
	bool	 use_ssd;
	int	 title_thickness;
	int	 border_width;
	uint64_t raise_seq;
	char	 squeeze_mode[16];
	char	 squeeze_justify[16];
	int	 frame_x, frame_y, frame_w, frame_h;
};

static struct wl_display		 *display;
static struct wl_compositor		 *compositor;
static struct wl_shm			 *shm;
static struct wl_seat			 *seat;
static struct wl_pointer		 *pointer;
static struct wl_keyboard		 *keyboard;
static struct zwlr_layer_shell_v1	 *layer_shell;
static struct xdg_wm_base		 *xdg_wm_base;
static struct zxdg_output_manager_v1	 *xdg_output_manager;
static struct cow_module_cursor		  cursor;

/*
 * wl_cursor fallback: used only when the compositor does not advertise
 * wp_cursor_shape_v1.  We attach the cursor image to a temporary
 * wl_surface and call wl_pointer.set_cursor on each pointer enter.
 */
static struct wl_cursor_theme *cursor_theme;
static struct wl_cursor	      *cursor_crosshair;
static struct wl_surface      *cursor_surface;
static int		       cursor_size = 24;

static struct out outs[MAX_OUTPUTS];
static int	  n_outs;

static struct view_info views[MAX_VIEWS];
static int		n_views;

/* xkb */
static struct xkb_context *xkb_ctx;
static struct xkb_keymap  *xkb_keymap;
static struct xkb_state	  *xkb_state;

/* Pointer state */
static struct out *ptr_output;
static double	   ptr_sx, ptr_sy; /* surface-local */

/* Phase progression */
enum phase {
	PHASE_TARGETING,
	PHASE_POPUP,
	PHASE_DONE,
};
static enum phase phase = PHASE_TARGETING;

/* Selected view (set when targeting click hits a view) */
static int selected_view = -1;

/* Popup state */
static struct wl_surface   *popup_surface;
static struct xdg_surface  *popup_xdg_surface;
static struct xdg_toplevel *popup_xdg_toplevel;
static int		    popup_w = 480;
static int		    popup_h = 360;
static bool		    popup_configured;

/* Configured font for popup */
static char cfg_font[256] = "monospace 10";

static char *
ipc_query(const char *cmd)
{
	return cow_module_cmd_request("cowident", cmd, true);
}

static bool
copy_jstr(char *dst, size_t cap, cJSON *obj, const char *key)
{
	if (!obj)
		return false;
	cJSON *v = cJSON_GetObjectItem(obj, key);
	if (!v || !cJSON_IsString(v))
		return false;
	strncpy(dst, v->valuestring, cap - 1);
	dst[cap - 1] = '\0';
	return true;
}

static char *
dup_jstr(cJSON *obj, const char *key)
{
	if (obj == NULL)
		return (NULL);
	cJSON *v = cJSON_GetObjectItem(obj, key);
	if (v == NULL || !cJSON_IsString(v) || v->valuestring == NULL)
		return (NULL);

	char *copy;
	xasprintf(&copy, "%s", v->valuestring);
	return (copy);
}

static void
free_views(void)
{
	for (int i = 0; i < n_views; i++) {
		xfree(views[i].app_id);
		xfree(views[i].title);
		xfree(views[i].output);
	}
	memset(views, 0, sizeof(views));
	n_views = 0;
}

static int
get_jnum(cJSON *obj, const char *key, int def)
{
	if (!obj)
		return def;
	cJSON *v = cJSON_GetObjectItem(obj, key);
	if (!v || !cJSON_IsNumber(v))
		return def;
	return (int)v->valuedouble;
}

static bool
get_jbool(cJSON *obj, const char *key)
{
	if (!obj)
		return false;
	cJSON *v = cJSON_GetObjectItem(obj, key);
	return v && cJSON_IsTrue(v);
}

/* Fetch and store cow's configured font name. */
static void
fetch_font(void)
{
	char *resp = ipc_query("show config.font");
	if (!resp)
		return;

	cJSON *root = cJSON_Parse(resp);
	free(resp);
	if (!root) {
		return;
	}
	cJSON *data = cJSON_GetObjectItem(root, "data");
	if (data && cJSON_IsString(data) && data->valuestring &&
	    data->valuestring[0]) {
		strncpy(cfg_font, data->valuestring, sizeof(cfg_font) - 1);
		cfg_font[sizeof(cfg_font) - 1] = '\0';
	}
	cJSON_Delete(root);
}

/* Fetch the list of windows and store in the global views array. */
static void
fetch_views(void)
{
	free_views();
	char *resp = ipc_query("show windows");
	if (!resp) {
		fprintf(stderr, "cowident: no response to 'show windows'\n");
		return;
	}

	cJSON *root = cJSON_Parse(resp);
	free(resp);
	if (!root) {
		fprintf(stderr, "cowident: malformed response\n");
		return;
	}
	cJSON *data = cJSON_GetObjectItem(root, "data");
	if (!data || !cJSON_IsArray(data)) {
		cJSON_Delete(root);
		return;
	}

	int n = cJSON_GetArraySize(data);
	if (n > MAX_VIEWS)
		n = MAX_VIEWS;

	for (int i = 0; i < n; i++) {
		cJSON *w = cJSON_GetArrayItem(data, i);
		if (!w)
			continue;

		struct view_info *v = &views[n_views];
		memset(v, 0, sizeof(*v));

		copy_jstr(v->id, sizeof(v->id), w, "id");
		v->app_id = dup_jstr(w, "app_id");
		v->title = dup_jstr(w, "title");
		v->output = dup_jstr(w, "output");
		v->desk		   = get_jnum(w, "desk", -1);
		v->x		   = get_jnum(w, "x", 0);
		v->y		   = get_jnum(w, "y", 0);
		v->vx		   = get_jnum(w, "vx", 0);
		v->vy		   = get_jnum(w, "vy", 0);
		v->width	   = get_jnum(w, "width", 0);
		v->height	   = get_jnum(w, "height", 0);
		v->focused	   = get_jbool(w, "focused");
		v->maximized	   = get_jbool(w, "maximized");
		v->shaded	   = get_jbool(w, "shaded");
		v->iconified	   = get_jbool(w, "iconified");
		v->circulate_skip = get_jbool(w, "circulate_skip");
		v->use_ssd	   = get_jbool(w, "use_ssd");
		v->title_thickness = get_jnum(w, "title_thickness", 0);
		v->border_width	   = get_jnum(w, "border_width", 0);
		v->raise_seq	   = (uint64_t)get_jnum(w, "raise_seq", 0);

		cJSON *sq = cJSON_GetObjectItem(w, "squeeze");
		copy_jstr(v->squeeze_mode, sizeof(v->squeeze_mode), sq, "mode");
		copy_jstr(v->squeeze_justify, sizeof(v->squeeze_justify), sq,
		    "justify");

		/*
		 * Frame box: include borders + titlebar.  Computed from
		 * x/y (content origin) - border - titlebar offsets.
		 */
		int bw	   = v->border_width;
		int tt	   = v->title_thickness;
		v->frame_x = v->x - bw;
		v->frame_y = v->y - bw - tt;
		v->frame_w = v->width + 2 * bw;
		v->frame_h = v->height + 2 * bw + tt;

		n_views++;
	}
	cJSON_Delete(root);
}

static struct wl_buffer *
shm_buffer(int w, int h, void **px)
{
	int   stride = w * 4;
	off_t size   = (off_t)stride * h;

    #if HAVE_MEMFD_CREATE
	int fd = memfd_create("cowident", MFD_CLOEXEC);
    #else
    /* POSIX fallback for systems without memfd_create. */
    char name[64];
    snprintf(name, sizeof(name), "/cowident-%d", getpid());
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
	struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
	struct wl_buffer *buf = wl_shm_pool_create_buffer(pool, 0, w, h, stride,
	    WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);
	return buf;
}

static void
draw_overlay(struct out *o)
{
	if (!o->configured)
		return;

	void		 *px;
	struct wl_buffer *buf = shm_buffer(o->surf_w, o->surf_h, &px);
	if (!buf)
		return;

	cairo_surface_t *cs = cairo_image_surface_create_for_data(px,
	    CAIRO_FORMAT_ARGB32, o->surf_w, o->surf_h, o->surf_w * 4);
	cairo_t		*cr = cairo_create(cs);

	/*
	 * Transparent so the desktop shows through.  We deliberately
	 * draw nothing else: the actual crosshair targeting glyph is
	 * the system pointer cursor (set to SHAPE_CROSSHAIR via
	 * wp_cursor_shape_v1, or the corresponding entry from the
	 * user's wl_cursor theme), which gives a conventional
	 * FvwmIdent-style pointer rather than full-screen guide lines.
	 */
	cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
	cairo_paint(cr);

	cairo_destroy(cr);
	cairo_surface_destroy(cs);

	wl_surface_attach(o->surface, buf, 0, 0);
	wl_surface_damage_buffer(o->surface, 0, 0, o->surf_w, o->surf_h);
	wl_surface_commit(o->surface);

	wl_buffer_destroy(buf);
	munmap(px, (size_t)o->surf_w * o->surf_h * 4);
}

static void
draw_overlay_all(void)
{
	for (int i = 0; i < n_outs; i++)
		draw_overlay(&outs[i]);
}

static int
hit_test(int gx, int gy)
{
	/*
	 * Return the topmost window at (gx, gy).  Topmost = the matching
	 * view with the highest raise_seq.  cow increments raise_seq
	 * every time a view is raised to top, so this correctly reflects
	 * stacking order regardless of the order 'show windows' returns
	 * views in.
	 */
	int	 best	 = -1;
	uint64_t best_rs = 0;
	for (int i = 0; i < n_views; i++) {
		struct view_info *v = &views[i];
		if (v->iconified || v->shaded)
			continue;
		if (gx < v->frame_x || gy < v->frame_y)
			continue;
		if (gx >= v->frame_x + v->frame_w ||
		    gy >= v->frame_y + v->frame_h)
			continue;
		if (best < 0 || v->raise_seq >= best_rs) {
			best	= i;
			best_rs = v->raise_seq;
		}
	}
	return best;
}

/* ----------------------------------------------------------------------
 * Layer surface event handlers
 * ---------------------------------------------------------------------- */

static void
ls_configure(void *data, struct zwlr_layer_surface_v1 *ls, uint32_t serial,
    uint32_t w, uint32_t h)
{
	struct out *o = data;
	o->surf_w     = (int)w;
	o->surf_h     = (int)h;
	o->configured = true;
	zwlr_layer_surface_v1_ack_configure(ls, serial);
	draw_overlay(o);
}

static void
ls_closed(void *data, struct zwlr_layer_surface_v1 *ls)
{
	(void)data;
	(void)ls;
	phase = PHASE_DONE;
}

static const struct zwlr_layer_surface_v1_listener ls_listener = {
	.configure = ls_configure,
	.closed	   = ls_closed,
};

/* ----------------------------------------------------------------------
 * Output event handlers (wl_output: capture geometry as fallback)
 * ---------------------------------------------------------------------- */

static void
out_geometry(void *data, struct wl_output *wl_output, int32_t x, int32_t y,
    int32_t pw, int32_t ph, int32_t subpixel, const char *make,
    const char *model, int32_t transform)
{
	struct out *o = data;
	(void)wl_output;
	(void)pw;
	(void)ph;
	(void)subpixel;
	(void)make;
	(void)model;
	(void)transform;
	o->x = x;
	o->y = y;
}

static void
out_mode(void *data, struct wl_output *wl_output, uint32_t flags, int32_t w,
    int32_t h, int32_t refresh)
{
	struct out *o = data;
	(void)wl_output;
	(void)refresh;
	if (flags & WL_OUTPUT_MODE_CURRENT) {
		/* mode reports physical pixels; layer-shell configure will
		 * eventually report logical -- store as fallback. */
		if (o->width == 0)
			o->width = w;
		if (o->height == 0)
			o->height = h;
	}
}

static void
out_done(void *data, struct wl_output *wl_output)
{
	struct out *o = data;
	(void)wl_output;
	o->have_geom = true;
}

static void
out_scale(void *data, struct wl_output *wl_output, int32_t scale)
{
	struct out *o = data;
	(void)wl_output;
	o->scale = scale;
}

static void
out_name(void *data, struct wl_output *wl_output, const char *name)
{
	(void)data;
	(void)wl_output;
	(void)name;
}

static void
out_description(void *data, struct wl_output *wl_output, const char *desc)
{
	(void)data;
	(void)wl_output;
	(void)desc;
}

static const struct wl_output_listener out_listener = {
	.geometry    = out_geometry,
	.mode	     = out_mode,
	.done	     = out_done,
	.scale	     = out_scale,
	.name	     = out_name,
	.description = out_description,
};

/* ----------------------------------------------------------------------
 * xdg_output event handlers: logical position (overrides wl_output)
 * ---------------------------------------------------------------------- */

static void
xdg_output_logical_position(void *data, struct zxdg_output_v1 *xdg_output,
    int32_t x, int32_t y)
{
	struct out *o = data;
	(void)xdg_output;
	o->x = x;
	o->y = y;
}

static void
xdg_output_logical_size(void *data, struct zxdg_output_v1 *xdg_output,
    int32_t w, int32_t h)
{
	struct out *o = data;
	(void)xdg_output;
	o->width  = w;
	o->height = h;
}

static void
xdg_output_done(void *data, struct zxdg_output_v1 *xdg_output)
{
	(void)data;
	(void)xdg_output;
}

static void
xdg_output_name(void *data, struct zxdg_output_v1 *xdg_output, const char *name)
{
	(void)data;
	(void)xdg_output;
	(void)name;
}

static void
xdg_output_description(void *data, struct zxdg_output_v1 *xdg_output,
    const char *desc)
{
	(void)data;
	(void)xdg_output;
	(void)desc;
}

static const struct zxdg_output_v1_listener xdg_output_listener = {
	.logical_position = xdg_output_logical_position,
	.logical_size	  = xdg_output_logical_size,
	.done		  = xdg_output_done,
	.name		  = xdg_output_name,
	.description	  = xdg_output_description,
};

/* ----------------------------------------------------------------------
 * Cursor: prefer wp_cursor_shape_v1, fall back to wl_cursor + wl_pointer
 * ---------------------------------------------------------------------- */

static void
cursor_init_fallback(void)
{
	if (cursor_theme)
		return;
	const char *theme  = getenv("XCURSOR_THEME");
	const char *size_s = getenv("XCURSOR_SIZE");
	if (size_s) {
		int s = atoi(size_s);
		if (s > 0)
			cursor_size = s;
	}
	cursor_theme = wl_cursor_theme_load(theme, cursor_size, shm);
	if (!cursor_theme)
		return;
	/* Try a list of names in order of preference. */
	static const char *names[] = { "crosshair", "cross", "tcross", NULL };
	for (int i = 0; names[i]; i++) {
		cursor_crosshair = wl_cursor_theme_get_cursor(cursor_theme,
		    names[i]);
		if (cursor_crosshair)
			break;
	}
	if (!cursor_crosshair)
		return;
	cursor_surface = wl_compositor_create_surface(compositor);
}

static void
cursor_apply(uint32_t serial)
{
	if (!pointer)
		return;
	if (cow_module_cursor_set_crosshair(&cursor, serial))
		return;
	if (!cursor_surface || !cursor_crosshair)
		return;
	struct wl_cursor_image *img = cursor_crosshair->images[0];
	struct wl_buffer       *buf = wl_cursor_image_get_buffer(img);
	if (!buf)
		return;
	wl_pointer_set_cursor(pointer, serial, cursor_surface, img->hotspot_x,
	    img->hotspot_y);
	wl_surface_attach(cursor_surface, buf, 0, 0);
	wl_surface_damage_buffer(cursor_surface, 0, 0, img->width, img->height);
	wl_surface_commit(cursor_surface);
}

/*
 * Release the crosshair cursor so the compositor reverts to its default.
 * Called when leaving PHASE_TARGETING (selection made or cancelled).
 */
static void
cursor_reset(void)
{
	if (!pointer)
		return;
	/*
	 * Setting a NULL surface tells the compositor we no longer wish
	 * to override the cursor; it will revert to whatever cursor the
	 * next focused surface sets (typically the default pointer).
	 */
	wl_pointer_set_cursor(pointer, 0, NULL, 0, 0);
}

/* ----------------------------------------------------------------------
 * Pointer event handlers
 * ---------------------------------------------------------------------- */

static struct out *
output_for_surface(struct wl_surface *s)
{
	for (int i = 0; i < n_outs; i++) {
		if (outs[i].surface == s)
			return &outs[i];
	}
	return NULL;
}

static void
on_target_click(uint32_t button)
{
	if (button == 0x110 /* BTN_LEFT */) {
		if (!ptr_output)
			return;
		int gx = ptr_output->x + (int)ptr_sx;
		int gy = ptr_output->y + (int)ptr_sy;
		int v  = hit_test(gx, gy);
		if (v < 0) {
			fprintf(stderr, "cowident: no window at (%d, %d)\n", gx,
			    gy);
			cursor_reset();
			phase = PHASE_DONE;
			return;
		}
		selected_view = v;
		cursor_reset();
		phase = PHASE_POPUP;
	} else {
		/* right-click or middle: cancel */
		cursor_reset();
		phase = PHASE_DONE;
	}
}

static void
ptr_enter(void *data, struct wl_pointer *p, uint32_t serial,
    struct wl_surface *surface, wl_fixed_t sx, wl_fixed_t sy)
{
	(void)data;
	(void)p;
	ptr_output = output_for_surface(surface);
	ptr_sx	   = wl_fixed_to_double(sx);
	ptr_sy	   = wl_fixed_to_double(sy);
	/*
	 * Override the default cursor with a crosshair only while the
	 * pointer is over our overlay surface.  Outside (pointer over
	 * the popup or other surfaces) we leave it alone so xdg/SSD
	 * cursors render normally.
	 */
	if (ptr_output != NULL && phase == PHASE_TARGETING)
		cursor_apply(serial);
	else
		cow_module_cursor_set_default(&cursor, serial);
	if (phase == PHASE_TARGETING)
		draw_overlay_all();
}

static void
ptr_leave(void *data, struct wl_pointer *p, uint32_t serial,
    struct wl_surface *surface)
{
	(void)data;
	(void)p;
	(void)serial;
	(void)surface;
	ptr_output = NULL;
	if (phase == PHASE_TARGETING)
		draw_overlay_all();
}

static void
ptr_motion(void *data, struct wl_pointer *p, uint32_t time, wl_fixed_t sx,
    wl_fixed_t sy)
{
	(void)data;
	(void)p;
	(void)time;
	ptr_sx = wl_fixed_to_double(sx);
	ptr_sy = wl_fixed_to_double(sy);
	if (phase == PHASE_TARGETING)
		draw_overlay_all();
}

static void
ptr_button(void *data, struct wl_pointer *p, uint32_t serial, uint32_t time,
    uint32_t button, uint32_t state)
{
	(void)data;
	(void)p;
	(void)serial;
	(void)time;
	if (state != WL_POINTER_BUTTON_STATE_PRESSED)
		return;
	if (phase == PHASE_TARGETING)
		on_target_click(button);
	else if (phase == PHASE_POPUP)
		phase = PHASE_DONE;
}

static void
ptr_axis(void *data, struct wl_pointer *p, uint32_t time, uint32_t axis,
    wl_fixed_t value)
{
	(void)data;
	(void)p;
	(void)time;
	(void)axis;
	(void)value;
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
static void
ptr_axis_value120(void *d, struct wl_pointer *p, uint32_t a, int32_t v)
{
	(void)d;
	(void)p;
	(void)a;
	(void)v;
}
static void
ptr_axis_relative_direction(void *d, struct wl_pointer *p, uint32_t a,
    uint32_t r)
{
	(void)d;
	(void)p;
	(void)a;
	(void)r;
}

static const struct wl_pointer_listener ptr_listener = {
	.enter			 = ptr_enter,
	.leave			 = ptr_leave,
	.motion			 = ptr_motion,
	.button			 = ptr_button,
	.axis			 = ptr_axis,
	.frame			 = ptr_frame,
	.axis_source		 = ptr_axis_source,
	.axis_stop		 = ptr_axis_stop,
	.axis_discrete		 = ptr_axis_discrete,
	.axis_value120		 = ptr_axis_value120,
	.axis_relative_direction = ptr_axis_relative_direction,
};

/* ----------------------------------------------------------------------
 * Keyboard event handlers (only Escape interests us)
 * ---------------------------------------------------------------------- */

static void
kb_keymap(void *data, struct wl_keyboard *kb, uint32_t format, int fd,
    uint32_t size)
{
	(void)data;
	(void)kb;
	if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
		close(fd);
		return;
	}
	char *map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
	close(fd);
	if (map == MAP_FAILED)
		return;
	if (xkb_keymap)
		xkb_keymap_unref(xkb_keymap);
	if (xkb_state)
		xkb_state_unref(xkb_state);
	xkb_keymap = xkb_keymap_new_from_string(xkb_ctx, map,
	    XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
	munmap(map, size);
	if (xkb_keymap)
		xkb_state = xkb_state_new(xkb_keymap);
}

static void
kb_enter(void *d, struct wl_keyboard *k, uint32_t s, struct wl_surface *surf,
    struct wl_array *keys)
{
	(void)d;
	(void)k;
	(void)s;
	(void)surf;
	(void)keys;
}

static void
kb_leave(void *d, struct wl_keyboard *k, uint32_t s, struct wl_surface *surf)
{
	(void)d;
	(void)k;
	(void)s;
	(void)surf;
}

static void
kb_key(void *data, struct wl_keyboard *kb, uint32_t serial, uint32_t time,
    uint32_t key, uint32_t state)
{
	(void)data;
	(void)kb;
	(void)serial;
	(void)time;
	if (state != WL_KEYBOARD_KEY_STATE_PRESSED)
		return;
	if (!xkb_state)
		return;
	xkb_keysym_t sym = xkb_state_key_get_one_sym(xkb_state, key + 8);
	if (sym == XKB_KEY_Escape) {
		if (phase == PHASE_TARGETING)
			cursor_reset();
		phase = PHASE_DONE;
	}
}

static void
kb_modifiers(void *data, struct wl_keyboard *kb, uint32_t serial,
    uint32_t mods_depressed, uint32_t mods_latched, uint32_t mods_locked,
    uint32_t group)
{
	(void)data;
	(void)kb;
	(void)serial;
	if (xkb_state)
		xkb_state_update_mask(xkb_state, mods_depressed, mods_latched,
		    mods_locked, 0, 0, group);
}

static void
kb_repeat_info(void *d, struct wl_keyboard *k, int32_t r, int32_t delay)
{
	(void)d;
	(void)k;
	(void)r;
	(void)delay;
}

static const struct wl_keyboard_listener kb_listener = {
	.keymap	     = kb_keymap,
	.enter	     = kb_enter,
	.leave	     = kb_leave,
	.key	     = kb_key,
	.modifiers   = kb_modifiers,
	.repeat_info = kb_repeat_info,
};

/* ----------------------------------------------------------------------
 * Seat capabilities
 * ---------------------------------------------------------------------- */

static void
seat_caps(void *data, struct wl_seat *s, uint32_t caps)
{
	(void)data;
	if ((caps & WL_SEAT_CAPABILITY_POINTER) && !pointer) {
		pointer = wl_seat_get_pointer(s);
		wl_pointer_add_listener(pointer, &ptr_listener, NULL);
		cow_module_cursor_set_pointer(&cursor, pointer);
	} else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && pointer) {
		cow_module_cursor_clear_pointer(&cursor, pointer);
		wl_pointer_destroy(pointer);
		pointer = NULL;
	}
	if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !keyboard) {
		keyboard = wl_seat_get_keyboard(s);
		wl_keyboard_add_listener(keyboard, &kb_listener, NULL);
	}
}

static void
seat_name(void *data, struct wl_seat *s, const char *name)
{
	(void)data;
	(void)s;
	(void)name;
}

static const struct wl_seat_listener seat_listener = {
	.capabilities = seat_caps,
	.name	      = seat_name,
};

/* ----------------------------------------------------------------------
 * xdg_wm_base
 * ---------------------------------------------------------------------- */

static void
xdg_wm_base_ping(void *data, struct xdg_wm_base *base, uint32_t serial)
{
	(void)data;
	xdg_wm_base_pong(base, serial);
}

static const struct xdg_wm_base_listener xdg_wm_base_listener = {
	.ping = xdg_wm_base_ping,
};

/* ----------------------------------------------------------------------
 * Registry
 * ---------------------------------------------------------------------- */

static void
reg_global(void *data, struct wl_registry *reg, uint32_t name,
    const char *iface, uint32_t version)
{
	(void)data;
	if (strcmp(iface, wl_compositor_interface.name) == 0) {
		compositor = wl_registry_bind(reg, name,
		    &wl_compositor_interface, version < 4 ? version : 4);
	} else if (strcmp(iface, wl_shm_interface.name) == 0) {
		shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
	} else if (strcmp(iface, wl_seat_interface.name) == 0) {
		seat = wl_registry_bind(reg, name, &wl_seat_interface,
		    version < 5 ? version : 5);
		wl_seat_add_listener(seat, &seat_listener, NULL);
	} else if (strcmp(iface, wl_output_interface.name) == 0) {
		if (n_outs >= MAX_OUTPUTS)
			return;
		struct out *o = &outs[n_outs++];
		memset(o, 0, sizeof(*o));
		o->name	     = name;
		o->scale     = 1;
		o->wl_output = wl_registry_bind(reg, name, &wl_output_interface,
		    version < 4 ? version : 4);
		wl_output_add_listener(o->wl_output, &out_listener, o);
	} else if (strcmp(iface, zwlr_layer_shell_v1_interface.name) == 0) {
		layer_shell = wl_registry_bind(reg, name,
		    &zwlr_layer_shell_v1_interface, version < 4 ? version : 4);
	} else if (strcmp(iface, xdg_wm_base_interface.name) == 0) {
		xdg_wm_base = wl_registry_bind(reg, name,
		    &xdg_wm_base_interface, 1);
		xdg_wm_base_add_listener(xdg_wm_base, &xdg_wm_base_listener,
		    NULL);
	} else if (strcmp(iface, zxdg_output_manager_v1_interface.name) == 0) {
		xdg_output_manager = wl_registry_bind(reg, name,
		    &zxdg_output_manager_v1_interface,
		    version < 3 ? version : 3);
	} else
		cow_module_cursor_handle_global(&cursor, reg, name, iface,
		    version);
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

/* ----------------------------------------------------------------------
 * Phase 1: create one layer surface per output to capture clicks
 * ---------------------------------------------------------------------- */

static void
overlay_create(void)
{
	for (int i = 0; i < n_outs; i++) {
		struct out *o	 = &outs[i];
		o->surface	 = wl_compositor_create_surface(compositor);
		o->layer_surface = zwlr_layer_shell_v1_get_layer_surface(
		    layer_shell, o->surface, o->wl_output,
		    ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "cowident");
		zwlr_layer_surface_v1_set_anchor(o->layer_surface,
		    ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
			ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
			ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
			ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
		zwlr_layer_surface_v1_set_keyboard_interactivity(
		    o->layer_surface,
		    ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE);
		zwlr_layer_surface_v1_set_exclusive_zone(o->layer_surface, -1);
		zwlr_layer_surface_v1_add_listener(o->layer_surface,
		    &ls_listener, o);
		wl_surface_commit(o->surface);
	}
}

static void
overlay_destroy(void)
{
	/*
	 * Same tear-down dance as popup_destroy(): unmap each
	 * overlay surface (NULL buffer + commit), roundtrip so the
	 * server processes the unmaps, destroy the role objects,
	 * then roundtrip again so the compositor's state machine
	 * fully unwinds before we move on.
	 */
	for (int i = 0; i < n_outs; i++) {
		struct out *o = &outs[i];
		if (o->surface) {
			wl_surface_attach(o->surface, NULL, 0, 0);
			wl_surface_commit(o->surface);
		}
	}
	wl_display_roundtrip(display);
	for (int i = 0; i < n_outs; i++) {
		struct out *o = &outs[i];
		if (o->layer_surface) {
			zwlr_layer_surface_v1_destroy(o->layer_surface);
			o->layer_surface = NULL;
		}
		if (o->surface) {
			wl_surface_destroy(o->surface);
			o->surface = NULL;
		}
		o->configured = false;
	}
	wl_display_roundtrip(display);
}

/* ----------------------------------------------------------------------
 * Phase 2: popup window with FvwmIdent-style layout
 * ---------------------------------------------------------------------- */

struct row {
	const char *label;
	char	   *value;
};

#define MAX_ROWS 32

static struct row popup_rows[MAX_ROWS];
static int	  n_rows;

static void
add_row(const char *label, char *value)
{
	if (n_rows >= MAX_ROWS) {
		xfree(value);
		return;
	}
	struct row *r = &popup_rows[n_rows++];
	r->label      = label;
	r->value      = value;
}

#define ADD_ROW(label, ...)                        \
	do {                                         \
		char *value;                           \
		xasprintf(&value, __VA_ARGS__);         \
		add_row((label), value);                \
	} while (0)

static void
free_popup_rows(void)
{
	for (int i = 0; i < n_rows; i++) {
		xfree(popup_rows[i].value);
		popup_rows[i].value = NULL;
	}
	n_rows = 0;
}

static void
append_state(char *buf, size_t len, bool active, const char *name)
{
	if (!active)
		return;
	if (buf[0] != '\0')
		strlcat(buf, " ", len);
	strlcat(buf, name, len);
}

static void
build_popup_rows(void)
{
	struct view_info *v = &views[selected_view];
	free_popup_rows();
	char state[128]	    = "";

	ADD_ROW("Window ID:", "%s", v->id);
	ADD_ROW("App ID:", "%s", v->app_id != NULL && v->app_id[0] != '\0' ?
	    v->app_id : "(none)");
	ADD_ROW("Title:", "%s", v->title != NULL && v->title[0] != '\0' ?
	    v->title : "(none)");
	ADD_ROW("Output:", "%s", v->output != NULL && v->output[0] != '\0' ?
	    v->output : "(unknown)");
	ADD_ROW("Desk:", "%d", v->desk);
	ADD_ROW("Position:", "%d,%d  (virtual %d,%d)", v->x, v->y, v->vx,
	    v->vy);
	ADD_ROW("Size:", "%d x %d", v->width, v->height);
	ADD_ROW("Frame:", "%d,%d  %d x %d", v->frame_x, v->frame_y, v->frame_w,
	    v->frame_h);
	append_state(state, sizeof(state), v->focused, "focused");
	append_state(state, sizeof(state), v->maximized, "maximized");
	append_state(state, sizeof(state), v->shaded, "shaded");
	append_state(state, sizeof(state), v->iconified, "iconified");
	append_state(state, sizeof(state), v->circulate_skip,
	    "circulate-skip");
	ADD_ROW("State:", "%s", state);
	if (v->use_ssd)
		ADD_ROW("Decoration:", "SSD (border %d, titlebar %d)",
		    v->border_width, v->title_thickness);
	else
		ADD_ROW("Decoration:", "CSD");
	if (v->squeeze_mode[0] && strcmp(v->squeeze_mode, "none") != 0)
		ADD_ROW("Squeeze:", "%s, %s", v->squeeze_mode,
		    v->squeeze_justify);
}

#undef ADD_ROW

/*
 * Compute popup size from rendered text extents and lay out columns.
 * Returns the column-divider x in *col_x.
 */
static void
measure_popup(cairo_t *cr, PangoFontDescription *font, int *out_w,
    int *out_h, int *col_x)
{
	int max_label = 0, max_value = 0, line_h = 0;
	for (int i = 0; i < n_rows; i++) {
		int w, h;
		cow_text_measure(cr, font, popup_rows[i].label, &w, &h);
		if (w > max_label)
			max_label = w;
		if (h > line_h)
			line_h = h;

		cow_text_measure(cr, font, popup_rows[i].value, &w, &h);
		if (w > max_value)
			max_value = w;
		if (h > line_h)
			line_h = h;
	}

	const int margin = 12;
	const int gutter = 12;
	if (max_value > MAX_VALUE_WIDTH)
		max_value = MAX_VALUE_WIDTH;
	*col_x		 = margin + max_label;
	*out_w		 = margin + max_label + gutter + max_value + margin;
	*out_h		 = margin + n_rows * (line_h + 2) + margin;
	if (*out_w < 280)
		*out_w = 280;
}

static void
draw_popup(void)
{
	void		 *px;
	struct wl_buffer *buf = shm_buffer(popup_w, popup_h, &px);
	if (!buf)
		return;

	cairo_surface_t *cs = cairo_image_surface_create_for_data(px,
	    CAIRO_FORMAT_ARGB32, popup_w, popup_h, popup_w * 4);
	cairo_t		*cr = cairo_create(cs);

	/* Background. */
	cairo_set_source_rgba(cr, 0.96, 0.96, 0.94, 1.0);
	cairo_paint(cr);
	cairo_set_source_rgba(cr, 0.20, 0.20, 0.20, 1.0);
	cairo_set_line_width(cr, 1.0);
	cairo_rectangle(cr, 0.5, 0.5, popup_w - 1, popup_h - 1);
	cairo_stroke(cr);

	/* Pango font using cow's font setting. */
	PangoFontDescription *desc   = pango_font_description_from_string(
	      cfg_font);
	if (!desc)
		desc = pango_font_description_from_string("monospace 10");

	int col_x, want_w, want_h;
	measure_popup(cr, desc, &want_w, &want_h, &col_x);

	/* Draw rows.  Labels are right-aligned in the left column,
	 * values are left-aligned at col_x + gutter. */
	const int margin   = 12;
	const int gutter   = 12;
	int	  text_h = 0;
	int	  text_y = margin;

	cairo_set_source_rgba(cr, 0.10, 0.10, 0.10, 1.0);
	for (int i = 0; i < n_rows; i++) {
		int lh, vh;
		cow_text_measure(cr, desc, popup_rows[i].label, NULL, &lh);
		cow_text_measure(cr, desc, popup_rows[i].value, NULL, &vh);
		text_h = lh > vh ? lh : vh;
		cow_text_render(cr, desc, margin, text_y, col_x - margin,
		    text_h, popup_rows[i].label, PANGO_ALIGN_RIGHT, NULL);
		cow_text_render(cr, desc, col_x + gutter, text_y,
		    popup_w - col_x - gutter - margin, text_h,
		    popup_rows[i].value, PANGO_ALIGN_LEFT, NULL);
		text_y += text_h + 2;
	}

	pango_font_description_free(desc);

	cairo_destroy(cr);
	cairo_surface_destroy(cs);

	wl_surface_attach(popup_surface, buf, 0, 0);
	wl_surface_damage_buffer(popup_surface, 0, 0, popup_w, popup_h);
	wl_surface_commit(popup_surface);

	wl_buffer_destroy(buf);
	munmap(px, (size_t)popup_w * popup_h * 4);

	(void)want_w;
	(void)want_h;
}

/*
 * Compute the popup's preferred size by rendering once into a
 * scratch context.  Sets popup_w/popup_h.
 */
static void
size_popup(void)
{
	cairo_surface_t *cs = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1,
	    1);
	cairo_t		*cr = cairo_create(cs);
	PangoFontDescription *desc = pango_font_description_from_string(
	    cfg_font);
	if (!desc)
		desc = pango_font_description_from_string("monospace 10");

	int col_x;
	measure_popup(cr, desc, &popup_w, &popup_h, &col_x);

	pango_font_description_free(desc);
	cairo_destroy(cr);
	cairo_surface_destroy(cs);
	(void)col_x;
}

static void
popup_xdg_surface_configure(void *data, struct xdg_surface *xs, uint32_t serial)
{
	(void)data;
	xdg_surface_ack_configure(xs, serial);
	popup_configured = true;
	draw_popup();
}

static const struct xdg_surface_listener popup_xs_listener = {
	.configure = popup_xdg_surface_configure,
};

static void
popup_toplevel_configure(void *d, struct xdg_toplevel *t, int32_t w, int32_t h,
    struct wl_array *states)
{
	(void)d;
	(void)t;
	(void)states;
	if (w > 0)
		popup_w = w;
	if (h > 0)
		popup_h = h;
}

static void
popup_toplevel_close(void *d, struct xdg_toplevel *t)
{
	(void)d;
	(void)t;
	phase = PHASE_DONE;
}

static void
popup_toplevel_configure_bounds(void *d, struct xdg_toplevel *t, int32_t w,
    int32_t h)
{
	(void)d;
	(void)t;
	(void)w;
	(void)h;
}

static void
popup_toplevel_wm_capabilities(void *d, struct xdg_toplevel *t,
    struct wl_array *caps)
{
	(void)d;
	(void)t;
	(void)caps;
}

static const struct xdg_toplevel_listener popup_tl_listener = {
	.configure	  = popup_toplevel_configure,
	.close		  = popup_toplevel_close,
	.configure_bounds = popup_toplevel_configure_bounds,
	.wm_capabilities  = popup_toplevel_wm_capabilities,
};

static void
popup_create(void)
{
	build_popup_rows();
	size_popup();

	popup_surface	  = wl_compositor_create_surface(compositor);
	popup_xdg_surface = xdg_wm_base_get_xdg_surface(xdg_wm_base,
	    popup_surface);
	xdg_surface_add_listener(popup_xdg_surface, &popup_xs_listener, NULL);
	popup_xdg_toplevel = xdg_surface_get_toplevel(popup_xdg_surface);
	xdg_toplevel_add_listener(popup_xdg_toplevel, &popup_tl_listener, NULL);

	xdg_toplevel_set_title(popup_xdg_toplevel, "cowident");
	xdg_toplevel_set_app_id(popup_xdg_toplevel, "cowident");

	wl_surface_commit(popup_surface);
}

static void
popup_destroy(void)
{
	/*
	 * Tear-down sequence for the popup xdg_toplevel.
	 *
	 *   1. Unmap the wl_surface (NULL buffer + commit).
	 *   2. Roundtrip so the server processes the unmap.
	 *   3. Destroy xdg_toplevel, xdg_surface, wl_surface.
	 *   4. Final roundtrip for server-side cleanup.
	 */
	if (popup_surface) {
		wl_surface_attach(popup_surface, NULL, 0, 0);
		wl_surface_commit(popup_surface);
		wl_display_roundtrip(display);
	}
	if (popup_xdg_toplevel) {
		xdg_toplevel_destroy(popup_xdg_toplevel);
		popup_xdg_toplevel = NULL;
	}
	if (popup_xdg_surface) {
		xdg_surface_destroy(popup_xdg_surface);
		popup_xdg_surface = NULL;
	}
	if (popup_surface) {
		wl_surface_destroy(popup_surface);
		popup_surface = NULL;
	}
	wl_display_roundtrip(display);
}

int
main(void)
{
	display = wl_display_connect(NULL);
	if (!display) {
		fprintf(stderr, "cowident: cannot connect to display\n");
		return 1;
	}

	xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	if (!xkb_ctx) {
		fprintf(stderr, "cowident: xkb_context_new failed\n");
		return 1;
	}

	struct wl_registry *registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &reg_listener, NULL);
	wl_display_roundtrip(display);

	if (!compositor || !shm || !seat || !layer_shell || !xdg_wm_base) {
		fprintf(stderr,
		    "cowident: compositor missing required globals\n");
		return 1;
	}

	/*
	 * Bind xdg_output for each wl_output to get reliable logical
	 * positions (wlroots sends 0,0 via wl_output.geometry).
	 */
	if (xdg_output_manager) {
		for (int i = 0; i < n_outs; i++) {
			struct out *o = &outs[i];
			o->xdg_output = zxdg_output_manager_v1_get_xdg_output(
			    xdg_output_manager, o->wl_output);
			zxdg_output_v1_add_listener(o->xdg_output,
			    &xdg_output_listener, o);
		}
	}

	/*
	 * Set up the themed-cursor fallback only when the compositor does not
	 * advertise wp_cursor_shape_v1.
	 */
	if (!cow_module_cursor_available(&cursor))
		cursor_init_fallback();

	/* Fetch font and views before drawing anything. */
	fetch_font();
	fetch_views();

	/* Wait for output geometry / xdg_output events. */
	wl_display_roundtrip(display);

	/* Phase 1: targeting overlay */
	overlay_create();
	while (phase == PHASE_TARGETING) {
		if (wl_display_dispatch(display) < 0)
			break;
	}
	overlay_destroy();
	wl_display_flush(display);

	if (phase == PHASE_DONE) {
		/* Cancelled. */
		free_popup_rows();
		free_views();
		cow_module_cursor_fini(&cursor);
		wl_display_disconnect(display);
		return 0;
	}

	/* Phase 2: popup */
	popup_create();
	while (phase == PHASE_POPUP) {
		if (wl_display_dispatch(display) < 0)
			break;
	}
	popup_destroy();
	wl_display_flush(display);

	free_popup_rows();
	free_views();
	cow_module_cursor_fini(&cursor);
	wl_display_disconnect(display);
	return 0;
}
