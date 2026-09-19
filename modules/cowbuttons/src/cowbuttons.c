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
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/wait.h>
#include <unistd.h>
#include <event2/event.h>
#include <wayland-client.h>

#include "cow/compat.h"
#include "cow/colour.h"
#include "cow/draw.h"
#include "cow/image.h"
#include "cow/memory.h"
#include "cow/module.h"
#include "cow/module_config.h"
#include "cow/module_cursor.h"
#include "cow/module_draw.h"
#include "cow/module_shm.h"
#include "cow/module_status.h"
#include "cow/text.h"
#include "cow/vector.h"
#include "xdg-shell-client-protocol.h"

#define PROG "cowbuttons"

#define MAX_CELLS 256
#define MAX_FIELDS 32
#define MAX_GRID 256
#define DEFAULT_CELL_W 96
#define DEFAULT_CELL_H 32
#define MIN_CELL_W 16
#define MIN_CELL_H 16
#define TEXT_PAD 6
#define BTN_LEFT 0x110
#define BTN_RIGHT 0x111
#define BTN_MIDDLE 0x112
#define DOUBLE_CLICK_MS 300
#define WL_SEAT_VERSION 4
#define DEFAULT_IMAGE_SIZE 16
#define MAX_BLOCKS 32
#define MAX_SELECTED_PANELS 32
#define PANEL_HANDLE_SIZE 22
#define DEFAULT_EXTERNAL_W 420
#define DEFAULT_EXTERNAL_H 120
#define EXTERNAL_PLACE_TRIES 20
#define EXTERNAL_PLACE_DELAY_US 50000
#define EXTERNAL_MOVE_TRIES 6
#define EXTERNAL_SYNC_MS 500
#define SHOW_INDEX_ENV "COWBUTTONS_SHOW_INDEX"

enum cell_type {
	CELL_EMPTY,
	CELL_BUTTON,
	CELL_SCRIPT,
	CELL_PANEL_HANDLE,
	CELL_EXTERNAL,
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

enum image_position {
	IMAGE_LEFT,
	IMAGE_CENTER,
	IMAGE_RIGHT,
	IMAGE_FILL,
};

enum text_justify {
	JUSTIFY_LEFT,
	JUSTIFY_CENTER,
	JUSTIFY_RIGHT,
};

enum panel_direction {
	PANEL_LEFT,
	PANEL_RIGHT,
	PANEL_UP,
	PANEL_DOWN,
};

enum click_button {
	CLICK_LEFT,
	CLICK_MIDDLE,
	CLICK_RIGHT,
	CLICK_BUTTON_COUNT,
};

enum click_count {
	CLICK_SINGLE,
	CLICK_DOUBLE,
	CLICK_COUNT,
};

enum block_type {
	BLOCK_ROW,
	BLOCK_IF,
	BLOCK_PANEL,
};

struct block {
	enum block_type type;
	bool		active;
	int		panel;
};

struct config {
	int  cell_w;
	int  cell_h;
	int  rows;
	int  cols;
	int  col_w[MAX_GRID];
	int  row_h[MAX_GRID];
	bool resizable;
	char font[128];
};

struct style {
	float bg[4];
	float bg_pressed[4];
	float fg[4];
	float fg_pressed[4];
	char *image;
	char *image_pressed;
	int   image_size;
	enum image_position image_pos;
	enum text_justify   justify;
};

struct cell_style {
	bool  bg_set;
	bool  bg_pressed_set;
	bool  fg_set;
	bool  fg_pressed_set;
	bool  image_size_set;
	bool  image_pos_set;
	bool  justify_set;
	bool  colspan_set;
	bool  rowspan_set;
	float bg[4];
	float bg_pressed[4];
	float fg[4];
	float fg_pressed[4];
	char *image;
	char *image_pressed;
	int   image_size;
	enum image_position image_pos;
	enum text_justify   justify;
	int   colspan;
	int   rowspan;
};

struct panel_selection {
	char		     *name;
	bool		      seen;
	bool		      open;
	bool		      mapped;
	bool		      directed;
	enum panel_direction  direction;
	struct cell_style     handle_style;
	char		     *external_command;
	char		     *external_app_id;
	int		      external_w;
	int		      external_h;
	char		      external_id[128];
	char		      external_rule[128];
	unsigned int	      external_rule_serial;
	pid_t		      external_pid;
};

struct win_info {
	char id[128];
	char app_id[128];
	char title[256];
	char display_title[256];
	int  x;
	int  y;
	int  width;
	int  height;
	int  border_width;
	int  title_thickness;
	char titlebar_position[32];
	double raise_seq;
};

struct cell {
	enum cell_type	  type;
	enum status_type  status;
	int		  row;
	int		  col;
	char		  label[256];
	char		  command[CLICK_BUTTON_COUNT][CLICK_COUNT][512];
	char		  script[512];
	int		  script_interval;
	pid_t		  script_pid;
	int		  script_fd;
	char		  script_output[256];
	size_t		  script_output_len;
	bool		  script_output_done;
	struct event	 *script_timer;
	struct event	 *script_read_event;
	enum panel_direction panel_dir;
	int		  panel;
	int		  target_panel;
	struct cell_style style;
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
static struct cow_module_cursor cursor;

static struct config cfg = {
	.cell_w = DEFAULT_CELL_W,
	.cell_h = DEFAULT_CELL_H,
	.rows = 1,
	.cols = 0,
	.font = "Sans 10",
};
static struct style style;
static struct cell cells[MAX_CELLS];
static int	   n_cells;
static int	   configured;
static int	   running = 1;
static int	   win_w;
static int	   win_h;
static int	   pressed_cell = -1;
static int	   pressed_button = -1;
static double	   ptr_x;
static double	   ptr_y;
static struct cow_module_shm_buffer buffers[2];
static int	   current_row = -1;
static int	   current_col;
static int	   current_panel = -1;
static bool	   in_row;
static struct block blocks[MAX_BLOCKS];
static int	   n_blocks;

static struct cow_module_status_stream status_stream;
static struct state    current_state;
static int	       dirty = 1;
static char	      *config_dir;
static struct cow_module_config_instances config_instances;
static char	       instance_title[256] = PROG;
static struct panel_selection selected_panels[MAX_SELECTED_PANELS];
static int	       n_selected_panels;
static struct event_base *script_event_base;
static struct event      *script_sigchld_event;
static struct event      *click_timer;
static int		 pending_click_cell = -1;
static int		 pending_click_button = -1;
static uint32_t		 pending_click_time;

static void render(void);
static int  cell_colspan(const struct cell *);
static int  cell_rowspan(const struct cell *);
static int  col_width(int);
static int  row_height(int);
static int  natural_width(void);
static int  natural_height(void);
static bool panel_any_closed(void);
static bool panel_finish(int, const char **);
static bool cell_hidden(const struct cell *);
static bool load_cell_props(struct cell *, char **, int, int, const char **);
static bool parse_cell_style_prop(struct cell_style *, const char *,
    const char *, const char **);
static void build_instance_title(void);
static bool external_panels_exist(void);
static void update_toplevel_size(void);
static bool script_cells_start(struct event_base *);
static void script_cells_stop(void);

static void
usage(void)
{
	fprintf(stderr, "usage: cowbuttons [-c path] [-h]\n");
}

static void
style_init(void)
{
	cow_colour_convert(style.bg, 0x6b6b73);
	cow_colour_convert(style.bg_pressed, 0x4a4a51);
	cow_colour_convert(style.fg, 0xf0f0f5);
	cow_colour_convert(style.fg_pressed, 0xffffff);
	style.image_size = DEFAULT_IMAGE_SIZE;
	style.image_pos = IMAGE_LEFT;
	style.justify = JUSTIFY_LEFT;
}

static int
parse_int(const char *s, int *out)
{
	return cow_module_config_parse_int(s, out);
}

static int
parse_bool(const char *s, bool *out)
{
	return cow_module_config_parse_bool(s, out);
}

static char *
path_dirname(const char *path)
{
	return cow_module_config_dirname(path);
}

static char *
resolve_path(const char *path)
{
	return cow_module_config_resolve_path(config_dir, path);
}

static char *
command_app_id(const char *cmd)
{
	const char *start = cmd;
	const char *end;
	const char *base;
	char *out;
	char quote = '\0';

	while (*start == ' ' || *start == '\t')
		start++;
	if (*start == '\'' || *start == '"')
		quote = *start++;
	end = start;
	while (*end != '\0') {
		if (quote != '\0') {
			if (*end == quote)
				break;
		} else if (*end == ' ' || *end == '\t')
			break;
		end++;
	}
	base = end;
	while (base > start && base[-1] != '/')
		base--;
	if (end == base)
		return NULL;
	xasprintf(&out, "%.*s", (int)(end - base), base);
	return out;
}

static void
free_fields(char **fields, int n)
{
	cow_module_config_free_fields(fields, n);
}

static int
parse_fields(const char *line, char **fields, int max_fields, char *err,
    size_t errsz)
{
	return cow_module_config_parse_fields(line, fields, max_fields, err,
	    errsz);
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

static int
image_position_parse(const char *s, enum image_position *out)
{
	if (strcmp(s, "left") == 0)
		*out = IMAGE_LEFT;
	else if (strcmp(s, "center") == 0)
		*out = IMAGE_CENTER;
	else if (strcmp(s, "right") == 0)
		*out = IMAGE_RIGHT;
	else if (strcmp(s, "fill") == 0)
		*out = IMAGE_FILL;
	else
		return 0;
	return 1;
}

static bool
justify_parse(const char *s, enum text_justify *out)
{
	if (strcmp(s, "left") == 0)
		*out = JUSTIFY_LEFT;
	else if (strcmp(s, "center") == 0)
		*out = JUSTIFY_CENTER;
	else if (strcmp(s, "right") == 0)
		*out = JUSTIFY_RIGHT;
	else
		return false;
	return true;
}

static bool
panel_direction_parse(const char *s, enum panel_direction *out)
{
	if (strcmp(s, "left") == 0)
		*out = PANEL_LEFT;
	else if (strcmp(s, "right") == 0)
		*out = PANEL_RIGHT;
	else if (strcmp(s, "up") == 0)
		*out = PANEL_UP;
	else if (strcmp(s, "down") == 0)
		*out = PANEL_DOWN;
	else
		return false;
	return true;
}

static bool
set_style_image(char **dst, const char *path)
{
	char *resolved = resolve_path(path);
	char *cause = NULL;

	if (resolved == NULL)
		return false;
	if (!cow_image_validate(resolved, &cause)) {
		fprintf(stderr, "%s: %s\n", PROG, cause);
		free(cause);
		free(resolved);
		return false;
	}
	free(*dst);
	*dst = resolved;
	return true;
}

static bool
command_exists(const char *name)
{
	const char *path = getenv("PATH");
	char *copy, *dir, *save = NULL;
	bool found = false;

	if (name == NULL || *name == '\0' || strchr(name, '/') != NULL)
		return false;
	if (path == NULL || *path == '\0')
		return false;
	copy = strdup(path);
	if (copy == NULL)
		return false;
	for (dir = strtok_r(copy, ":", &save); dir != NULL;
	    dir = strtok_r(NULL, ":", &save)) {
		char *candidate;
		if (*dir == '\0')
			dir = ".";
		if (asprintf(&candidate, "%s/%s", dir, name) < 0)
			continue;
		if (access(candidate, X_OK) == 0)
			found = true;
		free(candidate);
		if (found)
			break;
	}
	free(copy);
	return found;
}

static bool
file_exists(const char *path)
{
	char *resolved = resolve_path(path);
	bool exists;

	if (resolved == NULL)
		return false;
	exists = access(resolved, F_OK) == 0;
	free(resolved);
	return exists;
}

static bool
env_exists(const char *name)
{
	const char *value = getenv(name);

	return value != NULL && *value != '\0';
}

static bool
blocks_active(void)
{
	for (int i = 0; i < n_blocks; i++)
		if (!blocks[i].active)
			return false;
	return true;
}

static int
panel_selection_find(const char *name)
{
	for (int i = 0; i < n_selected_panels; i++)
		if (strcmp(selected_panels[i].name, name) == 0)
			return i;
	return -1;
}

static bool
panel_selection_add(const char *name)
{
	if (panel_selection_find(name) != -1)
		return true;
	if (n_selected_panels >= MAX_SELECTED_PANELS)
		return false;
	selected_panels[n_selected_panels].name = strdup(name);
	if (selected_panels[n_selected_panels].name == NULL)
		return false;
	selected_panels[n_selected_panels].seen = false;
	selected_panels[n_selected_panels].open = true;
	selected_panels[n_selected_panels].mapped = true;
	selected_panels[n_selected_panels].directed = false;
	selected_panels[n_selected_panels].direction = PANEL_RIGHT;
	selected_panels[n_selected_panels].external_pid = -1;
	n_selected_panels++;
	return true;
}

static bool
inside_active_panel(void)
{
	for (int i = 0; i < n_blocks; i++)
		if (blocks[i].type == BLOCK_PANEL && blocks[i].active)
			return true;
	return false;
}

static bool
block_push(enum block_type type, bool active, const char **why)
{
	if (n_blocks >= MAX_BLOCKS) {
		*why = "too many nested blocks";
		return false;
	}
	blocks[n_blocks++] = (struct block) {
		.type = type,
		.active = active,
		.panel = type == BLOCK_PANEL ? current_panel : -1,
	};
	return true;
}

static bool
block_pop(const char **why)
{
	if (n_blocks == 0) {
		*why = "bad block close";
		return false;
	}
	struct block b = blocks[--n_blocks];
	if (b.type == BLOCK_ROW && b.active) {
		in_row = false;
		current_col = 0;
	} else if (b.type == BLOCK_PANEL) {
		if (b.active && !panel_finish(b.panel, why))
			return false;
		current_panel = -1;
	}
	return true;
}

static bool
load_if(char **fields, int n, const char **why)
{
	bool negated = false;
	int type_idx = 1;

	if (n != 4 && n != 5) {
		*why = "bad if";
		return false;
	}
	if (strcmp(fields[n - 1], "{") != 0) {
		*why = "bad if";
		return false;
	}
	if (strcmp(fields[1], "not") == 0) {
		negated = true;
		type_idx = 2;
		if (n != 5) {
			*why = "bad if";
			return false;
		}
	} else if (n != 4) {
		*why = "bad if";
		return false;
	}

	bool parent_active = blocks_active();
	bool ok = false;
	if (!parent_active)
		ok = false;
	else if (strcmp(fields[type_idx], "command") == 0)
		ok = command_exists(fields[type_idx + 1]);
	else if (strcmp(fields[type_idx], "file") == 0)
		ok = file_exists(fields[type_idx + 1]);
	else if (strcmp(fields[type_idx], "env") == 0)
		ok = env_exists(fields[type_idx + 1]);
	else {
		*why = "bad if";
		return false;
	}
	if (negated)
		ok = !ok;
	return block_push(BLOCK_IF, parent_active && ok, why);
}

static bool
load_panel(char **fields, int n, const char **why)
{
	if (n < 3 || strcmp(fields[n - 1], "{") != 0) {
		*why = "bad panel";
		return false;
	}
	if (n_blocks != 0) {
		*why = "nested panel";
		return false;
	}
	int idx = panel_selection_find(fields[1]);
	bool directed = false;
	enum panel_direction direction = PANEL_RIGHT;

	if (n > 3) {
		if (!panel_direction_parse(fields[2], &direction)) {
			*why = "bad panel direction";
			return false;
		}
		if (((n - 4) % 2) != 0) {
			*why = "bad panel property";
			return false;
		}
		directed = true;
	}
	bool active = idx != -1 && blocks_active();
	current_panel = active ? idx : -1;
	if (idx != -1) {
		selected_panels[idx].seen = true;
		selected_panels[idx].directed = directed;
		selected_panels[idx].direction = direction;
		for (int i = 3; directed && i < n - 1; i += 2) {
			if (!parse_cell_style_prop(&selected_panels[idx].
			    handle_style, fields[i], fields[i + 1], why))
				return false;
		}
	}
	return block_push(BLOCK_PANEL, active, why);
}

static bool
load_show(char **fields, int n, const char **why, bool collect)
{
	if (n < 3 || strcmp(fields[1], "panel") != 0) {
		*why = "bad show";
		return false;
	}
	if (!collect)
		return true;
	for (int i = 2; i < n; i++) {
		if (!panel_selection_add(fields[i])) {
			*why = "too many panels";
			return false;
		}
	}
	return true;
}

static struct cell *
cell_find(int row, int col)
{
	for (int i = 0; i < n_cells; i++)
		if (cells[i].row == row && cells[i].col == col)
			return &cells[i];
	return NULL;
}

static struct cell *
cell_get(int row, int col)
{
	struct cell *c = cell_find(row, col);

	if (c != NULL)
		return c;
	if (n_cells >= MAX_CELLS)
		return NULL;
	c = &cells[n_cells++];
	memset(c, 0, sizeof(*c));
	c->row = row;
	c->col = col;
	c->panel = current_panel;
	c->target_panel = -1;
	c->script_pid = -1;
	c->script_fd = -1;
	if (row + 1 > cfg.rows)
		cfg.rows = row + 1;
	if (col + 1 > cfg.cols)
		cfg.cols = col + 1;
	return c;
}

static bool
parse_cell_style_prop(struct cell_style *st, const char *prop, const char *val,
    const char **why)
{
	int v;

	if (strcmp(prop, "bg") == 0) {
		if (!cow_colour_parse(val, st->bg)) {
			*why = "bad bg";
			return false;
		}
		st->bg_set = true;
	} else if (strcmp(prop, "bg_pressed") == 0) {
		if (!cow_colour_parse(val, st->bg_pressed)) {
			*why = "bad bg_pressed";
			return false;
		}
		st->bg_pressed_set = true;
	} else if (strcmp(prop, "fg") == 0) {
		if (!cow_colour_parse(val, st->fg)) {
			*why = "bad fg";
			return false;
		}
		st->fg_set = true;
	} else if (strcmp(prop, "fg_pressed") == 0) {
		if (!cow_colour_parse(val, st->fg_pressed)) {
			*why = "bad fg_pressed";
			return false;
		}
		st->fg_pressed_set = true;
	} else if (strcmp(prop, "image") == 0) {
		if (!set_style_image(&st->image, val)) {
			*why = "bad image";
			return false;
		}
	} else if (strcmp(prop, "image_pressed") == 0) {
		if (!set_style_image(&st->image_pressed, val)) {
			*why = "bad image_pressed";
			return false;
		}
	} else if (strcmp(prop, "image_size") == 0) {
		if (!parse_int(val, &v) || v < 1) {
			*why = "bad image_size";
			return false;
		}
		st->image_size = v;
		st->image_size_set = true;
	} else if (strcmp(prop, "image_position") == 0) {
		if (!image_position_parse(val, &st->image_pos)) {
			*why = "bad image_position";
			return false;
		}
		st->image_pos_set = true;
	} else if (strcmp(prop, "justify") == 0) {
		if (!justify_parse(val, &st->justify)) {
			*why = "bad justify";
			return false;
		}
		st->justify_set = true;
	} else if (strcmp(prop, "colspan") == 0) {
		if (!parse_int(val, &v) || v < 1) {
			*why = "bad colspan";
			return false;
		}
		st->colspan = v;
		st->colspan_set = true;
	} else if (strcmp(prop, "rowspan") == 0) {
		if (!parse_int(val, &v) || v < 1) {
			*why = "bad rowspan";
			return false;
		}
		st->rowspan = v;
		st->rowspan_set = true;
	} else {
		*why = "unknown cell style";
		return false;
	}
	return true;
}

static bool
load_style(char **fields, int n, const char **why)
{
	int v;

	if (n < 3) {
		*why = "bad style";
		return false;
	}
	if (strcmp(fields[1], "button.bg") == 0) {
		if (n != 3 || !cow_colour_parse(fields[2], style.bg)) {
			*why = "bad button.bg";
			return false;
		}
	} else if (strcmp(fields[1], "button.bg_pressed") == 0) {
		if (n != 3 || !cow_colour_parse(fields[2], style.bg_pressed)) {
			*why = "bad button.bg_pressed";
			return false;
		}
	} else if (strcmp(fields[1], "button.fg") == 0) {
		if (n != 3 || !cow_colour_parse(fields[2], style.fg)) {
			*why = "bad button.fg";
			return false;
		}
	} else if (strcmp(fields[1], "button.fg_pressed") == 0) {
		if (n != 3 || !cow_colour_parse(fields[2], style.fg_pressed)) {
			*why = "bad button.fg_pressed";
			return false;
		}
	} else if (strcmp(fields[1], "button.image") == 0) {
		if (n != 3 || !set_style_image(&style.image, fields[2])) {
			*why = "bad button.image";
			return false;
		}
	} else if (strcmp(fields[1], "button.image_pressed") == 0) {
		if (n != 3 ||
		    !set_style_image(&style.image_pressed, fields[2])) {
			*why = "bad button.image_pressed";
			return false;
		}
	} else if (strcmp(fields[1], "button.image_size") == 0) {
		if (n != 3 || !parse_int(fields[2], &v) || v < 1) {
			*why = "bad button.image_size";
			return false;
		}
		style.image_size = v;
	} else if (strcmp(fields[1], "button.image_position") == 0) {
		if (n != 3 || !image_position_parse(fields[2],
		    &style.image_pos)) {
			*why = "bad button.image_position";
			return false;
		}
	} else if (strcmp(fields[1], "button.justify") == 0) {
		if (n != 3 || !justify_parse(fields[2], &style.justify)) {
			*why = "bad button.justify";
			return false;
		}
	} else {
		*why = "unknown style";
		return false;
	}
	return true;
}

static bool
grid_cell_taken(int row, int col)
{
	for (int i = 0; i < n_cells; i++) {
		struct cell *c = &cells[i];
		if (row >= c->row && row < c->row + cell_rowspan(c) &&
		    col >= c->col && col < c->col + cell_colspan(c))
			return true;
	}
	return false;
}

static void
row_begin(void)
{
	current_row++;
	current_col = 0;
	in_row = true;
	if (current_row + 1 > cfg.rows)
		cfg.rows = current_row + 1;
}

static struct cell *
cell_append(void)
{
	if (!in_row)
		return NULL;
	while (current_col < MAX_GRID &&
	    grid_cell_taken(current_row, current_col))
		current_col++;
	if (current_col >= MAX_GRID)
		return NULL;
	return cell_get(current_row, current_col);
}

static struct cell *
cell_append_block(void)
{
	struct cell *c;

	current_row++;
	current_col = 0;
	if (current_row + 1 > cfg.rows)
		cfg.rows = current_row + 1;
	c = cell_get(current_row, 0);
	if (c != NULL)
		current_col = c->col + cell_colspan(c);
	return c;
}

static bool
parse_span(struct cell *c, char *val, const char **why)
{
	char *x;
	int col_span, row_span;

	if (strcmp(val, "fill") == 0) {
		int span = cfg.cols > c->col ? cfg.cols - c->col : 1;
		c->style.colspan = span;
		c->style.colspan_set = true;
		return true;
	}
	x = strchr(val, 'x');
	if (x == NULL)
		return parse_cell_style_prop(&c->style, "colspan", val, why);

	*x = '\0';
	if (!parse_int(val, &col_span) || !parse_int(x + 1, &row_span) ||
	    col_span < 1 || row_span < 1) {
		*why = "bad span";
		*x = 'x';
		return false;
	}
	*x = 'x';
	c->style.colspan = col_span;
	c->style.rowspan = row_span;
	c->style.colspan_set = true;
	c->style.rowspan_set = true;
	return true;
}

static bool
load_external(char **fields, int n, const char **why)
{
	struct cell *c;

	if (current_panel < 0) {
		*why = "external outside panel";
		return false;
	}
	if (n < 2) {
		*why = "bad external";
		return false;
	}
	if (selected_panels[current_panel].external_command != NULL) {
		*why = "duplicate external";
		return false;
	}
	c = cell_append_block();
	if (c == NULL) {
		*why = "too many cells";
		return false;
	}
	c->type = CELL_EXTERNAL;
	strlcpy(c->label, fields[1], sizeof(c->label));
	c->style.colspan = 1;
	c->style.colspan_set = true;
	c->style.rowspan = 1;
	c->style.rowspan_set = true;
	cfg.col_w[c->col] = DEFAULT_EXTERNAL_W;
	cfg.row_h[c->row] = DEFAULT_EXTERNAL_H;
	if (!load_cell_props(c, fields, 2, n, why))
		return false;
	selected_panels[current_panel].external_command = xstrdup(fields[1]);
	selected_panels[current_panel].external_app_id = command_app_id(fields[1]);
	if (selected_panels[current_panel].external_app_id == NULL)
		selected_panels[current_panel].external_app_id = xstrdup(fields[1]);
	selected_panels[current_panel].external_w = cfg.col_w[c->col];
	selected_panels[current_panel].external_h = cfg.row_h[c->row];
	selected_panels[current_panel].open = false;
	selected_panels[current_panel].mapped = false;
	return true;
}

static bool
click_property(const char *prop, enum click_button *button,
    enum click_count *count)
{
	if (strcmp(prop, "click") == 0 || strcmp(prop, "leftclick") == 0) {
		*button = CLICK_LEFT;
		*count = CLICK_SINGLE;
	} else if (strcmp(prop, "doubleclick") == 0 ||
	    strcmp(prop, "leftdoubleclick") == 0) {
		*button = CLICK_LEFT;
		*count = CLICK_DOUBLE;
	} else if (strcmp(prop, "middleclick") == 0) {
		*button = CLICK_MIDDLE;
		*count = CLICK_SINGLE;
	} else if (strcmp(prop, "middledoubleclick") == 0) {
		*button = CLICK_MIDDLE;
		*count = CLICK_DOUBLE;
	} else if (strcmp(prop, "rightclick") == 0) {
		*button = CLICK_RIGHT;
		*count = CLICK_SINGLE;
	} else if (strcmp(prop, "rightdoubleclick") == 0) {
		*button = CLICK_RIGHT;
		*count = CLICK_DOUBLE;
	} else {
		return false;
	}

	return true;
}

static bool
load_cell_props(struct cell *c, char **fields, int start, int n,
    const char **why)
{
	int v;

	if (((n - start) % 2) != 0) {
		*why = "bad cell property";
		return false;
	}
	for (int i = start; i < n; i += 2) {
		const char *prop = fields[i];
		const char *val = fields[i + 1];
		enum click_button button;
		enum click_count count;

		if (click_property(prop, &button, &count)) {
			if (c->type != CELL_BUTTON && c->type != CELL_SCRIPT) {
				*why = "click action on non-clickable cell";
				return false;
			}
			strlcpy(c->command[button][count], val,
			    sizeof(c->command[button][count]));
		} else if (strcmp(prop, "width") == 0) {
			if (!parse_int(val, &v) || v < MIN_CELL_W) {
				*why = "bad width";
				return false;
			}
			cfg.col_w[c->col] = v;
		} else if (strcmp(prop, "height") == 0) {
			if (!parse_int(val, &v) || v < MIN_CELL_H) {
				*why = "bad height";
				return false;
			}
			cfg.row_h[c->row] = v;
		} else if (strcmp(prop, "span") == 0) {
			if (!parse_span(c, fields[i + 1], why))
				return false;
		} else if (!parse_cell_style_prop(&c->style, prop, val, why))
			return false;
	}
	if (c->row + cell_rowspan(c) > cfg.rows)
		cfg.rows = c->row + cell_rowspan(c);
	if (c->col + cell_colspan(c) > cfg.cols)
		cfg.cols = c->col + cell_colspan(c);
	return true;
}

static bool
load_script(char **fields, int n, const char **why)
{
	struct cell *c;
	bool interval_set = false;

	if (n < 4 || ((n - 2) % 2) != 0) {
		*why = "bad script";
		return false;
	}
	c = cell_append();
	if (c == NULL) {
		*why = in_row ? "too many cells" : "script outside row";
		return false;
	}
	c->type = CELL_SCRIPT;
	strlcpy(c->script, fields[1], sizeof(c->script));

	for (int i = 2; i < n; i += 2) {
		if (strcmp(fields[i], "interval") == 0) {
			int interval;

			if (!parse_int(fields[i + 1], &interval) || interval < 1) {
				*why = "bad script interval";
				return false;
			}
			c->script_interval = interval;
			interval_set = true;
		} else if (!load_cell_props(c, fields, i, i + 2, why))
			return false;
	}
	if (!interval_set) {
		*why = "script requires interval";
		return false;
	}
	current_col = c->col + cell_colspan(c);
	return true;
}

static bool
panel_bounds(int panel, int *min_row, int *max_row, int *min_col,
    int *max_col)
{
	bool found = false;

	for (int i = 0; i < n_cells; i++) {
		const struct cell *c = &cells[i];

		if (c->panel != panel || c->type == CELL_PANEL_HANDLE)
			continue;
		if (!found) {
			*min_row = c->row;
			*max_row = c->row + cell_rowspan(c);
			*min_col = c->col;
			*max_col = c->col + cell_colspan(c);
			found = true;
			continue;
		}
		if (c->row < *min_row)
			*min_row = c->row;
		if (c->row + cell_rowspan(c) > *max_row)
			*max_row = c->row + cell_rowspan(c);
		if (c->col < *min_col)
			*min_col = c->col;
		if (c->col + cell_colspan(c) > *max_col)
			*max_col = c->col + cell_colspan(c);
	}
	return found;
}

static void
panel_shift(int panel, int row_delta, int col_delta)
{
	for (int i = 0; i < n_cells; i++) {
		struct cell *c = &cells[i];

		if (c->panel != panel || c->type == CELL_PANEL_HANDLE)
			continue;
		c->row += row_delta;
		c->col += col_delta;
		if (c->row + cell_rowspan(c) > cfg.rows)
			cfg.rows = c->row + cell_rowspan(c);
		if (c->col + cell_colspan(c) > cfg.cols)
			cfg.cols = c->col + cell_colspan(c);
	}
}

static struct cell *
panel_handle_add(int row, int col, int rowspan, int colspan, int panel)
{
	struct cell *c;

	if (n_cells >= MAX_CELLS)
		return NULL;
	c = &cells[n_cells++];
	memset(c, 0, sizeof(*c));
	c->type = CELL_PANEL_HANDLE;
	c->row = row;
	c->col = col;
	c->panel = panel;
	c->target_panel = panel;
	c->panel_dir = selected_panels[panel].direction;
	c->style = selected_panels[panel].handle_style;
	c->style.rowspan = rowspan;
	c->style.rowspan_set = true;
	c->style.colspan = colspan;
	c->style.colspan_set = true;
	if (row + rowspan > cfg.rows)
		cfg.rows = row + rowspan;
	if (col + colspan > cfg.cols)
		cfg.cols = col + colspan;
	return c;
}

static bool
panel_finish(int panel, const char **why)
{
	int min_row, max_row, min_col, max_col;
	struct panel_selection *p;
	struct cell *handle;

	if (panel < 0)
		return true;
	p = &selected_panels[panel];
	if (!p->directed)
		return true;
	if (!panel_bounds(panel, &min_row, &max_row, &min_col, &max_col))
		return true;

	switch (p->direction) {
	case PANEL_LEFT:
		handle = panel_handle_add(min_row, max_col,
		    max_row - min_row, 1, panel);
		if (handle != NULL)
			cfg.col_w[handle->col] = PANEL_HANDLE_SIZE;
		break;
	case PANEL_RIGHT:
		panel_shift(panel, 0, 1);
		if (!panel_bounds(panel, &min_row, &max_row, &min_col,
		    &max_col))
			return true;
		handle = panel_handle_add(min_row, min_col - 1,
		    max_row - min_row, 1, panel);
		if (handle != NULL)
			cfg.col_w[handle->col] = PANEL_HANDLE_SIZE;
		break;
	case PANEL_UP:
		handle = panel_handle_add(max_row, min_col, 1,
		    max_col - min_col, panel);
		if (handle != NULL)
			cfg.row_h[handle->row] = PANEL_HANDLE_SIZE;
		break;
	case PANEL_DOWN:
		panel_shift(panel, 1, 0);
		if (!panel_bounds(panel, &min_row, &max_row, &min_col,
		    &max_col))
			return true;
		handle = panel_handle_add(min_row - 1, min_col, 1,
		    max_col - min_col, panel);
		if (handle != NULL)
			cfg.row_h[handle->row] = PANEL_HANDLE_SIZE;
		break;
	}
	if (handle == NULL) {
		*why = "too many cells";
		return false;
	}
	return true;
}

static bool
load_row(char **fields, int n, const char **why)
{
	int v;
	bool parent_active = blocks_active();

	if (n_selected_panels != 0 && !inside_active_panel())
		parent_active = false;
	if (n != 2 && n != 4) {
		*why = "bad row";
		return false;
	}
	if (strcmp(fields[n - 1], "{") != 0) {
		*why = "bad row";
		return false;
	}
	if (parent_active && in_row) {
		*why = "nested row";
		return false;
	}
	if (n == 4) {
		if (strcmp(fields[1], "height") != 0 ||
		    !parse_int(fields[2], &v) || v < MIN_CELL_H) {
			*why = "bad row height";
			return false;
		}
	}
	if (!block_push(BLOCK_ROW, parent_active, why))
		return false;
	if (!parent_active)
		return true;
	row_begin();
	if (n == 4)
		cfg.row_h[current_row] = v;
	return true;
}

static int
scan_selected_panels(const char *path)
{
	FILE *f;
	char *line = NULL;
	size_t cap = 0;
	unsigned int lineno = 0;
	int depth = 0;
	bool selected = false;

	f = fopen(path, "r");
	if (f == NULL) {
		fprintf(stderr, "%s: %s: %s\n", PROG, path, strerror(errno));
		return 0;
	}
	config_instances.count = 0;
	while (getline(&line, &cap, f) != -1) {
		char *fields[MAX_FIELDS] = { 0 };
		char err[128] = "";
		const char *why = NULL;
		int n;

		lineno++;
		n = parse_fields(line, fields, MAX_FIELDS, err, sizeof(err));
		if (n < 0) {
			fprintf(stderr, "%s: %s:%u: %s\n", PROG, path, lineno,
			    err);
			free(line);
			fclose(f);
			return 0;
		}
		if (n == 0) {
			free_fields(fields, n);
			continue;
		}
		if (depth == 0 && strcmp(fields[0], "show") == 0) {
			bool collect =
			    cow_module_config_instances_select_next(&config_instances);

			if (!load_show(fields, n, &why, collect)) {
				fprintf(stderr, "%s: %s:%u: %s\n", PROG, path,
				    lineno, why);
				free_fields(fields, n);
				free(line);
				fclose(f);
				return 0;
			}
			if (collect)
				selected = true;
		}
		if (strcmp(fields[0], "}") == 0 && n == 1) {
			if (depth > 0)
				depth--;
		} else if (strcmp(fields[n - 1], "{") == 0)
			depth++;
		free_fields(fields, n);
	}
	free(line);
	fclose(f);
	if (config_instances.count == 0) {
		fprintf(stderr, "%s: %s: no show panel lines\n", PROG, path);
		return 0;
	}
	if (!selected) {
		fprintf(stderr, "%s: %s: no show panel line %d\n", PROG, path,
		    config_instances.index);
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
	free(config_dir);
	config_dir = path_dirname(path);
	current_row = -1;
	current_col = 0;
	in_row = false;
	n_blocks = 0;
	if (!scan_selected_panels(path)) {
		fclose(f);
		return 0;
	}

	while (getline(&line, &cap, f) != -1) {
		char *fields[MAX_FIELDS] = { 0 };
		char err[128] = "";
		const char *why = NULL;
		int n, v;

		lineno++;
		n = parse_fields(line, fields, MAX_FIELDS, err, sizeof(err));
		if (n < 0) {
			fprintf(stderr, "%s: %s:%u: %s\n", PROG, path, lineno,
			    err);
			continue;
		}
		if (n == 0)
			continue;

		if (strcmp(fields[0], "}") == 0) {
			if (n != 1 || !block_pop(&why))
				goto line_error;
		} else if (strcmp(fields[0], "if") == 0) {
			if (!load_if(fields, n, &why))
				goto line_error;
		} else if (strcmp(fields[0], "panel") == 0) {
			if (!load_panel(fields, n, &why))
				goto line_error;
		} else if (strcmp(fields[0], "show") == 0) {
			if (!load_show(fields, n, &why, false))
				goto line_error;
		} else if (strcmp(fields[0], "row") == 0) {
			if (!load_row(fields, n, &why))
				goto line_error;
		} else if (!blocks_active()) {
		} else if (strcmp(fields[0], "cell") == 0) {
			int h;

			if (n != 3 || !parse_int(fields[1], &v) ||
			    !parse_int(fields[2], &h) || v < MIN_CELL_W ||
			    h < MIN_CELL_H) {
				why = "bad cell";
				goto line_error;
			}
			cfg.cell_w = v;
			cfg.cell_h = h;
		} else if (strcmp(fields[0], "col") == 0) {
			int idx;

			if (n != 4 || !parse_int(fields[1], &idx) ||
			    strcmp(fields[2], "width") != 0 ||
			    !parse_int(fields[3], &v) || idx < 0 ||
			    idx >= MAX_GRID || v < MIN_CELL_W) {
				why = "bad col";
				goto line_error;
			}
			cfg.col_w[idx] = v;
			if (idx + 1 > cfg.cols)
				cfg.cols = idx + 1;
		} else if (strcmp(fields[0], "grid") == 0) {
			int cols;

			if (n != 3 || !parse_int(fields[1], &v) ||
			    !parse_int(fields[2], &cols) || v < 1 ||
			    cols < 1 || v > MAX_GRID || cols > MAX_GRID) {
				why = "bad grid";
				goto line_error;
			}
			cfg.rows = v;
			cfg.cols = cols;
		} else if (strcmp(fields[0], "font") == 0) {
			if (n != 2) {
				why = "bad font";
				goto line_error;
			}
			strlcpy(cfg.font, fields[1], sizeof(cfg.font));
		} else if (strcmp(fields[0], "resizable") == 0) {
			if (n != 2 || !parse_bool(fields[1], &cfg.resizable)) {
				why = "bad resizable";
				goto line_error;
			}
		} else if (strcmp(fields[0], "style") == 0) {
			if (!load_style(fields, n, &why))
				goto line_error;
		} else if (strcmp(fields[0], "external") == 0) {
			if (!load_external(fields, n, &why))
				goto line_error;
		} else if (strcmp(fields[0], "button") == 0) {
			if (n < 3) {
				why = "bad button";
				goto line_error;
			}
			struct cell *c = cell_append();
			if (c == NULL) {
				why = in_row ? "too many cells" : "button outside row";
				goto line_error;
			}
			c->type = CELL_BUTTON;
			strlcpy(c->label, fields[1], sizeof(c->label));
			strlcpy(c->command[CLICK_LEFT][CLICK_SINGLE], fields[2],
			    sizeof(c->command[CLICK_LEFT][CLICK_SINGLE]));
			if (!load_cell_props(c, fields, 3, n, &why))
				goto line_error;
			current_col = c->col + cell_colspan(c);
		} else if (strcmp(fields[0], "script") == 0) {
			if (!load_script(fields, n, &why))
				goto line_error;
		} else if (strcmp(fields[0], "status") == 0) {
			if (n < 2) {
				why = "bad status";
				goto line_error;
			}
			enum status_type type;
			if (!status_type_parse(fields[1], &type)) {
				why = "bad status type";
				goto line_error;
			}
			struct cell *c = cell_append();
			if (c == NULL) {
				why = in_row ? "too many cells" : "status outside row";
				goto line_error;
			}
			c->type = CELL_STATUS;
			c->status = type;
			if (!load_cell_props(c, fields, 2, n, &why))
				goto line_error;
			current_col = c->col + cell_colspan(c);
		} else if (strcmp(fields[0], "spacer") == 0) {
			struct cell *c = cell_append();
			if (c == NULL) {
				why = in_row ? "too many cells" : "spacer outside row";
				goto line_error;
			}
			c->type = CELL_SPACER;
			if (!load_cell_props(c, fields, 1, n, &why))
				goto line_error;
			current_col = c->col + cell_colspan(c);
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
	if (n_blocks != 0) {
		fprintf(stderr, "%s: %s:%u: unclosed block\n", PROG, path,
		    lineno);
		return 0;
	}
	for (int i = 0; i < n_selected_panels; i++) {
		if (!selected_panels[i].seen) {
			fprintf(stderr, "%s: %s: panel not found: %s\n", PROG,
			    path, selected_panels[i].name);
			return 0;
		}
	}
	if (cfg.cols < 1)
		cfg.cols = 1;
	if (cfg.rows < 1)
		cfg.rows = 1;
	build_instance_title();
	return 1;
}

static char *
default_config_path(void)
{
	return cow_module_config_default_path(PROG);
}

static void
build_instance_title(void)
{
	size_t len;

	if (n_selected_panels == 0) {
		strlcpy(instance_title, PROG, sizeof(instance_title));
		return;
	}
	snprintf(instance_title, sizeof(instance_title), "%s:%s", PROG,
	    selected_panels[0].name);
	len = strlen(instance_title);
	for (int i = 1; i < n_selected_panels; i++) {
		int n = snprintf(instance_title + len, sizeof(instance_title) - len,
		    "+%s", selected_panels[i].name);
		if (n < 0 || (size_t)n >= sizeof(instance_title) - len)
			break;
		len += (size_t)n;
	}
}

static const struct cell *
cell_at_grid(int row, int col)
{
	for (int i = 0; i < n_cells; i++)
		if (!cell_hidden(&cells[i]) && cells[i].row == row &&
		    cells[i].col == col)
			return &cells[i];
	return NULL;
}

static int
cell_colspan(const struct cell *c)
{
	return c->style.colspan_set ? c->style.colspan : 1;
}

static int
cell_rowspan(const struct cell *c)
{
	return c->style.rowspan_set ? c->style.rowspan : 1;
}

static bool
cell_hidden(const struct cell *c)
{
	if (c->type == CELL_EXTERNAL)
		return true;
	return c->panel >= 0 && c->type != CELL_PANEL_HANDLE &&
	    !selected_panels[c->panel].open;
}

static bool
col_hidden(int col)
{
	bool found = false;

	for (int i = 0; i < n_cells; i++) {
		const struct cell *c = &cells[i];

		if (col < c->col || col >= c->col + cell_colspan(c))
			continue;
		found = true;
		if (!cell_hidden(c))
			return false;
	}
	return found;
}

static bool
row_hidden(int row)
{
	bool found = false;

	for (int i = 0; i < n_cells; i++) {
		const struct cell *c = &cells[i];

		if (row < c->row || row >= c->row + cell_rowspan(c))
			continue;
		found = true;
		if (!cell_hidden(c))
			return false;
	}
	return found;
}

static int
base_col_width(int col)
{
	if (col_hidden(col))
		return 0;
	if (col >= 0 && col < MAX_GRID && cfg.col_w[col] > 0)
		return cfg.col_w[col];
	return cfg.cell_w;
}

static int
base_row_height(int row)
{
	if (row_hidden(row))
		return 0;
	if (row >= 0 && row < MAX_GRID && cfg.row_h[row] > 0)
		return cfg.row_h[row];
	return cfg.cell_h;
}

static int
base_cols_width(int col, int span)
{
	int w = 0;

	for (int i = 0; i < span; i++)
		w += base_col_width(col + i);
	return w;
}

static int
base_rows_height(int row, int span)
{
	int h = 0;

	for (int i = 0; i < span; i++)
		h += base_row_height(row + i);
	return h;
}

static int
natural_width(void)
{
	return base_cols_width(0, cfg.cols);
}

static int
natural_height(void)
{
	return base_rows_height(0, cfg.rows);
}

static bool
panel_any_closed(void)
{
	for (int i = 0; i < n_selected_panels; i++)
		if (selected_panels[i].directed && !selected_panels[i].open)
			return true;
	return false;
}

static int
target_width(void)
{
	int w = natural_width();

	if (panel_any_closed())
		return w;
	if (cfg.resizable && win_w > w)
		return win_w;
	return w;
}

static int
target_height(void)
{
	int h = natural_height();

	if (panel_any_closed())
		return h;
	if (cfg.resizable && win_h > h)
		return win_h;
	return h;
}

static int
scale_offset(int pos, int base_total, int target_total)
{
	if (base_total <= 0 || target_total <= base_total)
		return pos;
	return (int)((long long)pos * target_total / base_total);
}

static bool
col_fixed(int col)
{
	for (int i = 0; i < n_cells; i++) {
		const struct cell *c = &cells[i];

		if (cell_hidden(c) || c->type != CELL_PANEL_HANDLE)
			continue;
		if (c->col == col && cell_colspan(c) == 1 &&
		    (c->panel_dir == PANEL_LEFT || c->panel_dir == PANEL_RIGHT))
			return true;
	}
	return false;
}

static bool
row_fixed(int row)
{
	for (int i = 0; i < n_cells; i++) {
		const struct cell *c = &cells[i];

		if (cell_hidden(c) || c->type != CELL_PANEL_HANDLE)
			continue;
		if (c->row == row && cell_rowspan(c) == 1 &&
		    (c->panel_dir == PANEL_UP || c->panel_dir == PANEL_DOWN))
			return true;
	}
	return false;
}

static int
target_col_width(int col)
{
	int base = base_col_width(col);
	int extra = target_width() - natural_width();
	int flex = 0;

	if (extra <= 0 || col_fixed(col))
		return base;
	for (int i = 0; i < cfg.cols; i++)
		if (!col_fixed(i))
			flex += base_col_width(i);
	if (flex <= 0)
		return base;
	return scale_offset(base, flex, flex + extra);
}

static int
target_row_height(int row)
{
	int base = base_row_height(row);
	int extra = target_height() - natural_height();
	int flex = 0;

	if (extra <= 0 || row_fixed(row))
		return base;
	for (int i = 0; i < cfg.rows; i++)
		if (!row_fixed(i))
			flex += base_row_height(i);
	if (flex <= 0)
		return base;
	return scale_offset(base, flex, flex + extra);
}

static int
col_offset(int col)
{
	int x = 0;

	for (int i = 0; i < col; i++)
		x += target_col_width(i);
	return x;
}

static int
row_offset(int row)
{
	int y = 0;

	for (int i = 0; i < row; i++)
		y += target_row_height(i);
	return y;
}

static int
cols_width(int col, int span)
{
	return col_offset(col + span) - col_offset(col);
}

static int
rows_height(int row, int span)
{
	return row_offset(row + span) - row_offset(row);
}

static int
col_width(int col)
{
	return cols_width(col, 1);
}

static int
row_height(int row)
{
	return rows_height(row, 1);
}

static int
col_at(double x)
{
	int pos = 0;

	for (int col = 0; col < cfg.cols; col++) {
		pos += col_width(col);
		if (x < pos)
			return col;
	}
	return -1;
}

static int
row_at(double y)
{
	int pos = 0;

	for (int row = 0; row < cfg.rows; row++) {
		pos += row_height(row);
		if (y < pos)
			return row;
	}
	return -1;
}

static bool
cell_covered_by_span(const struct cell *c, int row, int col)
{
	return row >= c->row && row < c->row + cell_rowspan(c) &&
	    col >= c->col && col < c->col + cell_colspan(c);
}

static bool
grid_covered_by_span(int row, int col)
{
	for (int i = 0; i < n_cells; i++) {
		const struct cell *c = &cells[i];

		if (c->row == row && c->col == col)
			continue;
		if (!cell_hidden(c) && cell_covered_by_span(c, row, col))
			return true;
	}
	return false;
}

static int
cell_at_point(double x, double y)
{
	int col = col_at(x);
	int row = row_at(y);

	if (row < 0 || col < 0 || row >= cfg.rows || col >= cfg.cols)
		return -1;
	for (int i = 0; i < n_cells; i++)
		if (!cell_hidden(&cells[i]) &&
		    cell_covered_by_span(&cells[i], row, col))
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

static PangoAlignment
text_alignment(enum text_justify justify)
{
	switch (justify) {
	case JUSTIFY_CENTER:
		return (PANGO_ALIGN_CENTER);
	case JUSTIFY_RIGHT:
		return (PANGO_ALIGN_RIGHT);
	case JUSTIFY_LEFT:
	default:
		return (PANGO_ALIGN_LEFT);
	}
}

static void
button_image_rect(const struct cell *c, double x, double y, double w,
    double h, int *ix, int *iy, int *iw, int *ih)
{
	enum image_position pos = c->style.image_pos_set ? c->style.image_pos :
	    style.image_pos;
	int size = c->style.image_size_set ? c->style.image_size :
	    style.image_size;

	if (pos == IMAGE_FILL) {
		*ix = (int)x;
		*iy = (int)y;
		*iw = (int)w;
		*ih = (int)h;
		return;
	}

	if (size > (int)h - TEXT_PAD * 2)
		size = (int)h - TEXT_PAD * 2;
	if (size < 1)
		size = 1;

	*iy = (int)(y + (h - size) / 2.0);
	*iw = size;
	*ih = size;
	switch (pos) {
	case IMAGE_CENTER:
		*ix = (int)(x + (w - size) / 2.0);
		break;
	case IMAGE_RIGHT:
		*ix = (int)(x + w - size - TEXT_PAD);
		break;
	case IMAGE_LEFT:
	case IMAGE_FILL:
		*ix = (int)(x + TEXT_PAD);
		break;
	}
}

static void
draw_button_image(const struct cell *c, const char *path,
    struct cow_pixbuf *pixbuf, double x, double y, double w, double h)
{
	int ix, iy, iw, ih;

	if (path == NULL)
		return;
	button_image_rect(c, x, y, w, h, &ix, &iy, &iw, &ih);
	cow_image_render_rect(path, pixbuf, ix, iy, iw, ih);
}

static const float *
cell_bg_colour(const struct cell *c, int sunken)
{
	if (sunken && c->style.bg_pressed_set)
		return c->style.bg_pressed;
	if (!sunken && c->style.bg_set)
		return c->style.bg;
	return sunken ? style.bg_pressed : style.bg;
}

static const float *
cell_fg_colour(const struct cell *c, int sunken)
{
	if (sunken && c->style.fg_pressed_set)
		return c->style.fg_pressed;
	if (!sunken && c->style.fg_set)
		return c->style.fg;
	return sunken ? style.fg_pressed : style.fg;
}

static const char *
cell_image_path(const struct cell *c, int sunken)
{
	if (sunken && c->style.image_pressed != NULL)
		return c->style.image_pressed;
	if (!sunken && c->style.image != NULL)
		return c->style.image;
	return sunken && style.image_pressed != NULL ? style.image_pressed :
	    style.image;
}

static int
cell_image_size(const struct cell *c)
{
	return c->style.image_size_set ? c->style.image_size : style.image_size;
}

static enum image_position
cell_image_position(const struct cell *c)
{
	return c->style.image_pos_set ? c->style.image_pos : style.image_pos;
}

static enum text_justify
cell_justify(const struct cell *c)
{
	return c->style.justify_set ? c->style.justify : style.justify;
}

static struct cow_vector_button
panel_indicator_vector(enum panel_direction direction)
{
	struct cow_vector_button v = { 0 };

	v.num_points = 4;
	switch (direction) {
	case PANEL_LEFT:
		v.points[0] = (struct cow_vector_point) { 65, 25, 0, 0, -1 };
		v.points[1] = (struct cow_vector_point) { 35, 50, 0, 0,
		    COW_VECTOR_FG };
		v.points[2] = (struct cow_vector_point) { 65, 75, 0, 0,
		    COW_VECTOR_FG };
		v.points[3] = (struct cow_vector_point) { 65, 25, 0, 0,
		    COW_VECTOR_FG };
		break;
	case PANEL_RIGHT:
		v.points[0] = (struct cow_vector_point) { 35, 25, 0, 0, -1 };
		v.points[1] = (struct cow_vector_point) { 65, 50, 0, 0,
		    COW_VECTOR_FG };
		v.points[2] = (struct cow_vector_point) { 35, 75, 0, 0,
		    COW_VECTOR_FG };
		v.points[3] = (struct cow_vector_point) { 35, 25, 0, 0,
		    COW_VECTOR_FG };
		break;
	case PANEL_UP:
		v.points[0] = (struct cow_vector_point) { 25, 65, 0, 0, -1 };
		v.points[1] = (struct cow_vector_point) { 50, 35, 0, 0,
		    COW_VECTOR_FG };
		v.points[2] = (struct cow_vector_point) { 75, 65, 0, 0,
		    COW_VECTOR_FG };
		v.points[3] = (struct cow_vector_point) { 25, 65, 0, 0,
		    COW_VECTOR_FG };
		break;
	case PANEL_DOWN:
		v.points[0] = (struct cow_vector_point) { 25, 35, 0, 0, -1 };
		v.points[1] = (struct cow_vector_point) { 50, 65, 0, 0,
		    COW_VECTOR_FG };
		v.points[2] = (struct cow_vector_point) { 75, 35, 0, 0,
		    COW_VECTOR_FG };
		v.points[3] = (struct cow_vector_point) { 25, 35, 0, 0,
		    COW_VECTOR_FG };
		break;
	}
	return v;
}

static enum panel_direction
panel_direction_opposite(enum panel_direction direction)
{
	switch (direction) {
	case PANEL_LEFT:
		return PANEL_RIGHT;
	case PANEL_RIGHT:
		return PANEL_LEFT;
	case PANEL_UP:
		return PANEL_DOWN;
	case PANEL_DOWN:
		return PANEL_UP;
	}
	return PANEL_RIGHT;
}

static bool
panel_visible(int panel)
{
	if (panel < 0 || panel >= n_selected_panels)
		return false;
	return selected_panels[panel].mapped;
}

static void
draw_panel_indicator(struct cow_pixbuf *pixbuf, const struct cell *c, int x,
    int y, int w, int h, bool sunken)
{
	enum panel_direction direction = c->panel_dir;
	struct cow_vector_button vector;
	int btn_s = (w < h ? w : h) - TEXT_PAD * 2;

	if (panel_visible(c->target_panel))
		direction = panel_direction_opposite(direction);
	vector = panel_indicator_vector(direction);

	if (btn_s < 6)
		btn_s = h < w ? h : w;
	if (btn_s < 1)
		return;
	if (btn_s > 64)
		btn_s = 64;

	int bx = x + (w - btn_s) / 2;
	int by = y + (h - btn_s) / 2;
	uint32_t px_bg = cow_colour_to_pixel(cell_bg_colour(c, sunken));
	uint32_t colors[5] = {
		[COW_VECTOR_SHADOW] = cow_colour_to_pixel(style.bg_pressed),
		[COW_VECTOR_HIGHLIGHT] = cow_colour_to_pixel(style.fg_pressed),
		[COW_VECTOR_BG] = px_bg,
		[COW_VECTOR_FG] = cow_colour_to_pixel(cell_fg_colour(c, sunken)),
		[COW_VECTOR_INVISIBLE] = 0,
	};
	uint32_t btn_buf[btn_s * btn_s];

	for (int i = 0; i < btn_s * btn_s; i++)
		btn_buf[i] = px_bg;
	cow_vector_button_draw(&vector, btn_buf, btn_s, btn_s, btn_s, btn_s,
	    colors, sunken);
	for (int py = 0; py < btn_s; py++) {
		int dy = by + py;

		if (dy < 0 || dy >= pixbuf->height)
			continue;
		for (int px = 0; px < btn_s; px++) {
			int dx = bx + px;
			uint32_t colour = btn_buf[py * btn_s + px];

			if (dx < 0 || dx >= pixbuf->width || colour == px_bg)
				continue;
			pixbuf->pixels[dy * pixbuf->stride_px + dx] = colour;
		}
	}
}

static void
render(void)
{
	struct cow_module_shm_buffer *buf;
	struct cow_pixbuf pixbuf;
	cairo_surface_t *cs;
	cairo_t *cr;
	struct state st;

	if (!configured)
		return;

	win_w = target_width();
	win_h = target_height();
	buf = cow_module_shm_buffer_next(buffers,
	    sizeof(buffers) / sizeof(buffers[0]), wl_shm, PROG, win_w, win_h);
	if (buf == NULL)
		return;

	st = current_state;
	dirty = 0;

	cs = cairo_image_surface_create_for_data(buf->data, CAIRO_FORMAT_ARGB32,
	    win_w, win_h, win_w * 4);
	pixbuf = (struct cow_pixbuf){
		.pixels = buf->data,
		.width = win_w,
		.height = win_h,
		.stride_px = win_w,
	};
	cr = cairo_create(cs);
	PangoFontDescription *font = pango_font_description_new();
	pango_font_description_set_family(font, cfg.font);
	pango_font_description_set_absolute_size(font, 12 * PANGO_SCALE);

	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
	cow_module_cairo_set_source(cr, style.bg);
	cairo_paint(cr);
	cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

	for (int row = 0; row < cfg.rows; row++) {
		for (int col = 0; col < cfg.cols; col++) {
			const struct cell *c = cell_at_grid(row, col);
			double x = col_offset(col);
			double y = row_offset(row);
			double cell_w = col_width(col);
			double cell_h = row_height(row);
			double tx = x + TEXT_PAD;
			double tw;
			const char *label = "";
			const char *image = NULL;
			int sunken = 0;

			if (c == NULL && grid_covered_by_span(row, col))
				continue;
			if (c != NULL) {
				cell_w = cols_width(col, cell_colspan(c));
				cell_h = rows_height(row, cell_rowspan(c));
			}
			tw = cell_w - 2 * TEXT_PAD;

			if (c != NULL && c->type == CELL_SPACER)
				continue;
			if (c != NULL && (c->type == CELL_BUTTON ||
			    c->type == CELL_SCRIPT ||
			    c->type == CELL_PANEL_HANDLE)) {
				label = c->label;
				sunken = pressed_cell >= 0 &&
				    &cells[pressed_cell] == c;
				image = cell_image_path(c, sunken);
			} else if (c != NULL && c->type == CELL_EXTERNAL) {
				label = c->label;
			} else if (c != NULL && c->type == CELL_STATUS)
				label = status_text(c, &st);

			const float *cell_bg = c == NULL ? style.bg :
			    cell_bg_colour(c, sunken);
			cow_module_cairo_fill(cr, cell_bg, x, y, cell_w, cell_h);
			if (c != NULL)
				draw_button_image(c, image, &pixbuf, x, y, cell_w,
				    cell_h);
			cow_module_cairo_relief(cr, x, y, cell_w, cell_h, sunken,
			    cell_bg);

			if (c != NULL && image != NULL &&
			    cell_image_position(c) == IMAGE_LEFT) {
				tx += cell_image_size(c) + TEXT_PAD;
				tw -= cell_image_size(c) + TEXT_PAD;
			} else if (c != NULL && image != NULL &&
			    cell_image_position(c) ==
			    IMAGE_RIGHT) {
				tw -= cell_image_size(c) + TEXT_PAD;
			}
			cow_module_cairo_set_source(cr, c == NULL ? style.fg :
			    cell_fg_colour(c, sunken));
			cow_text_render(cr, font, tx, y, tw, cell_h, label,
			    text_alignment(c == NULL ? style.justify :
				cell_justify(c)), NULL);
		}
	}

	cairo_surface_flush(cs);
	for (int i = 0; i < n_cells; i++) {
		const struct cell *c = &cells[i];
		bool sunken = pressed_cell >= 0 && &cells[pressed_cell] == c;

		if (cell_hidden(c) || c->type != CELL_PANEL_HANDLE ||
		    cell_image_path(c, sunken) != NULL)
			continue;
		int x = col_offset(c->col);
		int y = row_offset(c->row);
		int w = cols_width(c->col, cell_colspan(c));
		int h = rows_height(c->row, cell_rowspan(c));

		if (w > 0 && h > 0)
			draw_panel_indicator(&pixbuf, c, x, y, w, h, sunken);
	}
	cairo_surface_mark_dirty(cs);
	pango_font_description_free(font);
	cairo_destroy(cr);
	cairo_surface_destroy(cs);

	buf->busy = true;
	wl_surface_attach(surface, buf->buffer, 0, 0);
	wl_surface_damage_buffer(surface, 0, 0, win_w, win_h);
	wl_surface_commit(surface);
}

static void
ipc_send(const char *cmd)
{
	(void)cow_module_cmd_execute(PROG, cmd, true);
}

static void
script_schedule(struct cell *c, int seconds)
{
	struct timeval tv = { .tv_sec = seconds };

	if (c->script_timer != NULL)
		event_add(c->script_timer, &tv);
}

static void
script_read_output(struct cell *c)
{
	char buf[512];
	ssize_t n;

	while ((n = read(c->script_fd, buf, sizeof(buf))) > 0) {
		for (ssize_t i = 0; i < n; i++) {
			unsigned char ch = (unsigned char)buf[i];

			if (c->script_output_done)
				continue;
			if (ch == '\n') {
				c->script_output_done = true;
				continue;
			}
			if (ch == '\r' || ch == '\0')
				continue;
			if (c->script_output_len + 1 <
			    sizeof(c->script_output)) {
				c->script_output[c->script_output_len++] = (char)ch;
				c->script_output[c->script_output_len] = '\0';
			}
		}
	}
	if (n == 0 && c->script_read_event != NULL)
		event_del(c->script_read_event);
}

static void
script_read_cb(__unused evutil_socket_t fd, __unused short events, void *data)
{
	script_read_output(data);
}

static void
script_timer_cb(__unused evutil_socket_t fd, __unused short events, void *data)
{
	struct cell *c = data;
	int pipefd[2];
	pid_t pid;

	if (c->script_pid > 0)
		return;
	if (pipe(pipefd) < 0) {
		fprintf(stderr, "%s: script: pipe: %s\n", PROG,
		    strerror(errno));
		script_schedule(c, c->script_interval);
		return;
	}
	pid = fork();
	if (pid < 0) {
		fprintf(stderr, "%s: script: fork: %s\n", PROG,
		    strerror(errno));
		close(pipefd[0]);
		close(pipefd[1]);
		script_schedule(c, c->script_interval);
		return;
	}
	if (pid == 0) {
		close(pipefd[0]);
		setpgid(0, 0);
		if (dup2(pipefd[1], STDOUT_FILENO) < 0)
			_exit(127);
		close(pipefd[1]);
		execl("/bin/sh", "sh", "-c", c->script, (char *)NULL);
		_exit(127);
	}

	close(pipefd[1]);
	setpgid(pid, pid);
	fcntl(pipefd[0], F_SETFL, O_NONBLOCK);
	c->script_pid = pid;
	c->script_fd = pipefd[0];
	c->script_output[0] = '\0';
	c->script_output_len = 0;
	c->script_output_done = false;
	c->script_read_event = event_new(script_event_base, c->script_fd,
	    EV_READ | EV_PERSIST, script_read_cb, c);
	if (c->script_read_event == NULL ||
	    event_add(c->script_read_event, NULL) < 0) {
		fprintf(stderr, "%s: script: cannot monitor output: %s\n", PROG,
		    c->script);
		if (c->script_read_event != NULL) {
			event_free(c->script_read_event);
			c->script_read_event = NULL;
		}
		close(c->script_fd);
		c->script_fd = -1;
	}
}

static void
script_finish(struct cell *c, int status)
{
	if (c->script_fd >= 0)
		script_read_output(c);
	if (c->script_read_event != NULL) {
		event_free(c->script_read_event);
		c->script_read_event = NULL;
	}
	if (c->script_fd >= 0) {
		close(c->script_fd);
		c->script_fd = -1;
	}
	c->script_pid = -1;

	if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
		if (strcmp(c->label, c->script_output) != 0) {
			strlcpy(c->label, c->script_output, sizeof(c->label));
			dirty = 1;
			render();
		}
	} else if (WIFEXITED(status)) {
		fprintf(stderr, "%s: script exited with status %d: %s\n", PROG,
		    WEXITSTATUS(status), c->script);
	} else if (WIFSIGNALED(status)) {
		fprintf(stderr, "%s: script terminated by signal %d: %s\n",
		    PROG, WTERMSIG(status), c->script);
	}
	script_schedule(c, c->script_interval);
}

static void
script_sigchld_cb(__unused evutil_socket_t sig, __unused short events,
    __unused void *data)
{
	for (int i = 0; i < n_cells; i++) {
		struct cell *c = &cells[i];
		int status;

		if (c->type != CELL_SCRIPT || c->script_pid <= 0)
			continue;
		if (waitpid(c->script_pid, &status, WNOHANG) == c->script_pid)
			script_finish(c, status);
	}
}

static bool
script_cells_start(struct event_base *base)
{
	struct timeval now = { 0 };
	bool have_scripts = false;

	script_event_base = base;
	for (int i = 0; i < n_cells; i++) {
		struct cell *c = &cells[i];

		if (c->type != CELL_SCRIPT)
			continue;
		have_scripts = true;
		c->script_timer = evtimer_new(base, script_timer_cb, c);
		if (c->script_timer == NULL ||
		    event_add(c->script_timer, &now) < 0)
			return false;
	}
	if (!have_scripts)
		return true;
	script_sigchld_event = evsignal_new(base, SIGCHLD, script_sigchld_cb,
	    NULL);
	return script_sigchld_event != NULL &&
	    event_add(script_sigchld_event, NULL) == 0;
}

static void
script_cells_stop(void)
{
	if (script_sigchld_event != NULL) {
		event_free(script_sigchld_event);
		script_sigchld_event = NULL;
	}
	for (int i = 0; i < n_cells; i++) {
		struct cell *c = &cells[i];

		if (c->type != CELL_SCRIPT)
			continue;
		if (c->script_timer != NULL) {
			event_free(c->script_timer);
			c->script_timer = NULL;
		}
		if (c->script_read_event != NULL) {
			event_free(c->script_read_event);
			c->script_read_event = NULL;
		}
		if (c->script_fd >= 0) {
			close(c->script_fd);
			c->script_fd = -1;
		}
		if (c->script_pid > 0) {
			kill(-c->script_pid, SIGTERM);
			waitpid(c->script_pid, NULL, WNOHANG);
			c->script_pid = -1;
		}
	}
	script_event_base = NULL;
}

static char *
ipc_query(const char *cmd)
{
	return cow_module_cmd_request(PROG, cmd, true);
}

static void
win_from_json(struct win_info *wi, cJSON *w)
{
	cJSON *v;

	memset(wi, 0, sizeof(*wi));
#define GS(k, f, z)                    \
	v = cJSON_GetObjectItem(w, k); \
	if (cJSON_IsString(v))         \
		strlcpy(wi->f, v->valuestring, z)
#define GN(k, f)                       \
	v = cJSON_GetObjectItem(w, k); \
	if (cJSON_IsNumber(v))         \
		wi->f = (int)v->valuedouble
	GS("id", id, sizeof(wi->id));
	GS("app_id", app_id, sizeof(wi->app_id));
	GS("title", title, sizeof(wi->title));
	GS("display_title", display_title, sizeof(wi->display_title));
	GS("titlebar_position", titlebar_position, sizeof(wi->titlebar_position));
	GN("x", x);
	GN("y", y);
	GN("width", width);
	GN("height", height);
	GN("border_width", border_width);
	GN("title_thickness", title_thickness);
	v = cJSON_GetObjectItem(w, "raise_seq");
	if (cJSON_IsNumber(v))
		wi->raise_seq = v->valuedouble;
#undef GN
#undef GS
}

static void
win_frame_edges(const struct win_info *w, int *left, int *top, int *right,
    int *bottom)
{
	int title_l = 0, title_t = 0, title_r = 0, title_b = 0;

	if (strcmp(w->titlebar_position, "left") == 0)
		title_l = w->title_thickness;
	else if (strcmp(w->titlebar_position, "right") == 0)
		title_r = w->title_thickness;
	else if (strcmp(w->titlebar_position, "bottom") == 0)
		title_b = w->title_thickness;
	else
		title_t = w->title_thickness;

	*left = w->x - w->border_width - title_l;
	*top = w->y - w->border_width - title_t;
	*right = w->x + w->width + w->border_width + title_r;
	*bottom = w->y + w->height + w->border_width + title_b;
}

static void
win_content_inset(const struct win_info *w, int *x, int *y)
{
	*x = w->border_width;
	*y = w->border_width;
	if (strcmp(w->titlebar_position, "left") == 0)
		*x += w->title_thickness;
	else if (strcmp(w->titlebar_position, "top") == 0 ||
	    w->titlebar_position[0] == '\0')
		*y += w->title_thickness;
}

static bool
windows_find_for_panel(struct panel_selection *p, struct win_info *panel,
    struct win_info *external)
{
	char *resp, *panel_app;
	bool found_panel = false, found_external = false;
	double best_raise = -1.0;

	xasprintf(&panel_app, "%s:%s", PROG, p->name);
	resp = ipc_query("show -a window");
	if (resp == NULL) {
		free(panel_app);
		return false;
	}
	cJSON *root = cJSON_Parse(resp);
	free(resp);
	if (root == NULL) {
		free(panel_app);
		return false;
	}
	cJSON *data = cJSON_GetObjectItem(root, "data");
	if (!cJSON_IsArray(data)) {
		cJSON_Delete(root);
		free(panel_app);
		return false;
	}
	cJSON *w;
	cJSON_ArrayForEach(w, data) {
		struct win_info wi;

		if (!cJSON_IsObject(w))
			continue;
		win_from_json(&wi, w);
		if (strcmp(wi.app_id, panel_app) == 0 ||
		    strcmp(wi.title, panel_app) == 0 ||
		    strcmp(wi.display_title, panel_app) == 0 ||
		    strcmp(wi.title, instance_title) == 0 ||
		    strcmp(wi.display_title, instance_title) == 0) {
			*panel = wi;
			found_panel = true;
			continue;
		}
		if (external != NULL && p->external_app_id != NULL &&
		    strcmp(wi.app_id, p->external_app_id) == 0 &&
		    wi.raise_seq >= best_raise) {
			*external = wi;
			best_raise = wi.raise_seq;
			found_external = true;
		}
	}
	cJSON_Delete(root);
	free(panel_app);
	return found_panel && (external == NULL || found_external);
}

static bool
windows_find_panel(struct panel_selection *p, struct win_info *panel)
{
	return windows_find_for_panel(p, panel, NULL);
}

static void
external_rule_remove(struct panel_selection *p)
{
	char *cmd;

	if (p->external_rule[0] == '\0')
		return;
	xasprintf(&cmd, "rule -d -n %s", p->external_rule);
	ipc_send(cmd);
	free(cmd);
	p->external_rule[0] = '\0';
}

static bool
external_preplace(int panel)
{
	struct panel_selection *p;
	struct win_info pw;
	int pl, pt, pr, pb, ix, iy, x, y;
	int ew, eh, etitle_l = 0, etitle_t = 0, etitle_r = 0, etitle_b = 0;
	char *cmd;

	if (panel < 0 || panel >= n_selected_panels)
		return false;
	p = &selected_panels[panel];
	if (p->external_app_id == NULL || p->external_w <= 0 ||
	    p->external_h <= 0)
		return false;
	if (!windows_find_panel(p, &pw))
		return false;

	win_frame_edges(&pw, &pl, &pt, &pr, &pb);
	ix = pw.border_width;
	iy = pw.border_width;
	if (strcmp(pw.titlebar_position, "left") == 0) {
		ix += pw.title_thickness;
		etitle_l = pw.title_thickness;
	} else if (strcmp(pw.titlebar_position, "right") == 0)
		etitle_r = pw.title_thickness;
	else if (strcmp(pw.titlebar_position, "bottom") == 0)
		etitle_b = pw.title_thickness;
	else {
		iy += pw.title_thickness;
		etitle_t = pw.title_thickness;
	}
	ew = p->external_w + pw.border_width * 2 + etitle_l + etitle_r;
	eh = p->external_h + pw.border_width * 2 + etitle_t + etitle_b;
	x = pw.x;
	y = pw.y;
	switch (p->direction) {
	case PANEL_LEFT:
		x = pl - ew + ix;
		y = pt + iy;
		break;
	case PANEL_RIGHT:
		x = pr + ix;
		y = pt + iy;
		break;
	case PANEL_UP:
		x = pl + ix;
		y = pt - eh + iy;
		break;
	case PANEL_DOWN:
		x = pl + ix;
		y = pb + iy;
		break;
	}

	external_rule_remove(p);
	snprintf(p->external_rule, sizeof(p->external_rule),
	    "cowbuttons-panel-%ld-%u", (long)getpid(),
	    ++p->external_rule_serial);
	xasprintf(&cmd, "rule -g -n %s -T on-map -s %%%s "
	    "window-move -x %d -y %d",
	    p->external_rule, p->external_app_id, x, y);
	ipc_send(cmd);
	free(cmd);
	return true;
}

static void
external_place(int panel)
{
	struct panel_selection *p;
	struct win_info pw, ew;
	int pl, pt, pr, pb, el, et, er, eb, ix, iy, x, y;
	char *cmd;
	bool found = false;

	if (panel < 0 || panel >= n_selected_panels)
		return;
	p = &selected_panels[panel];
	for (int i = 0; i < EXTERNAL_PLACE_TRIES; i++) {
		if (windows_find_for_panel(p, &pw, &ew)) {
			found = true;
			break;
		}
		usleep(EXTERNAL_PLACE_DELAY_US);
	}
	if (!found)
		return;

	win_frame_edges(&pw, &pl, &pt, &pr, &pb);
	win_frame_edges(&ew, &el, &et, &er, &eb);
	win_content_inset(&ew, &ix, &iy);
	x = ew.x;
	y = ew.y;
	switch (p->direction) {
	case PANEL_LEFT:
		x = pl - (er - el) + ix;
		y = pt + iy;
		break;
	case PANEL_RIGHT:
		x = pr + ix;
		y = pt + iy;
		break;
	case PANEL_UP:
		x = pl + ix;
		y = pt - (eb - et) + iy;
		break;
	case PANEL_DOWN:
		x = pl + ix;
		y = pb + iy;
		break;
	}
	strlcpy(p->external_id, ew.id, sizeof(p->external_id));
	p->mapped = true;
	external_rule_remove(p);
	xasprintf(&cmd, "window-move -t '#%s' -x %d -y %d", ew.id, x, y);
	for (int i = 0; i < EXTERNAL_MOVE_TRIES; i++) {
		ipc_send(cmd);
		usleep(EXTERNAL_PLACE_DELAY_US);
	}
	free(cmd);
}

static void
external_reap(struct panel_selection *p)
{
	int status;

	if (p->external_pid <= 0)
		return;
	if (waitpid(p->external_pid, &status, WNOHANG) == p->external_pid)
		p->external_pid = -1;
	else if (errno == ECHILD)
		p->external_pid = -1;
}

static bool
external_panels_exist(void)
{
	for (int i = 0; i < n_selected_panels; i++)
		if (selected_panels[i].external_command != NULL)
			return true;
	return false;
}

static const char *
panel_direction_name(enum panel_direction direction)
{
	switch (direction) {
	case PANEL_LEFT:
		return "left";
	case PANEL_RIGHT:
		return "right";
	case PANEL_UP:
		return "up";
	case PANEL_DOWN:
		return "down";
	}
	return "right";
}

static void
external_start(int panel)
{
	struct panel_selection *p;
	pid_t pid;
	struct win_info panel_win, external_win;

	if (panel < 0 || panel >= n_selected_panels)
		return;
	p = &selected_panels[panel];
	if (p->external_command == NULL || !p->open)
		return;
	external_reap(p);
	if (windows_find_for_panel(p, &panel_win, &external_win)) {
		p->mapped = true;
		strlcpy(p->external_id, external_win.id, sizeof(p->external_id));
		external_place(panel);
		return;
	}
	if (p->external_pid > 0) {
		external_place(panel);
		return;
	}
	external_preplace(panel);
	pid = fork();
	if (pid < 0)
		return;
	if (pid == 0) {
		setenv("COWBUTTONS_PANEL", p->name, 1);
		setenv("COWBUTTONS_PANEL_DIRECTION",
		    panel_direction_name(p->direction), 1);
		setenv("COWBUTTONS_PANEL_MODE", "toplevel", 1);
		execl("/bin/sh", "sh", "-c", p->external_command,
		    (char *)NULL);
		_exit(127);
	}
	p->external_pid = pid;
	external_place(panel);
}

static void
external_stop(int panel)
{
	struct panel_selection *p;
	char *cmd;

	if (panel < 0 || panel >= n_selected_panels)
		return;
	p = &selected_panels[panel];
	external_rule_remove(p);
	external_reap(p);
	if (p->external_id[0] != '\0') {
		xasprintf(&cmd, "winops -t '#%s' -c", p->external_id);
		ipc_send(cmd);
		free(cmd);
		p->external_id[0] = '\0';
	}
	p->mapped = false;
	if (p->external_pid <= 0)
		return;
	kill(p->external_pid, SIGTERM);
	external_reap(p);
}

static void
external_refresh(void)
{
	bool changed = false;

	for (int i = 0; i < n_selected_panels; i++) {
		struct panel_selection *p = &selected_panels[i];
		struct win_info pw, ew;
		bool mapped;

		if (p->external_command == NULL)
			continue;
		external_reap(p);
		mapped = windows_find_for_panel(p, &pw, &ew);
		if (mapped)
			strlcpy(p->external_id, ew.id, sizeof(p->external_id));
		else if (p->external_id[0] != '\0') {
			p->external_id[0] = '\0';
			p->open = false;
		}
		if (p->mapped != mapped) {
			p->mapped = mapped;
			changed = true;
		}
	}
	if (changed) {
		update_toplevel_size();
		render();
	}
}

static void
externals_sync(void)
{
	for (int i = 0; i < n_selected_panels; i++) {
		if (selected_panels[i].open)
			external_start(i);
		else
			external_stop(i);
	}
}

static void
externals_stop_all(void)
{
	for (int i = 0; i < n_selected_panels; i++)
		external_stop(i);
}

static void
update_toplevel_size(void)
{
	win_w = natural_width();
	win_h = natural_height();
	if (xdg_toplevel == NULL)
		return;
	xdg_toplevel_set_min_size(xdg_toplevel, win_w, win_h);
	if (cfg.resizable)
		xdg_toplevel_set_max_size(xdg_toplevel, 0, 0);
	else
		xdg_toplevel_set_max_size(xdg_toplevel, win_w, win_h);
}

static void
panel_reposition_toplevel(int panel, const struct win_info *before, int old_w,
    int old_h)
{
	struct panel_selection *p;
	char *cmd;
	int new_w, new_h, x, y;

	if (panel < 0 || panel >= n_selected_panels || before->id[0] == '\0')
		return;
	p = &selected_panels[panel];
	if (p->external_command != NULL)
		return;

	new_w = target_width();
	new_h = target_height();
	x = before->x;
	y = before->y;
	switch (p->direction) {
	case PANEL_LEFT:
		x -= new_w - old_w;
		break;
	case PANEL_UP:
		y -= new_h - old_h;
		break;
	case PANEL_RIGHT:
	case PANEL_DOWN:
		break;
	}
	if (x == before->x && y == before->y)
		return;
	xasprintf(&cmd, "window-move -t '#%s' -x %d -y %d", before->id, x,
	    y);
	ipc_send(cmd);
	free(cmd);
}

static void
panel_toggle(int panel)
{
	struct win_info before = { 0 };
	int old_w, old_h;

	if (panel < 0 || panel >= n_selected_panels)
		return;
	old_w = target_width();
	old_h = target_height();
	windows_find_panel(&selected_panels[panel], &before);

	selected_panels[panel].open = !selected_panels[panel].open;
	if (selected_panels[panel].external_command == NULL)
		selected_panels[panel].mapped = selected_panels[panel].open;
	externals_sync();
	update_toplevel_size();
	panel_reposition_toplevel(panel, &before, old_w, old_h);
	render();
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
handle_status_line(const char *line, __unused void *data)
{
	cJSON *root = cJSON_Parse(line);
	if (root == NULL)
		return;

	struct state st;
	state_from_json(&st, root);
	cJSON_Delete(root);

	current_state = st;
	dirty = 1;
	if (configured)
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
external_sync_cb(__unused evutil_socket_t fd, __unused short events,
    __unused void *data)
{
	external_refresh();
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
    __unused struct xdg_toplevel *toplevel, int32_t w,
    int32_t h, __unused struct wl_array *states)
{
	if (cfg.resizable && w > 0 && h > 0) {
		if (w < natural_width())
			w = natural_width();
		if (h < natural_height())
			h = natural_height();
		if (panel_any_closed()) {
			w = natural_width();
			h = natural_height();
		}
		if (w != win_w || h != win_h) {
			win_w = w;
			win_h = h;
			dirty = 1;
		}
	}
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
    uint32_t serial, __unused struct wl_surface *s,
    wl_fixed_t sx, wl_fixed_t sy)
{
	cow_module_cursor_set_default(&cursor, serial);
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

static int
click_button_index(uint32_t button)
{
	switch (button) {
	case BTN_LEFT:
		return CLICK_LEFT;
	case BTN_MIDDLE:
		return CLICK_MIDDLE;
	case BTN_RIGHT:
		return CLICK_RIGHT;
	default:
		return -1;
	}
}

static bool
cell_has_click_action(const struct cell *c, int button)
{
	return c->command[button][CLICK_SINGLE][0] != '\0' ||
	    c->command[button][CLICK_DOUBLE][0] != '\0';
}

static void
pending_click_run_single(void)
{
	int cell = pending_click_cell;
	int button = pending_click_button;

	pending_click_cell = -1;
	pending_click_button = -1;
	pending_click_time = 0;
	if (click_timer != NULL)
		event_del(click_timer);
	if (cell >= 0 && cell < n_cells && button >= 0 &&
	    button < CLICK_BUTTON_COUNT &&
	    cells[cell].command[button][CLICK_SINGLE][0] != '\0')
		ipc_send(cells[cell].command[button][CLICK_SINGLE]);
}

static void
click_timer_cb(__unused evutil_socket_t fd, __unused short events,
    __unused void *data)
{
	pending_click_run_single();
}

static void
cell_click(int cell, int button, uint32_t time)
{
	struct cell *c = &cells[cell];
	const char *single = c->command[button][CLICK_SINGLE];
	const char *double_click = c->command[button][CLICK_DOUBLE];

	if (double_click[0] == '\0') {
		if (pending_click_cell >= 0)
			pending_click_run_single();
		if (single[0] != '\0')
			ipc_send(single);
		return;
	}

	if (pending_click_cell == cell && pending_click_button == button &&
	    time - pending_click_time < DOUBLE_CLICK_MS) {
		pending_click_cell = -1;
		pending_click_button = -1;
		pending_click_time = 0;
		event_del(click_timer);
		ipc_send(double_click);
		return;
	}

	if (pending_click_cell >= 0)
		pending_click_run_single();
	pending_click_cell = cell;
	pending_click_button = button;
	pending_click_time = time;
	struct timeval delay = {
	    .tv_sec = 0,
	    .tv_usec = DOUBLE_CLICK_MS * 1000,
	};
	evtimer_add(click_timer, &delay);
}

static void
pointer_button(__unused void *data, __unused struct wl_pointer *p,
    __unused uint32_t serial, uint32_t time, uint32_t button,
    uint32_t state)
{
	int click_button = click_button_index(button);
	if (click_button < 0)
		return;

	if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
		int idx = cell_at_point(ptr_x, ptr_y);
		if (idx >= 0 && (cells[idx].type == CELL_BUTTON ||
		    cells[idx].type == CELL_SCRIPT) &&
		    cell_has_click_action(&cells[idx], click_button)) {
			pressed_cell = idx;
			pressed_button = click_button;
			render();
		} else if (idx >= 0 && click_button == CLICK_LEFT &&
		    cells[idx].type == CELL_PANEL_HANDLE) {
			pressed_cell = idx;
			pressed_button = click_button;
			render();
		}
		return;
	}

	if (pressed_button != click_button)
		return;
	int idx = cell_at_point(ptr_x, ptr_y);
	int pressed = pressed_cell;
	pressed_cell = -1;
	pressed_button = -1;
	render();
	if (pressed >= 0 && idx == pressed &&
	    (cells[pressed].type == CELL_BUTTON ||
	    cells[pressed].type == CELL_SCRIPT))
		cell_click(pressed, click_button, time);
	else if (pressed >= 0 && cells[pressed].type == CELL_PANEL_HANDLE)
		panel_toggle(cells[pressed].target_panel);
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
		cow_module_cursor_set_pointer(&cursor, pointer);
	} else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && pointer != NULL) {
		cow_module_cursor_clear_pointer(&cursor, pointer);
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
	} else
		cow_module_cursor_handle_global(&cursor, reg, name, interface,
		    version);
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
	char *config_path = NULL;
	struct wl_registry *reg;
	struct event_base *base;
	struct event *wl_ev = NULL, *external_ev = NULL;
	struct timeval external_tv = {
		.tv_sec = 0,
		.tv_usec = EXTERNAL_SYNC_MS * 1000,
	};

	style_init();
	if (!cow_module_config_instances_init(&config_instances, PROG,
		SHOW_INDEX_ENV))
		return 1;
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
	cow_module_config_instances_spawn(&config_instances, argv);
	free(config_path);

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
	xdg_toplevel_set_title(xdg_toplevel, instance_title);
	xdg_toplevel_set_app_id(xdg_toplevel, PROG);
	xdg_toplevel_set_min_size(xdg_toplevel, natural_width(),
	    natural_height());
	if (cfg.resizable)
		xdg_toplevel_set_max_size(xdg_toplevel, 0, 0);
	else
		xdg_toplevel_set_max_size(xdg_toplevel, natural_width(),
		    natural_height());
	wl_surface_commit(surface);
	externals_sync();

	state_clear(&current_state);
	base = event_base_new();
	if (base == NULL) {
		fprintf(stderr, "%s: failed to initialise event loop\n", PROG);
		return 1;
	}
	wl_ev = event_new(base, wl_display_get_fd(display), EV_READ | EV_PERSIST,
	    wayland_cb, NULL);
	click_timer = evtimer_new(base, click_timer_cb, NULL);
	if (external_panels_exist())
		external_ev = event_new(base, -1, EV_PERSIST,
		    external_sync_cb, NULL);
	if (wl_ev == NULL || click_timer == NULL ||
	    event_add(wl_ev, NULL) < 0 ||
	    (external_ev != NULL &&
	    event_add(external_ev, &external_tv) < 0) ||
	    !script_cells_start(base) ||
	    !cow_module_status_stream_init(&status_stream, base, PROG,
	    handle_status_line, NULL)) {
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
	if (external_ev != NULL)
		event_free(external_ev);
	if (click_timer != NULL) {
		event_free(click_timer);
		click_timer = NULL;
	}
	script_cells_stop();
	cow_module_status_stream_fini(&status_stream);
	event_base_free(base);
	externals_stop_all();

	if (pointer != NULL) {
		cow_module_cursor_clear_pointer(&cursor, pointer);
		wl_pointer_destroy(pointer);
	}
	cow_module_cursor_fini(&cursor);
	if (seat != NULL)
		wl_seat_destroy(seat);
	if (xdg_toplevel != NULL)
		xdg_toplevel_destroy(xdg_toplevel);
	if (xdg_surface != NULL)
		xdg_surface_destroy(xdg_surface);
	if (surface != NULL)
		wl_surface_destroy(surface);
	cow_module_shm_buffers_destroy(buffers,
	    sizeof(buffers) / sizeof(buffers[0]));
	for (int i = 0; i < n_cells; i++) {
		free(cells[i].style.image);
		free(cells[i].style.image_pressed);
	}
	for (int i = 0; i < n_selected_panels; i++) {
		free(selected_panels[i].name);
		free(selected_panels[i].external_command);
		free(selected_panels[i].external_app_id);
		free(selected_panels[i].handle_style.image);
		free(selected_panels[i].handle_style.image_pressed);
	}
	free(style.image);
	free(style.image_pressed);
	free(config_dir);
	wl_display_disconnect(display);
	return 0;
}
