/*
 * Copyright (c) 2026 Thomas Adam
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH
 * REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT,
 * INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
 * LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR
 * OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
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
#include <unistd.h>
#include <event2/event.h>
#include <wayland-client.h>

#include "cow/compat.h"
#include "cow/colour.h"
#include "cow/image_cache.h"
#include "cow/json_util.h"
#include "cow/module.h"
#include "cow/module_config.h"
#include "cow/module_cursor.h"
#include "cow/module_draw.h"
#include "cow/module_theme.h"
#include "cow/output_selector.h"
#include "cow/module_shm.h"
#include "cow/text.h"
#include "cow/util.h"
#include "xdg-shell-client-protocol.h"

#define PROG "cowiconman"
#define SHOW_INDEX_ENV "COWICONMAN_SHOW_INDEX"

#define MAX_WINS 512
#define MAX_OUTS 32
#define MAX_ROWS 512
#define MAX_NAME 128
#define MAX_MATCHES 16
#define MAX_FIELDS 16
#define MAX_SELECTED_ICONMANS 32
#define MAX_ICON_PATH 1024
#define DEFAULT_BUTTON_W 300
#define DEFAULT_MIN_BUTTON_W 80
#define DEFAULT_ICON_SIZE 16
#define ROW_H 24
#define PAD 4
#define TEXT_PAD 8
#define MARKER_SIZE 10
#define MARKER_GAP 4
#define BTN_LEFT 0x110
#define BTN_MIDDLE 0x112

enum filter_value {
	FILTER_ANY,
	FILTER_CURRENT,
	FILTER_EXPLICIT,
	FILTER_SELF,
};

enum sort_mode {
	SORT_TITLE,
	SORT_APP,
	SORT_ID,
	SORT_NONE,
};

enum anchor_mode {
	ANCHOR_NONE,
	ANCHOR_TOP,
	ANCHOR_BOTTOM,
};

enum group_mode {
	GROUP_NONE,
	GROUP_APP,
	GROUP_TITLE,
	GROUP_OUTPUT,
	GROUP_DESK,
};

enum button_relief_mode {
	BUTTON_RELIEF_AUTO,
	BUTTON_RELIEF_RAISED,
	BUTTON_RELIEF_SUNKEN,
	BUTTON_RELIEF_FLAT,
};

enum match_field {
	MATCH_TITLE,
	MATCH_APP,
	MATCH_ID,
};

enum palette_slot {
	PALETTE_BACKGROUND,
	PALETTE_ACTIVE_BG,
	PALETTE_ACTIVE_FG,
	PALETTE_INACTIVE_BG,
	PALETTE_INACTIVE_FG,
	PALETTE_ICONIFIED_BG,
	PALETTE_BORDER,
	PALETTE_COUNT,
};

struct match_rule {
	enum match_field field;
	int		 negate;
	char		 value[MAX_NAME];
};

struct iconman_selection {
	char *name;
	bool  seen;
};

struct iconman_load_ctx {
	int depth;
	bool in_selected_iconman;
};

struct output {
	char  name[MAX_NAME];
	int   number;
	int   x, y, w, h;
	int   usable_x, usable_y, usable_w, usable_h;
	int   current;
	int   current_desk;
	int   page_vx, page_vy;
	int   output_w, output_h;
	bool  has_background;
	float background[4];
};

struct win {
	char id[MAX_NAME];
	char app_id[MAX_NAME];
	char title[256];
	char output[MAX_NAME];
	char icon[MAX_ICON_PATH];
	int  desk;
	int  x, y;
	int  vx, vy, w, h;
	int  focused;
	int  iconified;
};

struct state {
	struct output outs[MAX_OUTS];
	int	      n_outs;
	struct win    wins[MAX_WINS];
	int	      n_wins;
	struct cow_module_theme theme;
	struct cow_module_theme titlebar_theme;
};

struct palette {
	float background[4];
	float active_bg[4];
	float inactive_bg[4];
	float active_fg[4];
	float inactive_fg[4];
	float iconified_bg[4];
	float border[4];
	const char *font;
	int height;
};

static const float default_background[4] = { 0.10, 0.10, 0.11, 1.0 };
static const float default_active_bg[4] = { 0.25, 0.38, 0.64, 1.0 };
static const float default_inactive_bg[4] = { 0.28, 0.28, 0.31, 1.0 };
static const float default_active_fg[4] = { 0.94, 0.94, 0.96, 1.0 };
static const float default_inactive_fg[4] = { 0.94, 0.94, 0.96, 1.0 };

struct row {
	struct win win;
	char	   label[384];
	int	   first;
	int	   count;
	int	   focused;
	int	   any_iconified;
	int	   all_iconified;
	int	   x;
	int	   y;
	int	   w;
	int	   h;
};

struct tracked_output {
	struct wl_output *wl_output;
	uint32_t	  global_name;
	char		  name[MAX_NAME];
	int		  on_surface;
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
static struct cow_module_shm_buffer buffers[2];

static struct tracked_output tracked[MAX_OUTS];
static int		     n_tracked;

static int    running = 1;
static int    configured;
static int    win_w, win_h;
static double ptr_x, ptr_y;
static int    layout_cols = 1;
static int    layout_rows = 1;
static int    wake_pipe[2];

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static struct state    global_state;
static int	       dirty;

static struct row rows[MAX_ROWS];
static struct win group_wins[MAX_WINS];
static int	  n_rows;
static int	  n_group_wins;
static int	  scroll_first;
static int	  visible_capacity;
static int	  total_rows;

static enum filter_value output_filter = FILTER_ANY;
static enum filter_value desk_filter = FILTER_ANY;
static enum filter_value page_filter = FILTER_ANY;
static char		 output_arg[MAX_NAME];
static int		 desk_arg;
static int		 page_x_arg, page_y_arg;
static enum sort_mode	 sort_mode = SORT_TITLE;
static enum anchor_mode	 anchor_mode = ANCHOR_NONE;
static enum group_mode	 group_mode = GROUP_NONE;
static int		 show_self;
static char		 label_format[128] = "%t";
static int		 opt_rows;
static int		 opt_cols;
static int		 button_w = DEFAULT_BUTTON_W;
static int		 min_button_w = DEFAULT_MIN_BUTTON_W;
static int		 frame_padding = PAD;
static int		 button_h;
static int		 iconified_marker;
static int		 show_icons;
static int		 icon_size = DEFAULT_ICON_SIZE;
static enum button_relief_mode button_relief = BUTTON_RELIEF_AUTO;
static float		 palette_overrides[PALETTE_COUNT][4];
static bool		 palette_override_set[PALETTE_COUNT];
static struct cow_image_cache icon_cache;
static char		 self_title[MAX_NAME];
static char		 self_id[MAX_NAME];
static struct match_rule matches[MAX_MATCHES];
static int		 n_matches;
static struct iconman_selection selected_iconmans[MAX_SELECTED_ICONMANS];
static int n_selected_iconmans;
static struct cow_module_config_instances config_instances;

static void do_render(const struct state *);
static void ipc_send(const char *);
static void format_label(char *, size_t, const struct win *);
static const char *group_key(const struct win *, char *, size_t);

static void
usage(void)
{
	fprintf(stderr, "usage: cowiconman [-c path] [-h]\n");
}

static int
get_jnum(cJSON *obj, const char *key)
{
	long long value = 0;

	(void)cow_json_get_int(obj, key, &value);
	return (int)value;
}

static int
get_jbool(cJSON *obj, const char *key)
{
	bool value = false;

	(void)cow_json_get_bool(obj, key, &value);
	return value;
}

static int
parse_int(const char *s, int *out)
{
	return cow_module_config_parse_int(s, out);
}

static int
parse_page(const char *s)
{
	char *comma, *dup;
	int   x, y;

	if (strcmp(s, "all") == 0) {
		page_filter = FILTER_ANY;
		return 1;
	}
	if (strcmp(s, "current") == 0) {
		page_filter = FILTER_CURRENT;
		return 1;
	}

	dup = strdup(s);
	if (dup == NULL)
		return 0;
	comma = strchr(dup, ',');
	if (comma == NULL) {
		free(dup);
		return 0;
	}
	*comma++ = '\0';
	if (!parse_int(dup, &x) || !parse_int(comma, &y)) {
		free(dup);
		return 0;
	}
	free(dup);
	page_x_arg = x;
	page_y_arg = y;
	page_filter = FILTER_EXPLICIT;
	return 1;
}

static void
parse_output_filter(const char *s)
{
	if (strcmp(s, "all") == 0) {
		output_filter = FILTER_ANY;
		output_arg[0] = '\0';
	} else if (strcmp(s, "current") == 0)
		output_filter = FILTER_CURRENT;
	else if (strcmp(s, "self") == 0)
		output_filter = FILTER_SELF;
	else {
		output_filter = FILTER_EXPLICIT;
		strlcpy(output_arg, s, sizeof(output_arg));
	}
}

static int
parse_desk_filter(const char *s)
{
	if (strcmp(s, "all") == 0) {
		desk_filter = FILTER_ANY;
		return 1;
	}
	if (strcmp(s, "current") == 0) {
		desk_filter = FILTER_CURRENT;
		return 1;
	}
	if (!parse_int(s, &desk_arg))
		return 0;
	desk_filter = FILTER_EXPLICIT;
	return 1;
}

static int
parse_sort(const char *s)
{
	if (strcmp(s, "title") == 0)
		sort_mode = SORT_TITLE;
	else if (strcmp(s, "app") == 0)
		sort_mode = SORT_APP;
	else if (strcmp(s, "id") == 0)
		sort_mode = SORT_ID;
	else if (strcmp(s, "none") == 0)
		sort_mode = SORT_NONE;
	else
		return 0;
	return 1;
}

static int
parse_anchor(const char *s)
{
	if (strcmp(s, "none") == 0)
		anchor_mode = ANCHOR_NONE;
	else if (strcmp(s, "top") == 0)
		anchor_mode = ANCHOR_TOP;
	else if (strcmp(s, "bottom") == 0)
		anchor_mode = ANCHOR_BOTTOM;
	else
		return 0;
	return 1;
}

static int
parse_button_relief(const char *s)
{
	if (strcmp(s, "auto") == 0)
		button_relief = BUTTON_RELIEF_AUTO;
	else if (strcmp(s, "raised") == 0)
		button_relief = BUTTON_RELIEF_RAISED;
	else if (strcmp(s, "sunken") == 0)
		button_relief = BUTTON_RELIEF_SUNKEN;
	else if (strcmp(s, "flat") == 0)
		button_relief = BUTTON_RELIEF_FLAT;
	else
		return 0;
	return 1;
}

static int
parse_group(const char *s)
{
	if (strcmp(s, "none") == 0)
		group_mode = GROUP_NONE;
	else if (strcmp(s, "app") == 0)
		group_mode = GROUP_APP;
	else if (strcmp(s, "title") == 0)
		group_mode = GROUP_TITLE;
	else if (strcmp(s, "output") == 0)
		group_mode = GROUP_OUTPUT;
	else if (strcmp(s, "desk") == 0)
		group_mode = GROUP_DESK;
	else
		return 0;
	return 1;
}

static int
parse_match(const char *s)
{
	const char *eq;
	const char *value;
	char  field[16];
	size_t len;

	if (n_matches >= MAX_MATCHES)
		return 0;
	eq = strchr(s, '=');
	if (eq == NULL || eq == s || eq[1] == '\0')
		return 0;
	len = (size_t)(eq - s);
	if (len >= sizeof field)
		return 0;
	memcpy(field, s, len);
	field[len] = '\0';

	if (strcmp(field, "title") == 0)
		matches[n_matches].field = MATCH_TITLE;
	else if (strcmp(field, "app") == 0)
		matches[n_matches].field = MATCH_APP;
	else if (strcmp(field, "id") == 0)
		matches[n_matches].field = MATCH_ID;
	else
		return 0;
	value = eq + 1;
	matches[n_matches].negate = 0;
	if (*value == '!') {
		value++;
		if (*value == '\0')
			return 0;
		matches[n_matches].negate = 1;
	}
	strlcpy(matches[n_matches].value, value,
	    sizeof(matches[n_matches].value));
	n_matches++;
	return 1;
}

static int
parse_positive(const char *s, int *out)
{
	int n;

	if (!parse_int(s, &n) || n < 1)
		return 0;
	*out = n;
	return 1;
}

static int
parse_nonnegative(const char *s, int *out)
{
	int n;

	if (!parse_int(s, &n) || n < 0)
		return 0;
	*out = n;
	return 1;
}

static bool
parse_style(char **fields, int n, const char **why)
{
	static const char *names[PALETTE_COUNT] = {
		[PALETTE_BACKGROUND] = "background",
		[PALETTE_ACTIVE_BG] = "active.bg",
		[PALETTE_ACTIVE_FG] = "active.fg",
		[PALETTE_INACTIVE_BG] = "inactive.bg",
		[PALETTE_INACTIVE_FG] = "inactive.fg",
		[PALETTE_ICONIFIED_BG] = "iconified.bg",
		[PALETTE_BORDER] = "border",
	};

	if (n != 3) {
		*why = "style requires NAME COLOUR";
		return false;
	}
	for (int i = 0; i < PALETTE_COUNT; i++) {
		if (strcmp(fields[1], names[i]) != 0)
			continue;
		if (!cow_colour_parse(fields[2], palette_overrides[i])) {
			*why = "bad style colour";
			return false;
		}
		palette_override_set[i] = true;
		return true;
	}
	*why = "unknown style";
	return false;
}

static int
iconman_selection_find(const char *name)
{
	for (int i = 0; i < n_selected_iconmans; i++)
		if (strcmp(selected_iconmans[i].name, name) == 0)
			return i;
	return -1;
}

static bool
iconman_selection_add(const char *name)
{
	if (iconman_selection_find(name) != -1)
		return true;
	if (n_selected_iconmans >= MAX_SELECTED_ICONMANS)
		return false;
	selected_iconmans[n_selected_iconmans].name = strdup(name);
	if (selected_iconmans[n_selected_iconmans].name == NULL)
		return false;
	selected_iconmans[n_selected_iconmans].seen = false;
	n_selected_iconmans++;
	return true;
}

static void
iconman_selections_free(void)
{
	for (int i = 0; i < n_selected_iconmans; i++) {
		free(selected_iconmans[i].name);
		selected_iconmans[i].name = NULL;
		selected_iconmans[i].seen = false;
	}
	n_selected_iconmans = 0;
}

static bool
load_show_iconman(char **fields, int n, const char **why, bool collect)
{
	if (n < 3 || strcmp(fields[1], "iconman") != 0) {
		*why = "bad show";
		return false;
	}
	if (!collect)
		return true;
	for (int i = 2; i < n; i++) {
		if (!iconman_selection_add(fields[i])) {
			*why = "too many icon managers";
			return false;
		}
	}
	return true;
}

static bool
scan_iconman_config_line(const struct cow_module_config_line *line,
    const char **why, void *data)
{
	struct iconman_load_ctx *ctx = data;
	char **fields = line->fields;
	int n = line->nfields;

	if (ctx->depth == 0 && strcmp(fields[0], "show") == 0) {
		bool collect =
		    cow_module_config_instances_select_next(&config_instances);

		if (!load_show_iconman(fields, n, why, collect))
			return false;
	}
	if (strcmp(fields[0], "}") == 0 && n == 1) {
		if (ctx->depth > 0)
			ctx->depth--;
	} else if (strcmp(fields[n - 1], "{") == 0)
		ctx->depth++;
	return true;
}

static bool
load_iconman_option(char **fields, int n, const char **why)
{
	int value;
	bool enabled;

	if (strcmp(fields[0], "output") == 0) {
		if (n != 2) {
			*why = "bad output";
			return false;
		}
		parse_output_filter(fields[1]);
	} else if (strcmp(fields[0], "desk") == 0) {
		if (n != 2 || !parse_desk_filter(fields[1])) {
			*why = "bad desk";
			return false;
		}
	} else if (strcmp(fields[0], "page") == 0) {
		if (n != 2 || !parse_page(fields[1])) {
			*why = "bad page";
			return false;
		}
	} else if (strcmp(fields[0], "sort") == 0) {
		if (n != 2 || !parse_sort(fields[1])) {
			*why = "bad sort";
			return false;
		}
	} else if (strcmp(fields[0], "group-by") == 0) {
		if (n != 2 || !parse_group(fields[1])) {
			*why = "bad group-by";
			return false;
		}
	} else if (strcmp(fields[0], "match") == 0) {
		if (n != 2 || !parse_match(fields[1])) {
			*why = "bad match";
			return false;
		}
	} else if (strcmp(fields[0], "format") == 0) {
		if (n != 2) {
			*why = "bad format";
			return false;
		}
		strlcpy(label_format, fields[1], sizeof(label_format));
	} else if (strcmp(fields[0], "rows") == 0) {
		if (n != 2 || !parse_positive(fields[1], &opt_rows)) {
			*why = "bad rows";
			return false;
		}
	} else if (strcmp(fields[0], "columns") == 0) {
		if (n != 2 || !parse_positive(fields[1], &opt_cols)) {
			*why = "bad columns";
			return false;
		}
	} else if (strcmp(fields[0], "anchor") == 0) {
		if (n != 2 || !parse_anchor(fields[1])) {
			*why = "bad anchor";
			return false;
		}
	} else if (strcmp(fields[0], "button-width") == 0) {
		if (n != 2 || !parse_positive(fields[1], &button_w)) {
			*why = "bad button-width";
			return false;
		}
	} else if (strcmp(fields[0], "button-height") == 0) {
		if (n != 2 || !parse_positive(fields[1], &button_h)) {
			*why = "bad button-height";
			return false;
		}
	} else if (strcmp(fields[0], "button-relief") == 0) {
		if (n != 2 || !parse_button_relief(fields[1])) {
			*why = "bad button-relief";
			return false;
		}
	} else if (strcmp(fields[0], "min-button-width") == 0) {
		if (n != 2 || !parse_positive(fields[1], &min_button_w)) {
			*why = "bad min-button-width";
			return false;
		}
	} else if (strcmp(fields[0], "frame-padding") == 0) {
		if (n != 2 || !parse_nonnegative(fields[1], &frame_padding)) {
			*why = "bad frame-padding";
			return false;
		}
	} else if (strcmp(fields[0], "icons") == 0) {
		if (n != 2 ||
		    !cow_module_config_parse_bool(fields[1], &enabled)) {
			*why = "bad icons";
			return false;
		}
		show_icons = enabled;
	} else if (strcmp(fields[0], "icon-size") == 0) {
		if (n != 2 || !parse_positive(fields[1], &value) ||
		    value > ROW_H - 4) {
			*why = "bad icon-size";
			return false;
		}
		icon_size = value;
	} else if (strcmp(fields[0], "iconified-marker") == 0) {
		if (n != 2 ||
		    !cow_module_config_parse_bool(fields[1], &enabled)) {
			*why = "bad iconified-marker";
			return false;
		}
		iconified_marker = enabled;
	} else if (strcmp(fields[0], "show-self") == 0) {
		if (n != 2 ||
		    !cow_module_config_parse_bool(fields[1], &enabled)) {
			*why = "bad show-self";
			return false;
		}
		show_self = enabled;
	} else if (strcmp(fields[0], "style") == 0) {
		return parse_style(fields, n, why);
	} else {
		*why = "unknown command";
		return false;
	}
	return true;
}

static bool
load_iconman_config_line(const struct cow_module_config_line *line,
    const char **why, void *data)
{
	struct iconman_load_ctx *ctx = data;
	char **fields = line->fields;
	int n = line->nfields;

	if (strcmp(fields[0], "}") == 0) {
		if (n != 1 || ctx->depth == 0) {
			*why = "bad block close";
			return false;
		}
		ctx->depth--;
		if (ctx->depth == 0)
			ctx->in_selected_iconman = false;
		return true;
	}
	if (strcmp(fields[0], "show") == 0)
		return load_show_iconman(fields, n, why, false);
	if (strcmp(fields[0], "iconman") == 0) {
		if (n != 3 || strcmp(fields[2], "{") != 0) {
			*why = "bad iconman";
			return false;
		}
		if (ctx->depth != 0) {
			*why = "nested iconman";
			return false;
		}

		int idx = iconman_selection_find(fields[1]);
		ctx->depth = 1;
		ctx->in_selected_iconman = idx != -1;
		if (idx != -1)
			selected_iconmans[idx].seen = true;
		return true;
	}
	if (ctx->depth > 0 && !ctx->in_selected_iconman)
		return true;
	return load_iconman_option(fields, n, why);
}

static bool
load_iconman_config(const char *path)
{
	struct iconman_load_ctx scan_ctx = { 0 };
	struct iconman_load_ctx load_ctx = { 0 };

	iconman_selections_free();
	config_instances.count = 0;
	if (!cow_module_config_load(PROG, path, MAX_FIELDS,
		scan_iconman_config_line, &scan_ctx))
		return false;
	if (config_instances.count > 0 && n_selected_iconmans == 0) {
		fprintf(stderr, "%s: %s: no show iconman line %d\n", PROG,
		    path, config_instances.index);
		return false;
	}
	if (!cow_module_config_load(PROG, path, MAX_FIELDS,
		load_iconman_config_line, &load_ctx))
		return false;
	if (load_ctx.depth != 0) {
		fprintf(stderr, "%s: %s: unclosed block\n", PROG, path);
		return false;
	}
	for (int i = 0; i < n_selected_iconmans; i++) {
		if (selected_iconmans[i].seen)
			continue;
		fprintf(stderr, "%s: %s: iconman not found: %s\n", PROG,
		    path, selected_iconmans[i].name);
		return false;
	}
	if (min_button_w > button_w) {
		fprintf(stderr, "%s: %s: min-button-width exceeds "
		    "button-width\n", PROG, path);
		return false;
	}
	return true;
}

static void
build_instance_title(void)
{
	size_t len;

	if (n_selected_iconmans == 0) {
		snprintf(self_title, sizeof(self_title), "%s-%ld", PROG,
		    (long)getpid());
		return;
	}
	snprintf(self_title, sizeof(self_title), "%s:%s",
	    PROG, selected_iconmans[0].name);
	len = strlen(self_title);
	for (int i = 1; i < n_selected_iconmans; i++) {
		int n = snprintf(self_title + len, sizeof(self_title) - len,
		    "+%s", selected_iconmans[i].name);
		if (n < 0 || (size_t)n >= sizeof(self_title) - len)
			break;
		len += (size_t)n;
	}
	if (len < sizeof(self_title))
		snprintf(self_title + len, sizeof(self_title) - len, "-%ld",
		    (long)getpid());
}

static int
contains_fold(const char *haystack, const char *needle)
{
	size_t nlen;

	if (haystack == NULL)
		haystack = "";
	if (needle == NULL || *needle == '\0')
		return 1;
	nlen = strlen(needle);
	for (const char *p = haystack; *p != '\0'; p++)
		if (strncasecmp(p, needle, nlen) == 0)
			return 1;
	return 0;
}

static struct tracked_output *
tracked_for_wl(const struct wl_output *wo)
{
	for (int i = 0; i < n_tracked; i++)
		if (tracked[i].wl_output == wo)
			return &tracked[i];
	return NULL;
}

static const char *
self_output_name(void)
{
	for (int i = 0; i < n_tracked; i++)
		if (tracked[i].on_surface && tracked[i].name[0] != '\0')
			return tracked[i].name;
	return NULL;
}

static const struct output *
state_output_by_name(const struct state *st, const char *name)
{
	if (name == NULL || name[0] == '\0')
		return NULL;
	for (int i = 0; i < st->n_outs; i++)
		if (strcmp(st->outs[i].name, name) == 0)
			return &st->outs[i];
	return NULL;
}

static const struct output *
state_current_output(const struct state *st)
{
	for (int i = 0; i < st->n_outs; i++)
		if (st->outs[i].current)
			return &st->outs[i];
	return st->n_outs > 0 ? &st->outs[0] : NULL;
}

static const struct output *
state_output_by_selector(const struct state *st, const char *selector)
{
	struct cow_output_selector_entry entries[MAX_OUTS];

	for (int i = 0; i < st->n_outs; i++) {
		entries[i].name = st->outs[i].name;
		entries[i].number = (uint32_t)st->outs[i].number;
		entries[i].current = st->outs[i].current != 0;
	}
	int selected = cow_output_selector_resolve(selector, entries,
	    (size_t)st->n_outs);
	return selected >= 0 ? &st->outs[selected] : NULL;
}

static const struct output *
selected_output(const struct state *st)
{
	if (output_filter == FILTER_CURRENT)
		return state_current_output(st);
	if (output_filter == FILTER_SELF)
		return state_output_by_name(st, self_output_name());
	if (output_filter != FILTER_EXPLICIT)
		return NULL;
	return state_output_by_selector(st, output_arg);
}

static const struct output *
sizing_output(const struct state *st)
{
	const struct output *out = selected_output(st);

	if (out != NULL)
		return out;
	out = state_output_by_name(st, self_output_name());
	if (out != NULL)
		return out;
	return state_current_output(st);
}

static int
window_on_page(const struct win *w, const struct output *o, int px, int py)
{
	int ow = o->output_w > 0 ? o->output_w : 1;
	int oh = o->output_h > 0 ? o->output_h : 1;
	int vx = px * ow;
	int vy = py * oh;

	if (w->vx + w->w <= vx || w->vx >= vx + ow)
		return 0;
	if (w->vy + w->h <= vy || w->vy >= vy + oh)
		return 0;
	return 1;
}

static int
window_matches(const struct state *st, const struct win *w)
{
	const struct output *out = selected_output(st);

	if (!show_self && strcmp(w->app_id, PROG) == 0)
		return 0;

	if (output_filter != FILTER_ANY) {
		if (out == NULL || strcmp(w->output, out->name) != 0)
			return 0;
	}

	if (desk_filter == FILTER_CURRENT) {
		const struct output *ref = out != NULL ? out :
		    state_current_output(st);
		if (ref == NULL || w->desk != ref->current_desk)
			return 0;
	} else if (desk_filter == FILTER_EXPLICIT && w->desk != desk_arg)
		return 0;

	if (page_filter != FILTER_ANY) {
		const struct output *ref = out != NULL ? out :
		    state_output_by_name(st, w->output);
		int px = page_x_arg, py = page_y_arg;
		if (ref == NULL)
			return 0;
		if (page_filter == FILTER_CURRENT) {
			if (ref->output_w <= 0 || ref->output_h <= 0)
				return 0;
			px = ref->page_vx / ref->output_w;
			py = ref->page_vy / ref->output_h;
		}
		if (!window_on_page(w, ref, px, py))
			return 0;
	}

	for (int i = 0; i < n_matches; i++) {
		const char *value;
		int	    matched;

		switch (matches[i].field) {
		case MATCH_TITLE:
			value = w->title;
			break;
		case MATCH_APP:
			value = w->app_id;
			break;
		case MATCH_ID:
			value = w->id;
			break;
		default:
			value = "";
			break;
		}
		matched = contains_fold(value, matches[i].value);
		if (matches[i].negate)
			matched = !matched;
		if (!matched)
			return 0;
	}

	return 1;
}

static const char *
sort_key(const struct win *w)
{
	if (sort_mode == SORT_APP)
		return w->app_id;
	if (sort_mode == SORT_ID)
		return w->id;
	return w->title[0] != '\0' ? w->title : w->app_id;
}

static int
win_cmp(const void *a, const void *b)
{
	const struct win *wa = a;
	const struct win *wb = b;
	int		  c;

	if (sort_mode == SORT_NONE)
		return 0;
	c = strcasecmp(sort_key(wa), sort_key(wb));
	if (c != 0)
		return c;
	return strcmp(wa->id, wb->id);
}

static int
win_group_cmp(const void *a, const void *b)
{
	const struct win *wa = a;
	const struct win *wb = b;
	char		  abuf[32], bbuf[32];
	int		  c;

	c = strcasecmp(group_key(wa, abuf, sizeof abuf),
	    group_key(wb, bbuf, sizeof bbuf));
	if (c != 0)
		return c;
	return win_cmp(a, b);
}

static const char *
group_key(const struct win *w, char *buf, size_t bufsz)
{
	switch (group_mode) {
	case GROUP_APP:
		return w->app_id[0] != '\0' ? w->app_id : "(no app)";
	case GROUP_TITLE:
		return w->title[0] != '\0' ? w->title : "(untitled)";
	case GROUP_OUTPUT:
		return w->output[0] != '\0' ? w->output : "(no output)";
	case GROUP_DESK:
		snprintf(buf, bufsz, "desk %d", w->desk);
		return buf;
	case GROUP_NONE:
		break;
	}
	return w->id;
}

static int
same_group(const struct win *a, const struct win *b)
{
	char abuf[32], bbuf[32];

	return strcmp(group_key(a, abuf, sizeof abuf),
	    group_key(b, bbuf, sizeof bbuf)) == 0;
}

static void
set_group_label(struct row *r)
{
	char key[256];
	const char *s;

	if (r->count <= 1) {
		format_label(r->label, sizeof(r->label), &r->win);
		return;
	}
	s = group_key(&r->win, key, sizeof key);
	if (s != key)
		strlcpy(key, s, sizeof key);
	snprintf(r->label, sizeof(r->label), "%s (%d)", key, r->count);
}

static void
finish_row(struct row *r)
{
	r->win = group_wins[r->first];
	r->focused = 0;
	r->any_iconified = 0;
	r->all_iconified = 1;
	for (int i = 0; i < r->count; i++) {
		const struct win *w = &group_wins[r->first + i];

		if (w->focused)
			r->focused = 1;
		if (w->iconified)
			r->any_iconified = 1;
		else
			r->all_iconified = 0;
	}
	r->win.focused = r->focused;
	r->win.iconified = r->any_iconified;
	set_group_label(r);
}

static void
layout_grid(int n, int *cols, int *rows, int *column_major)
{
	if (n < 1)
		n = 1;

	*column_major = 0;
	if (opt_rows > 0 && opt_cols > 0) {
		*rows = opt_rows;
		*cols = opt_cols;
	} else if (opt_rows > 0) {
		*rows = opt_rows;
		*cols = (n + opt_rows - 1) / opt_rows;
		*column_major = 1;
	} else if (opt_cols > 0) {
		*cols = opt_cols;
		*rows = (n + opt_cols - 1) / opt_cols;
	} else {
		*cols = 1;
		*rows = n;
	}
	if (*cols < 1)
		*cols = 1;
	if (*rows < 1)
		*rows = 1;
}

static int
max_columns_for_width(int width)
{
	int cols;

	if (width <= frame_padding * 2)
		return 1;
	cols = (width - frame_padding * 2) / min_button_w;
	return cols > 0 ? cols : 1;
}

static int
single_row_anchor(void)
{
	return opt_rows == 1 && anchor_mode != ANCHOR_NONE;
}

static void
copy_colour(float dst[4], const float src[4])
{
	memcpy(dst, src, sizeof(float) * 4);
}

static const float *
theme_colour(bool primary_set, const float primary[4], bool fallback_set,
    const float fallback[4], const float defaults[4])
{
	if (primary_set)
		return (primary);
	if (fallback_set)
		return (fallback);
	return (defaults);
}

static void
resolve_palette(const struct state *st, const struct output *output,
    struct palette *palette)
{
	const struct cow_module_theme *generic = &st->theme;
	const struct cow_module_theme *titlebar = &st->titlebar_theme;

	const float *inactive_bg = theme_colour(titlebar->has_inactive_bg,
	    titlebar->inactive_bg, generic->has_inactive_bg,
	    generic->inactive_bg, default_inactive_bg);
	if (output != NULL && output->has_background)
		copy_colour(palette->background, output->background);
	else
		copy_colour(palette->background, generic->has_background ?
		    generic->background : default_background);
	copy_colour(palette->active_bg, theme_colour(titlebar->has_active_bg,
	    titlebar->active_bg, generic->has_active_bg, generic->active_bg,
	    default_active_bg));
	copy_colour(palette->inactive_bg, inactive_bg);
	copy_colour(palette->active_fg, theme_colour(titlebar->has_active_fg,
	    titlebar->active_fg, generic->has_active_fg, generic->active_fg,
	    default_active_fg));
	copy_colour(palette->inactive_fg,
	    theme_colour(titlebar->has_inactive_fg, titlebar->inactive_fg,
		generic->has_inactive_fg, generic->inactive_fg,
		default_inactive_fg));
	float *colours[PALETTE_COUNT] = {
		[PALETTE_BACKGROUND] = palette->background,
		[PALETTE_ACTIVE_BG] = palette->active_bg,
		[PALETTE_ACTIVE_FG] = palette->active_fg,
		[PALETTE_INACTIVE_BG] = palette->inactive_bg,
		[PALETTE_INACTIVE_FG] = palette->inactive_fg,
		[PALETTE_ICONIFIED_BG] = palette->iconified_bg,
		[PALETTE_BORDER] = palette->border,
	};
	for (int i = 0; i < PALETTE_ICONIFIED_BG; i++)
		if (palette_override_set[i])
			copy_colour(colours[i], palette_overrides[i]);
	cow_colour_darken(palette->iconified_bg, palette->inactive_bg);
	if (palette_override_set[PALETTE_ICONIFIED_BG])
		copy_colour(palette->iconified_bg,
		    palette_overrides[PALETTE_ICONIFIED_BG]);
	if (palette_override_set[PALETTE_BORDER])
		copy_colour(palette->border,
		    palette_overrides[PALETTE_BORDER]);
	palette->font = titlebar->has_font ? titlebar->font :
	    generic->has_font ? generic->font : "Sans 12";
	palette->height = titlebar->has_height ? titlebar->height : ROW_H;
	if (button_h > 0)
		palette->height = button_h;
	if (show_icons && palette->height < icon_size + 4)
		palette->height = icon_size + 4;
	if (iconified_marker && palette->height < MARKER_SIZE + 4)
		palette->height = MARKER_SIZE + 4;
}

static void
clamp_scroll(void)
{
	int max_first;

	if (visible_capacity < 1 || total_rows <= visible_capacity) {
		scroll_first = 0;
		return;
	}
	max_first = total_rows - visible_capacity;
	if (scroll_first > max_first)
		scroll_first = max_first;
	if (scroll_first < 0)
		scroll_first = 0;
}

static void
collect_rows(const struct state *st, int *column_major)
{
	int cols, grid_rows, cm;

	*column_major = 0;
	layout_cols = 1;
	layout_rows = 1;
	n_rows = 0;
	n_group_wins = 0;
	for (int i = 0; i < st->n_wins && n_group_wins < MAX_WINS; i++) {
		if (!window_matches(st, &st->wins[i]))
			continue;
		group_wins[n_group_wins++] = st->wins[i];
	}
	if (group_mode != GROUP_NONE)
		qsort(group_wins, (size_t)n_group_wins, sizeof(group_wins[0]),
		    win_group_cmp);
	else if (sort_mode != SORT_NONE)
		qsort(group_wins, (size_t)n_group_wins, sizeof(group_wins[0]),
		    win_cmp);

	for (int i = 0; i < n_group_wins && n_rows < MAX_ROWS;) {
		rows[n_rows].first = i;
		rows[n_rows].count = 1;
		if (group_mode != GROUP_NONE) {
			while (i + rows[n_rows].count < n_group_wins &&
			    same_group(&group_wins[i],
				&group_wins[i + rows[n_rows].count]))
				rows[n_rows].count++;
		}
		finish_row(&rows[n_rows]);
		i += rows[n_rows].count;
		n_rows++;
	}

	total_rows = n_rows;
	layout_grid(n_rows, &cols, &grid_rows, &cm);
	layout_cols = cols;
	layout_rows = grid_rows;
	*column_major = cm;
}

static void
apply_width_limit(int max_w, int *desired_w)
{
	int capacity;

	visible_capacity = n_rows;
	if (single_row_anchor() && max_w > 0) {
		layout_cols = n_rows > 0 ? n_rows : 1;
		layout_rows = 1;
		visible_capacity = n_rows;
		scroll_first = 0;
		*desired_w = max_w;
		return;
	}
	if (max_w <= 0 || *desired_w <= max_w)
		return;

	if (opt_rows > 0) {
		int fit_cols = max_columns_for_width(max_w);

		capacity = fit_cols * layout_rows;
		if (capacity < 1)
			capacity = 1;
		visible_capacity = capacity;
		clamp_scroll();
		if (n_rows > visible_capacity) {
			layout_cols = fit_cols;
			memmove(rows, rows + scroll_first,
			    sizeof(rows[0]) * (size_t)visible_capacity);
			n_rows = visible_capacity;
			*desired_w = max_w;
			return;
		}
	}

	*desired_w = frame_padding * 2 + layout_cols * min_button_w;
	if (*desired_w > max_w)
		*desired_w = max_w;
}

static void
layout_rows_for_width(int width, int column_major, int row_height)
{
	int cell_w = (width - frame_padding * 2) / layout_cols;
	if (cell_w < 1)
		cell_w = 1;

	for (int i = 0; i < n_rows; i++) {
		int col, row;
		if (column_major) {
			col = i / layout_rows;
			row = i % layout_rows;
		} else {
			col = i % layout_cols;
			row = i / layout_cols;
		}
		rows[i].x = frame_padding + col * cell_w;
		rows[i].y = frame_padding + row * row_height;
		rows[i].w = cell_w;
		rows[i].h = row_height;
		if (row + 1 > layout_rows)
			layout_rows = row + 1;
	}
}

static const struct win *
self_window(const struct state *st)
{
	for (int i = 0; i < st->n_wins; i++) {
		if (strcmp(st->wins[i].app_id, PROG) != 0)
			continue;
		if (strcmp(st->wins[i].title, self_title) == 0)
			return &st->wins[i];
	}
	return NULL;
}

static void
apply_anchor(const struct state *st, const struct output *out)
{
	const struct win *self;
	char		  cmd[512];
	int		  x, y;

	if (anchor_mode == ANCHOR_NONE || out == NULL)
		return;
	self = self_window(st);
	if (self == NULL)
		return;

	x = out->usable_x;
	y = out->usable_y;
	if (anchor_mode == ANCHOR_BOTTOM)
		y = out->usable_y + out->usable_h - win_h;

	if (self->x == x && self->y == y)
		return;
	snprintf(cmd, sizeof(cmd), "window-move -t '#%s' -x %d -y %d",
	    self->id, x, y);
	ipc_send(cmd);
}

static void
format_label(char *dst, size_t dstsz, const struct win *w)
{
	size_t len = 0;

	if (dstsz == 0)
		return;
	for (const char *p = label_format; *p != '\0' && len + 1 < dstsz;
	    p++) {
		const char *s = NULL;
		char	    num[32];
		if (*p != '%') {
			dst[len++] = *p;
			continue;
		}
		p++;
		if (*p == '\0')
			break;
		switch (*p) {
		case 't':
			s = w->title[0] != '\0' ? w->title : "(untitled)";
			break;
		case 'a':
			s = w->app_id;
			break;
		case 'i':
			snprintf(num, sizeof(num), "%.8s", w->id);
			s = num;
			break;
		case 'o':
			s = w->output;
			break;
		case 'd':
			snprintf(num, sizeof(num), "%d", w->desk);
			s = num;
			break;
		case '%':
			dst[len++] = '%';
			continue;
		default:
			dst[len++] = '%';
			dst[len++] = *p;
			continue;
		}
		if (s == NULL)
			s = "";
		while (*s != '\0' && len + 1 < dstsz)
			dst[len++] = *s++;
	}
	dst[len] = '\0';
}

static void
draw_iconified_marker(cairo_t *cr, const struct win *win, double x,
    double y, double h, const float base[4])
{
	double marker_y = y + (h - MARKER_SIZE) / 2.0;

	if (!win->iconified)
		return;
	cow_module_cairo_fill(cr, base, x, marker_y, MARKER_SIZE, MARKER_SIZE);
	cow_module_cairo_relief(cr, x, marker_y, MARKER_SIZE, MARKER_SIZE,
	    false, base);
}

static void
draw_button_frame(cairo_t *cr, double x, double y, double width,
    double height, bool sunken, const float background[4],
    const struct palette *palette)
{
	if (button_relief != BUTTON_RELIEF_FLAT) {
		if (button_relief == BUTTON_RELIEF_RAISED)
			sunken = false;
		else if (button_relief == BUTTON_RELIEF_SUNKEN)
			sunken = true;
		cow_module_cairo_relief(cr, x, y, width, height, sunken,
		    background);
	}
	if (!palette_override_set[PALETTE_BORDER])
		return;

	cairo_save(cr);
	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
	cow_module_cairo_set_source(cr, palette->border);
	cairo_set_line_width(cr, 1.0);
	cairo_rectangle(cr, x + 0.5, y + 0.5, width - 1.0, height - 1.0);
	cairo_stroke(cr);
	cairo_restore(cr);
}

static void
draw_icon(cairo_t *cr, const char *path, int x, int y, int size)
{
	cairo_surface_t	 *target;
	struct cow_pixbuf pixbuf;

	if (path[0] == '\0')
		return;

	target = cairo_get_target(cr);
	cairo_surface_flush(target);
	pixbuf = (struct cow_pixbuf) {
		.pixels = (uint32_t *)cairo_image_surface_get_data(target),
		.stride_px = cairo_image_surface_get_stride(target) / 4,
		.width = cairo_image_surface_get_width(target),
		.height = cairo_image_surface_get_height(target),
	};
	(void)cow_image_cache_blit(&icon_cache, path, NULL, &pixbuf, x, y,
	    size);
	cairo_surface_mark_dirty(target);
}

static void
do_render(const struct state *st)
{
	struct cow_module_shm_buffer *buf;
	cairo_surface_t *cs;
	cairo_t		*cr;
	int		 column_major;
	int		 desired_w;
	const struct output *out;
	struct palette	 palette;
	int		 max_w = 0, max_h = 0;

	collect_rows(st, &column_major);

	out = sizing_output(st);
	resolve_palette(st, out, &palette);
	if (out != NULL) {
		max_w = out->usable_w > 0 ? out->usable_w : out->w;
		max_h = out->usable_h > 0 ? out->usable_h : out->h;
	}

	desired_w = frame_padding * 2 + layout_cols * button_w;
	apply_width_limit(max_w, &desired_w);
	win_w	  = desired_w;
	layout_rows_for_width(win_w, column_major, palette.height);
	win_h = frame_padding * 2 + layout_rows * palette.height;
	if (max_h > 0 && win_h > max_h)
		win_h = max_h;

	buf = cow_module_shm_buffer_next(buffers,
	    sizeof(buffers) / sizeof(buffers[0]), wl_shm, PROG, win_w, win_h);
	if (buf == NULL)
		return;

	cs = cairo_image_surface_create_for_data(buf->data, CAIRO_FORMAT_ARGB32,
	    win_w, win_h, win_w * 4);
	cr = cairo_create(cs);
	PangoFontDescription *font =
	    pango_font_description_from_string(palette.font);
	cow_module_cairo_fill(cr, palette.background, 0, 0, win_w, win_h);

	if (n_rows == 0) {
		cow_module_cairo_fill(cr, palette.active_bg, frame_padding, frame_padding,
		    win_w - frame_padding * 2, palette.height);
		draw_button_frame(cr, frame_padding, frame_padding,
		    win_w - frame_padding * 2, palette.height, false,
		    palette.active_bg, &palette);
		cow_module_cairo_set_source(cr, palette.active_fg);
		cow_text_render(cr, font, frame_padding + TEXT_PAD, frame_padding,
		    win_w - 2 * (frame_padding + TEXT_PAD), palette.height, "cowiconman",
		    PANGO_ALIGN_LEFT, NULL);
	} else {
		for (int i = 0; i < n_rows; i++) {
			struct row *r = &rows[i];
			bool	    sunken = r->focused || r->any_iconified;
			int	    label_x = r->x + TEXT_PAD;
			int	    label_w = r->w - 2 * TEXT_PAD;
			const float *bg = r->focused ? palette.active_bg :
			    r->all_iconified ? palette.iconified_bg :
			    palette.inactive_bg;
			const float *fg = r->focused ? palette.active_fg :
			    palette.inactive_fg;

			cow_module_cairo_fill(cr, bg, r->x, r->y, r->w - 1,
			    r->h - 1);
			draw_button_frame(cr, r->x, r->y, r->w - 1,
			    r->h - 1, sunken, bg, &palette);

			if (show_icons) {
				draw_icon(cr, r->win.icon, label_x,
				    r->y + (r->h - icon_size) / 2, icon_size);
				label_x += icon_size + MARKER_GAP;
				label_w -= icon_size + MARKER_GAP;
			}
			if (iconified_marker) {
				draw_iconified_marker(cr, &r->win, label_x, r->y,
				    r->h, palette.iconified_bg);
				label_x += MARKER_SIZE + MARKER_GAP;
				label_w -= MARKER_SIZE + MARKER_GAP;
			}
			cow_module_cairo_set_source(cr, fg);
			cow_text_render(cr, font, label_x, r->y, label_w, r->h,
			    r->label, PANGO_ALIGN_LEFT, NULL);
		}
	}

	pango_font_description_free(font);
	cairo_destroy(cr);
	cairo_surface_flush(cs);
	cairo_surface_destroy(cs);

	buf->busy = true;
	wl_surface_attach(surface, buf->buffer, 0, 0);
	wl_surface_damage_buffer(surface, 0, 0, win_w, win_h);
	wl_surface_commit(surface);

	apply_anchor(st, out);
}

static void
ipc_send(const char *cmd)
{
	(void)cow_module_cmd_execute(PROG, cmd, true);
}

static char *
ipc_query(const char *cmd)
{
	return cow_module_cmd_request(PROG, cmd, true);
}

static void
activate_win(const struct win *w, uint32_t button)
{
	char cmd[256];

	if (button == BTN_LEFT) {
		if (w->iconified) {
			snprintf(cmd, sizeof(cmd), "winops -t '#%s' -i",
			    w->id);
			ipc_send(cmd);
		}
		snprintf(cmd, sizeof(cmd), "focus -t '#%s'", w->id);
		ipc_send(cmd);
		snprintf(cmd, sizeof(cmd), "winops -t '#%s' -r", w->id);
		ipc_send(cmd);
	} else if (button == BTN_MIDDLE) {
		snprintf(cmd, sizeof(cmd), "winops -t '#%s' -i",
		    w->id);
		ipc_send(cmd);
	}
	fprintf(stderr, "SENT: %s\n", cmd);
}

static void
filter_append_string(char **dst, const char *value)
{
	*(*dst)++ = '"';
	for (const char *p = value; *p != '\0'; p++) {
		if (*p == '"' || *p == '\\')
			*(*dst)++ = '\\';
		*(*dst)++ = *p;
	}
	*(*dst)++ = '"';
}

static const char *
native_sort_name(void)
{
	switch (sort_mode) {
	case SORT_TITLE:
		return "title";
	case SORT_APP:
		return "app";
	case SORT_ID:
		return "id";
	case SORT_NONE:
		return "none";
	}
	return "none";
}

static char *
native_group_format(void)
{
	static const char suffix[] = " [%I] %o:%d";
	size_t size = strlen(label_format) + sizeof(suffix);
	char *format = malloc(size);

	if (format == NULL)
		return NULL;
	char *dst = format;
	for (const char *src = label_format; *src != '\0'; src++) {
		*dst++ = *src;
		if (*src == '%' && src[1] != '\0') {
			src++;
			*dst++ = *src == 'i' ? 'I' : *src;
		}
	}
	memcpy(dst, suffix, sizeof(suffix));
	return format;
}

static void
show_native_group(const struct row *r)
{
	static const char prefix[] = "self.id in [";
	size_t filter_size = strlen("self.id in []") + 1;

	for (int i = 0; i < r->count; i++)
		filter_size += strlen(group_wins[r->first + i].id) * 2 + 4;
	char *filter = malloc(filter_size);
	if (filter == NULL)
		return;
	memcpy(filter, prefix, sizeof(prefix) - 1);
	char *p = filter + sizeof(prefix) - 1;
	for (int i = 0; i < r->count; i++) {
		if (i != 0) {
			*p++ = ',';
			*p++ = ' ';
		}
		filter_append_string(&p, group_wins[r->first + i].id);
	}
	*p++ = ']';
	*p = '\0';

	char *format = native_group_format();
	if (format == NULL) {
		free(filter);
		return;
	}

	const char *argv[11];
	size_t argc = 0;
	argv[argc++] = "window-list";
	argv[argc++] = "-n";
	argv[argc++] = "-p";
	if (show_icons)
		argv[argc++] = "-I";
	argv[argc++] = "-F";
	argv[argc++] = filter;
	argv[argc++] = "-f";
	argv[argc++] = format;
	argv[argc++] = "-S";
	argv[argc++] = native_sort_name();
	char *command = cow_command_from_argv(argc, argv);
	if (command != NULL) {
		ipc_send(command);
		free(command);
	}
	free(format);
	free(filter);
}

static void
activate_row(int idx, uint32_t button)
{
	struct row *r;

	if (idx < 0 || idx >= n_rows)
		return;
	r = &rows[idx];
	if (r->count > 1) {
		if (button == BTN_LEFT)
			show_native_group(r);
		return;
	}
	activate_win(&r->win, button);
}

static void
parse_status(const char *json)
{
	cJSON	     *root = cJSON_Parse(json);
	static struct state st;
	cJSON	     *outs, *out;
	int	      n = 0;

	if (root == NULL)
		return;
	memset(&st, 0, sizeof(st));

	outs = cJSON_GetObjectItem(root, "outputs");
	cJSON_ArrayForEach(out, outs)
	{
		if (n >= MAX_OUTS)
			break;
		cow_json_copy_string(out, "name", st.outs[n].name,
		    sizeof(st.outs[n].name));
		st.outs[n].number	= get_jnum(out, "number");
		st.outs[n].x		= get_jnum(out, "x");
		st.outs[n].y		= get_jnum(out, "y");
		st.outs[n].w		= get_jnum(out, "width");
		st.outs[n].h		= get_jnum(out, "height");
		st.outs[n].current	= get_jbool(out, "current");
		st.outs[n].current_desk = get_jnum(out, "current_desk");
		st.outs[n].page_vx	= get_jnum(out, "page_vx");
		st.outs[n].page_vy	= get_jnum(out, "page_vy");
		st.outs[n].output_w	= get_jnum(out, "output_w");
		st.outs[n].output_h	= get_jnum(out, "output_h");
		cJSON *usable = cJSON_GetObjectItem(out, "usable");
		if (cJSON_IsObject(usable)) {
			st.outs[n].usable_x = get_jnum(usable, "x");
			st.outs[n].usable_y = get_jnum(usable, "y");
			st.outs[n].usable_w = get_jnum(usable, "width");
			st.outs[n].usable_h = get_jnum(usable, "height");
		}
		struct cow_module_theme output_theme;
		(void)cow_module_theme_parse(out, &output_theme);
		st.outs[n].has_background = output_theme.has_background;
		if (output_theme.has_background) {
			copy_colour(st.outs[n].background,
			    output_theme.background);
		}
		n++;
	}
	st.n_outs = n;

	cJSON *theme = cJSON_GetObjectItem(root, "theme");
	(void)cow_module_theme_parse(theme, &st.theme);
	if (cJSON_IsObject(theme)) {
		cJSON *titlebar = cJSON_GetObjectItem(theme, "titlebar");
		(void)cow_module_theme_parse(titlebar, &st.titlebar_theme);
	}

	pthread_mutex_lock(&lock);
	st.n_wins = global_state.n_wins;
	memcpy(st.wins, global_state.wins,
	    sizeof(st.wins[0]) * (size_t)st.n_wins);
	global_state = st;
	dirty	     = 1;
	pthread_mutex_unlock(&lock);

	cJSON_Delete(root);
}

static void
fetch_windows(void)
{
	char	     *resp = ipc_query("show -a window");
	cJSON	     *root, *data, *w;
	static struct win wins[MAX_WINS];
	char	     new_self_id[MAX_NAME] = "";
	int	      n = 0;

	if (resp == NULL)
		return;
	root = cJSON_Parse(resp);
	free(resp);
	if (root == NULL)
		return;

	data = cJSON_GetObjectItem(root, "data");
	cJSON_ArrayForEach(w, data)
	{
		if (n >= MAX_WINS)
			break;
		memset(&wins[n], 0, sizeof(wins[n]));
		cow_json_copy_string(w, "id", wins[n].id,
		    sizeof(wins[n].id));
		cow_json_copy_string(w, "app_id", wins[n].app_id,
		    sizeof(wins[n].app_id));
		cow_json_copy_string(w, "title", wins[n].title,
		    sizeof(wins[n].title));
		cow_json_copy_string(w, "output", wins[n].output,
		    sizeof(wins[n].output));
		cow_json_copy_string(w, "icon", wins[n].icon,
		    sizeof(wins[n].icon));
		if (strcmp(wins[n].app_id, PROG) == 0 &&
		    strcmp(wins[n].title, self_title) == 0)
			strlcpy(new_self_id, wins[n].id, sizeof(new_self_id));
		wins[n].desk	   = get_jnum(w, "desk");
		wins[n].x	   = get_jnum(w, "x");
		wins[n].y	   = get_jnum(w, "y");
		wins[n].vx	   = get_jnum(w, "vx");
		wins[n].vy	   = get_jnum(w, "vy");
		wins[n].w	   = get_jnum(w, "width");
		wins[n].h	   = get_jnum(w, "height");
		wins[n].focused   = get_jbool(w, "focused");
		wins[n].iconified = get_jbool(w, "iconified");
		n++;
	}
	cJSON_Delete(root);

	pthread_mutex_lock(&lock);
	strlcpy(self_id, new_self_id, sizeof(self_id));
	global_state.n_wins = n;
	memcpy(global_state.wins, wins, sizeof(wins[0]) * (size_t)n);
	dirty = 1;
	pthread_mutex_unlock(&lock);
}

static void *
status_thread(__unused void *arg)
{
	int fd = cow_module_status_connect(PROG, COW_MODULE_STATUS_RETRIES,
	    COW_MODULE_STATUS_RETRY_DELAY);
	FILE *f;
	static char line[COW_MODULE_STATUS_LINE_MAX];

	if (fd < 0)
		return NULL;
	f = fdopen(fd, "r");
	if (f == NULL) {
		close(fd);
		return NULL;
	}
	setlinebuf(f);
	while (running && fgets(line, sizeof(line), f) != NULL) {
		size_t len = strlen(line);
		if (len > 0 && line[len - 1] == '\n')
			line[len - 1] = '\0';
		parse_status(line);
		fetch_windows();
		(void)write(wake_pipe[1], "x", 1);
	}
	fclose(f);
	return NULL;
}

static void
handle_wake(void)
{
	char buf[64];
	while (read(wake_pipe[0], buf, sizeof(buf)) > 0)
		;

	pthread_mutex_lock(&lock);
	struct state st = global_state;
	dirty		= 0;
	pthread_mutex_unlock(&lock);

	if (configured)
		do_render(&st);
}

static void
render_current_state(void)
{
	struct state st;

	pthread_mutex_lock(&lock);
	st = global_state;
	pthread_mutex_unlock(&lock);

	if (configured)
		do_render(&st);
}

static void
wayland_cb(__unused evutil_socket_t fd, __unused short events,
    __unused void *data)
{
	if (wl_display_dispatch(display) < 0)
		running = 0;
}

static void
wake_cb(__unused evutil_socket_t fd, __unused short events, __unused void *data)
{
	handle_wake();
}

static void
xdg_surface_configure(__unused void *data, struct xdg_surface *xs,
    uint32_t serial)
{
	xdg_surface_ack_configure(xs, serial);
	configured = 1;
	handle_wake();
}

static const struct xdg_surface_listener xdg_surface_listener = {
	.configure = xdg_surface_configure,
};

static void
xdg_toplevel_configure(__unused void *data, __unused struct xdg_toplevel *tl,
    __unused int32_t w, __unused int32_t h, __unused struct wl_array *states)
{
}

static void
xdg_toplevel_close(__unused void *data, __unused struct xdg_toplevel *tl)
{
	running = 0;
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

static int
row_at(double x, double y)
{
	for (int i = 0; i < n_rows; i++) {
		if (x >= rows[i].x && x < rows[i].x + rows[i].w &&
		    y >= rows[i].y && y < rows[i].y + rows[i].h)
			return i;
	}
	return -1;
}

static void
ptr_enter(__unused void *d, __unused struct wl_pointer *p,
    uint32_t serial, __unused struct wl_surface *s, wl_fixed_t x,
    wl_fixed_t y)
{
	cow_module_cursor_set_default(&cursor, serial);
	ptr_x = wl_fixed_to_double(x);
	ptr_y = wl_fixed_to_double(y);
}

static void
ptr_leave(__unused void *d, __unused struct wl_pointer *p,
    __unused uint32_t serial, __unused struct wl_surface *s)
{
}

static void
ptr_motion(__unused void *d, __unused struct wl_pointer *p,
    __unused uint32_t time, wl_fixed_t x, wl_fixed_t y)
{
	ptr_x = wl_fixed_to_double(x);
	ptr_y = wl_fixed_to_double(y);
}

static void
ptr_button(__unused void *d, __unused struct wl_pointer *p,
    __unused uint32_t serial, __unused uint32_t time, uint32_t button,
    uint32_t state)
{
	if (state == WL_POINTER_BUTTON_STATE_PRESSED)
		return;
	if (state != WL_POINTER_BUTTON_STATE_RELEASED)
		return;
	activate_row(row_at(ptr_x, ptr_y), button);
}

static void
ptr_axis(__unused void *d, __unused struct wl_pointer *p, __unused uint32_t t,
    uint32_t a, wl_fixed_t v)
{
	int step;

	if (a != WL_POINTER_AXIS_VERTICAL_SCROLL ||
	    visible_capacity < 1 || total_rows <= visible_capacity)
		return;

	step = layout_rows > 0 ? layout_rows : 1;
	if (wl_fixed_to_double(v) > 0)
		scroll_first += step;
	else
		scroll_first -= step;
	clamp_scroll();
	render_current_state();
}

static void
ptr_frame(__unused void *d, __unused struct wl_pointer *p)
{
}

static void
ptr_axis_source(__unused void *d, __unused struct wl_pointer *p,
    __unused uint32_t s)
{
}

static void
ptr_axis_stop(__unused void *d, __unused struct wl_pointer *p,
    __unused uint32_t t, __unused uint32_t a)
{
}

static void
ptr_axis_discrete(__unused void *d, __unused struct wl_pointer *p,
    __unused uint32_t a, __unused int32_t v)
{
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
	if ((caps & WL_SEAT_CAPABILITY_POINTER) && pointer == NULL) {
		pointer = wl_seat_get_pointer(s);
		wl_pointer_add_listener(pointer, &ptr_listener, NULL);
		cow_module_cursor_set_pointer(&cursor, pointer);
	} else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && pointer != NULL) {
		cow_module_cursor_clear_pointer(&cursor, pointer);
		wl_pointer_destroy(pointer);
		pointer = NULL;
	}
}

static void
seat_name(__unused void *d, __unused struct wl_seat *s, __unused const char *n)
{
}

static const struct wl_seat_listener seat_listener = {
	.capabilities = seat_caps,
	.name	      = seat_name,
};

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
	struct tracked_output *to = tracked_for_wl(wo);

	if (to == NULL || name == NULL)
		return;
	strlcpy(to->name, name, sizeof(to->name));
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
surface_enter(__unused void *d, __unused struct wl_surface *s,
    struct wl_output *wo)
{
	struct tracked_output *to = tracked_for_wl(wo);
	if (to != NULL)
		to->on_surface = 1;
}

static void
surface_leave(__unused void *d, __unused struct wl_surface *s,
    struct wl_output *wo)
{
	struct tracked_output *to = tracked_for_wl(wo);
	if (to != NULL)
		to->on_surface = 0;
}

static const struct wl_surface_listener surface_listener = {
	.enter = surface_enter,
	.leave = surface_leave,
};

static void
reg_global(__unused void *d, struct wl_registry *reg, uint32_t name,
    const char *iface, uint32_t ver)
{
	if (strcmp(iface, wl_compositor_interface.name) == 0)
		compositor = wl_registry_bind(reg, name,
		    &wl_compositor_interface, 4);
	else if (strcmp(iface, wl_shm_interface.name) == 0)
		wl_shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
	else if (strcmp(iface, xdg_wm_base_interface.name) == 0) {
		xdg_wm_base = wl_registry_bind(reg, name,
		    &xdg_wm_base_interface, 1);
		xdg_wm_base_add_listener(xdg_wm_base, &xdg_wm_base_listener,
		    NULL);
	} else if (strcmp(iface, wl_seat_interface.name) == 0) {
		seat = wl_registry_bind(reg, name, &wl_seat_interface, 5);
		wl_seat_add_listener(seat, &seat_listener, NULL);
	} else if (strcmp(iface, wl_output_interface.name) == 0) {
		if (n_tracked >= MAX_OUTS)
			return;
		uint32_t bind_ver = ver >= 4 ? 4 : ver;
		tracked[n_tracked].wl_output = wl_registry_bind(reg, name,
		    &wl_output_interface, bind_ver);
		tracked[n_tracked].global_name = name;
		if (tracked[n_tracked].wl_output != NULL) {
			wl_output_add_listener(tracked[n_tracked].wl_output,
			    &out_listener, NULL);
			n_tracked++;
		}
	} else
		cow_module_cursor_handle_global(&cursor, reg, name, iface,
		    ver);
}

static void
reg_remove(__unused void *d, __unused struct wl_registry *r, uint32_t name)
{
	for (int i = 0; i < n_tracked; i++) {
		if (tracked[i].global_name != name)
			continue;
		if (tracked[i].wl_output != NULL)
			wl_output_destroy(tracked[i].wl_output);
		int last = n_tracked - 1;
		if (i != last)
			tracked[i] = tracked[last];
		memset(&tracked[last], 0, sizeof(tracked[last]));
		n_tracked--;
		break;
	}
}

static const struct wl_registry_listener reg_listener = {
	.global	       = reg_global,
	.global_remove = reg_remove,
};

static int
parse_args(int argc, char **argv, char **config_path)
{
	int ch;

	while ((ch = getopt(argc, argv, "c:h")) != -1) {
		switch (ch) {
		case 'c':
			free(*config_path);
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
	char		   *config_path = NULL;
	bool		    explicit_config;
	bool		    config_loaded = false;
	struct wl_registry *reg;
	pthread_t	    tid;
	struct event_base  *base;
	struct event	   *wl_ev = NULL, *wake_ev = NULL;

	if (!cow_module_config_instances_init(&config_instances, PROG,
		SHOW_INDEX_ENV))
		return 1;
	if (!parse_args(argc, argv, &config_path)) {
		usage();
		return 1;
	}
	explicit_config = config_path != NULL;
	if (config_path == NULL)
		config_path = cow_module_config_default_path(PROG);
	if (config_path != NULL &&
	    (explicit_config || access(config_path, R_OK) == 0)) {
		if (!load_iconman_config(config_path)) {
			free(config_path);
			return 1;
		}
		config_loaded = true;
	}
	if (config_loaded)
		cow_module_config_instances_spawn(&config_instances, argv);
	free(config_path);
	build_instance_title();

	if (pipe(wake_pipe) < 0) {
		perror("pipe");
		return 1;
	}
	fcntl(wake_pipe[0], F_SETFL, O_NONBLOCK);

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
	wl_surface_add_listener(surface, &surface_listener, NULL);
	xdg_surface = xdg_wm_base_get_xdg_surface(xdg_wm_base, surface);
	xdg_surface_add_listener(xdg_surface, &xdg_surface_listener, NULL);
	xdg_toplevel = xdg_surface_get_toplevel(xdg_surface);
	xdg_toplevel_add_listener(xdg_toplevel, &xdg_toplevel_listener, NULL);
	xdg_toplevel_set_title(xdg_toplevel, self_title);
	xdg_toplevel_set_app_id(xdg_toplevel, PROG);
	wl_surface_commit(surface);

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

	if (xdg_toplevel != NULL)
		xdg_toplevel_destroy(xdg_toplevel);
	if (xdg_surface != NULL)
		xdg_surface_destroy(xdg_surface);
	if (surface != NULL)
		wl_surface_destroy(surface);
	if (pointer != NULL) {
		cow_module_cursor_clear_pointer(&cursor, pointer);
		wl_pointer_destroy(pointer);
	}
	cow_module_cursor_fini(&cursor);
	cow_image_cache_clear(&icon_cache);
	if (seat != NULL)
		wl_seat_destroy(seat);
	cow_module_shm_buffers_destroy(buffers,
	    sizeof(buffers) / sizeof(buffers[0]));
	wl_display_disconnect(display);
	return 0;
}
