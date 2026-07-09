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
#include <libgen.h>
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
#include "cow/colour.h"
#include "cow/draw.h"
#include "cow/image.h"
#include "cow/module.h"
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
#define WL_SEAT_VERSION 4
#define DEFAULT_IMAGE_SIZE 16
#define MAX_BLOCKS 32
#define MAX_SELECTED_PANELS 32

enum cell_type {
	CELL_EMPTY,
	CELL_BUTTON,
	CELL_PANEL_TOGGLE,
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
	PANEL_NONE,
	PANEL_LEFT,
	PANEL_RIGHT,
	PANEL_UP,
	PANEL_DOWN,
};

enum block_type {
	BLOCK_ROW,
	BLOCK_IF,
	BLOCK_PANEL,
};

struct block {
	enum block_type type;
	bool		active;
};

struct panel_selection {
	char			*name;
	bool			 seen;
	bool			 visible;
	enum panel_direction	 direction;
	int			 min_row;
	int			 max_row;
	int			 min_col;
	int			 max_col;
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

struct cell {
	enum cell_type	  type;
	enum status_type  status;
	int		  row;
	int		  col;
	char		  label[128];
	char		  command[512];
	struct cell_style style;
	int		  panel;
	int		  target_panel;
};

struct state {
	char focused_title[256];
	char focused_app[128];
	char current_output[128];
	char current_desk[64];
	char desk_list[512];
};

struct shm_buffer {
	struct wl_buffer *buffer;
	void		 *data;
	size_t		  size;
	int		  w;
	int		  h;
	bool		  busy;
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
static struct style style;
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
static struct shm_buffer buffers[2];
static int	   current_row = -1;
static int	   current_col;
static int	   current_panel = -1;
static bool	   in_row;
static struct block blocks[MAX_BLOCKS];
static int	   n_blocks;

static pthread_mutex_t state_lock = PTHREAD_MUTEX_INITIALIZER;
static struct state    current_state;
static int	       dirty = 1;
static char	      *config_dir;
static struct panel_selection selected_panels[MAX_SELECTED_PANELS];
static int	       n_selected_panels;

static void render(void);
static int  cell_colspan(const struct cell *);
static int  cell_rowspan(const struct cell *);
static int  col_width(int);
static int  row_height(int);
static int  natural_width(void);
static int  natural_height(void);
static void update_toplevel_size(void);
static bool cell_covered_by_span(const struct cell *, int, int);
static bool cell_hidden(const struct cell *);

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

static int
parse_bool(const char *s, bool *out)
{
	if (strcmp(s, "true") == 0 || strcmp(s, "yes") == 0 ||
	    strcmp(s, "on") == 0 || strcmp(s, "1") == 0) {
		*out = true;
		return 1;
	}
	if (strcmp(s, "false") == 0 || strcmp(s, "no") == 0 ||
	    strcmp(s, "off") == 0 || strcmp(s, "0") == 0) {
		*out = false;
		return 1;
	}
	return 0;
}

static char *
path_dirname(const char *path)
{
	char *copy = strdup(path);
	char *dir, *out;

	if (copy == NULL)
		return NULL;
	dir = dirname(copy);
	out = strdup(dir);
	free(copy);
	return out;
}

static char *
resolve_path(const char *path)
{
	const char *home;
	char *out;

	if (path[0] == '/')
		return strdup(path);
	if (path[0] == '~' && path[1] == '/') {
		home = getenv("HOME");
		if (home == NULL || *home == '\0')
			return NULL;
		if (asprintf(&out, "%s/%s", home, path + 2) < 0)
			return NULL;
		return out;
	}
	if (config_dir == NULL)
		return strdup(path);
	if (asprintf(&out, "%s/%s", config_dir, path) < 0)
		return NULL;
	return out;
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
	selected_panels[n_selected_panels].visible = true;
	selected_panels[n_selected_panels].direction = PANEL_NONE;
	selected_panels[n_selected_panels].min_row = MAX_GRID;
	selected_panels[n_selected_panels].max_row = -1;
	selected_panels[n_selected_panels].min_col = MAX_GRID;
	selected_panels[n_selected_panels].max_col = -1;
	n_selected_panels++;
	return true;
}

static void
panel_note_cell(struct cell *c)
{
	if (c->panel < 0)
		return;

	struct panel_selection *p = &selected_panels[c->panel];
	int max_row = c->row + cell_rowspan(c) - 1;
	int max_col = c->col + cell_colspan(c) - 1;

	if (c->row < p->min_row)
		p->min_row = c->row;
	if (max_row > p->max_row)
		p->max_row = max_row;
	if (c->col < p->min_col)
		p->min_col = c->col;
	if (max_col > p->max_col)
		p->max_col = max_col;
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
	} else if (b.type == BLOCK_PANEL && b.active) {
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
	bool visible = true;
	enum panel_direction direction = PANEL_NONE;

	if ((n != 3 && n != 5) || strcmp(fields[n - 1], "{") != 0) {
		*why = "bad panel";
		return false;
	}
	if (n == 5) {
		if (strcmp(fields[2], "hidden") == 0)
			visible = false;
		else if (strcmp(fields[2], "shown") != 0) {
			*why = "bad panel";
			return false;
		}
		if (!panel_direction_parse(fields[3], &direction)) {
			*why = "bad panel direction";
			return false;
		}
	}
	if (n_blocks != 0) {
		*why = "nested panel";
		return false;
	}
	int idx = panel_selection_find(fields[1]);
	bool active = idx != -1 && blocks_active();
	if (idx != -1) {
		selected_panels[idx].seen = true;
		selected_panels[idx].visible = visible;
		selected_panels[idx].direction = direction;
	}
	if (active)
		current_panel = idx;
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

static bool
parse_span(struct cell *c, const char *val, const char **why)
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

		if (strcmp(prop, "width") == 0) {
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
	panel_note_cell(c);
	return true;
}

static bool
load_row(char **fields, int n, const char **why)
{
	int v;
	bool parent_active = blocks_active();

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

	f = fopen(path, "r");
	if (f == NULL) {
		fprintf(stderr, "%s: %s: %s\n", PROG, path, strerror(errno));
		return 0;
	}
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
			if (!load_show(fields, n, &why, true)) {
				fprintf(stderr, "%s: %s:%u: %s\n", PROG, path,
				    lineno, why);
				free_fields(fields, n);
				free(line);
				fclose(f);
				return 0;
			}
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
			strlcpy(c->command, fields[2], sizeof(c->command));
			if (!load_cell_props(c, fields, 3, n, &why))
				goto line_error;
			current_col = c->col + cell_colspan(c);
		} else if (strcmp(fields[0], "panel-toggle") == 0) {
			if (n < 2) {
				why = "bad panel-toggle";
				goto line_error;
			}
			int target = panel_selection_find(fields[1]);
			if (target == -1) {
				why = "unknown panel";
				goto line_error;
			}
			struct cell *c = cell_append();
			if (c == NULL) {
				why = in_row ? "too many cells" :
				    "panel-toggle outside row";
				goto line_error;
			}
			c->type = CELL_PANEL_TOGGLE;
			c->target_panel = target;
			if (!load_cell_props(c, fields, 2, n, &why))
				goto line_error;
			current_col = c->col + cell_colspan(c);
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
panel_horizontal(const struct panel_selection *p)
{
	return p->direction == PANEL_LEFT || p->direction == PANEL_RIGHT;
}

static bool
panel_vertical(const struct panel_selection *p)
{
	return p->direction == PANEL_UP || p->direction == PANEL_DOWN;
}

static bool
panel_col_hidden(int col)
{
	for (int i = 0; i < n_selected_panels; i++) {
		struct panel_selection *p = &selected_panels[i];

		if (p->visible || !panel_horizontal(p))
			continue;
		if (col < p->min_col || col > p->max_col)
			continue;
		for (int j = 0; j < n_cells; j++) {
			const struct cell *c = &cells[j];

			if (!cell_covered_by_span(c, c->row, col))
				continue;
			if (!cell_hidden(c))
				return false;
		}
		return true;
	}
	return false;
}

static bool
panel_row_hidden(int row)
{
	for (int i = 0; i < n_selected_panels; i++) {
		struct panel_selection *p = &selected_panels[i];

		if (p->visible || !panel_vertical(p))
			continue;
		if (row < p->min_row || row > p->max_row)
			continue;
		for (int j = 0; j < n_cells; j++) {
			const struct cell *c = &cells[j];

			if (!cell_covered_by_span(c, row, c->col))
				continue;
			if (!cell_hidden(c))
				return false;
		}
		return true;
	}
	return false;
}

static bool
cell_hidden(const struct cell *c)
{
	if (c->panel < 0)
		return false;
	return !selected_panels[c->panel].visible &&
	    selected_panels[c->panel].direction != PANEL_NONE;
}

static int
base_col_width(int col)
{
	if (panel_col_hidden(col))
		return 0;
	if (col >= 0 && col < MAX_GRID && cfg.col_w[col] > 0)
		return cfg.col_w[col];
	return cfg.cell_w;
}

static int
base_row_height(int row)
{
	if (panel_row_hidden(row))
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

static int
target_width(void)
{
	int w = natural_width();

	if (cfg.resizable && win_w > w)
		return win_w;
	return w;
}

static int
target_height(void)
{
	int h = natural_height();

	if (cfg.resizable && win_h > h)
		return win_h;
	return h;
}

static void
update_toplevel_size(void)
{
	if (xdg_toplevel == NULL)
		return;
	xdg_toplevel_set_min_size(xdg_toplevel, natural_width(),
	    natural_height());
	if (cfg.resizable)
		xdg_toplevel_set_max_size(xdg_toplevel, 0, 0);
	else
		xdg_toplevel_set_max_size(xdg_toplevel, natural_width(),
		    natural_height());
}

static int
scale_offset(int pos, int base_total, int target_total)
{
	if (base_total <= 0 || target_total <= base_total)
		return pos;
	return (int)((long long)pos * target_total / base_total);
}

static int
col_offset(int col)
{
	return scale_offset(base_cols_width(0, col), natural_width(),
	    target_width());
}

static int
row_offset(int row)
{
	return scale_offset(base_rows_height(0, row), natural_height(),
	    target_height());
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

		if (cell_hidden(c))
			continue;
		if (c->row == row && c->col == col)
			continue;
		if (cell_covered_by_span(c, row, col))
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

static void
draw_text(cairo_t *cr, double x, double y, double max_w, const char *text,
    enum text_justify justify)
{
	cairo_text_extents_t te;
	char buf[256];
	const char *out = text;
	double dx;

	if (text == NULL || max_w <= 0)
		return;
	cairo_text_extents(cr, text, &te);
	if (te.width > max_w) {
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
		out = buf;
		cairo_text_extents(cr, out, &te);
	}

	dx = 0;
	if (justify == JUSTIFY_CENTER && te.width < max_w)
		dx = (max_w - te.width) / 2.0;
	else if (justify == JUSTIFY_RIGHT && te.width < max_w)
		dx = max_w - te.width;
	cairo_move_to(cr, x + dx, y);
	cairo_show_text(cr, out);
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

static void
buffer_release(void *data, __unused struct wl_buffer *buffer)
{
	struct shm_buffer *buf = data;

	buf->busy = false;
}

static const struct wl_buffer_listener buffer_listener = {
	.release = buffer_release,
};

static void
shm_buffer_destroy(struct shm_buffer *buf)
{
	if (buf->buffer != NULL)
		wl_buffer_destroy(buf->buffer);
	if (buf->data != NULL && buf->data != MAP_FAILED)
		munmap(buf->data, buf->size);
	memset(buf, 0, sizeof(*buf));
}

static bool
shm_buffer_create(struct shm_buffer *buf, int w, int h)
{
	size_t size = (size_t)w * 4 * (size_t)h;
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
		return false;
	if (ftruncate(fd, size) < 0) {
		close(fd);
		return false;
	}
	buf->data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (buf->data == MAP_FAILED) {
		close(fd);
		return false;
	}

	struct wl_shm_pool *pool = wl_shm_create_pool(wl_shm, fd, size);
	buf->buffer = wl_shm_pool_create_buffer(pool, 0, w, h,
	    w * 4, WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);
	if (buf->buffer == NULL) {
		munmap(buf->data, size);
		memset(buf, 0, sizeof(*buf));
		return false;
	}
	wl_buffer_add_listener(buf->buffer, &buffer_listener, buf);
	buf->size = size;
	buf->w = w;
	buf->h = h;
	buf->busy = false;
	return true;
}

static struct shm_buffer *
next_buffer(int w, int h)
{
	for (size_t i = 0; i < sizeof(buffers) / sizeof(buffers[0]); i++) {
		struct shm_buffer *buf = &buffers[i];

		if (buf->busy)
			continue;
		if (buf->buffer != NULL && (buf->w != w || buf->h != h))
			shm_buffer_destroy(buf);
		if (buf->buffer == NULL && !shm_buffer_create(buf, w, h))
			return NULL;
		return buf;
	}
	return NULL;
}

static void
set_source_colour(cairo_t *cr, const float *colour)
{
	cairo_set_source_rgba(cr, colour[0], colour[1], colour[2], colour[3]);
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

static enum panel_direction
panel_toggle_direction(const struct cell *c)
{
	if (c->target_panel < 0 || c->target_panel >= n_selected_panels)
		return PANEL_NONE;

	struct panel_selection *p = &selected_panels[c->target_panel];

	switch (p->direction) {
	case PANEL_LEFT:
		return p->visible ? PANEL_RIGHT : PANEL_LEFT;
	case PANEL_RIGHT:
		return p->visible ? PANEL_LEFT : PANEL_RIGHT;
	case PANEL_UP:
		return p->visible ? PANEL_DOWN : PANEL_UP;
	case PANEL_DOWN:
		return p->visible ? PANEL_UP : PANEL_DOWN;
	case PANEL_NONE:
		break;
	}
	return PANEL_NONE;
}

static struct cow_vector_button
panel_toggle_vector(enum panel_direction direction)
{
	struct cow_vector_button v = { 0 };

	v.num_points = 3;
	switch (direction) {
	case PANEL_LEFT:
		v.points[0] = (struct cow_vector_point) { 65, 25, 0, 0, -1 };
		v.points[1] = (struct cow_vector_point) { 35, 50, 0, 0,
		    COW_VECTOR_FG };
		v.points[2] = (struct cow_vector_point) { 65, 75, 0, 0,
		    COW_VECTOR_FG };
		break;
	case PANEL_RIGHT:
		v.points[0] = (struct cow_vector_point) { 35, 25, 0, 0, -1 };
		v.points[1] = (struct cow_vector_point) { 65, 50, 0, 0,
		    COW_VECTOR_FG };
		v.points[2] = (struct cow_vector_point) { 35, 75, 0, 0,
		    COW_VECTOR_FG };
		break;
	case PANEL_UP:
		v.points[0] = (struct cow_vector_point) { 25, 65, 0, 0, -1 };
		v.points[1] = (struct cow_vector_point) { 50, 35, 0, 0,
		    COW_VECTOR_FG };
		v.points[2] = (struct cow_vector_point) { 75, 65, 0, 0,
		    COW_VECTOR_FG };
		break;
	case PANEL_DOWN:
		v.points[0] = (struct cow_vector_point) { 25, 35, 0, 0, -1 };
		v.points[1] = (struct cow_vector_point) { 50, 65, 0, 0,
		    COW_VECTOR_FG };
		v.points[2] = (struct cow_vector_point) { 75, 35, 0, 0,
		    COW_VECTOR_FG };
		break;
	case PANEL_NONE:
		v.num_points = 0;
		break;
	}
	return v;
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

static void
draw_panel_toggle(struct cow_pixbuf *pixbuf, const struct cell *c, int x, int y,
    int w, int h, bool sunken)
{
	enum panel_direction direction = panel_toggle_direction(c);
	struct cow_vector_button vector = panel_toggle_vector(direction);
	int btn_s = w < h ? w : h;

	if (vector.num_points == 0)
		return;
	btn_s -= TEXT_PAD * 2;
	if (btn_s < 6)
		btn_s = w < h ? w : h;
	if (btn_s < 1)
		return;
	if (btn_s > 128)
		btn_s = 128;

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
	struct shm_buffer *buf;
	struct cow_pixbuf pixbuf;
	cairo_surface_t *cs;
	cairo_t *cr;
	struct state st;

	if (!configured)
		return;

	win_w = target_width();
	win_h = target_height();
	buf = next_buffer(win_w, win_h);
	if (buf == NULL)
		return;

	pthread_mutex_lock(&state_lock);
	st = current_state;
	dirty = 0;
	pthread_mutex_unlock(&state_lock);

	cs = cairo_image_surface_create_for_data(buf->data, CAIRO_FORMAT_ARGB32,
	    win_w, win_h, win_w * 4);
	pixbuf = (struct cow_pixbuf){
		.pixels = buf->data,
		.width = win_w,
		.height = win_h,
		.stride_px = win_w,
	};
	cr = cairo_create(cs);
	cairo_select_font_face(cr, cfg.font, CAIRO_FONT_SLANT_NORMAL,
	    CAIRO_FONT_WEIGHT_NORMAL);
	cairo_set_font_size(cr, 12);

	cairo_set_source_rgb(cr, 0.16, 0.16, 0.17);
	cairo_paint(cr);

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
			if (c != NULL && cell_hidden(c))
				continue;
			if (c != NULL) {
				cell_w = cols_width(col, cell_colspan(c));
				cell_h = rows_height(row, cell_rowspan(c));
			}
			if (cell_w <= 0 || cell_h <= 0)
				continue;
			tw = cell_w - 2 * TEXT_PAD;

			if (c != NULL && c->type == CELL_SPACER)
				continue;
			if (c != NULL && (c->type == CELL_BUTTON ||
			    c->type == CELL_PANEL_TOGGLE)) {
				label = c->label;
				sunken = pressed_cell >= 0 &&
				    &cells[pressed_cell] == c;
				image = cell_image_path(c, sunken);
			} else if (c != NULL && c->type == CELL_STATUS)
				label = status_text(c, &st);

			set_source_colour(cr, c == NULL ? style.bg :
			    cell_bg_colour(c, sunken));
			cairo_rectangle(cr, x, y, cell_w, cell_h);
			cairo_fill(cr);
			if (c != NULL)
				draw_button_image(c, image, &pixbuf, x, y, cell_w,
				    cell_h);
			draw_relief(cr, x, y, cell_w, cell_h, sunken);

			if (c != NULL && image != NULL &&
			    cell_image_position(c) == IMAGE_LEFT) {
				tx += cell_image_size(c) + TEXT_PAD;
				tw -= cell_image_size(c) + TEXT_PAD;
			} else if (c != NULL && image != NULL &&
			    cell_image_position(c) ==
			    IMAGE_RIGHT) {
				tw -= cell_image_size(c) + TEXT_PAD;
			}
			set_source_colour(cr, c == NULL ? style.fg :
			    cell_fg_colour(c, sunken));
			draw_text(cr, tx, y + cell_h / 2.0 + 4.0, tw,
			    label, c == NULL ? style.justify : cell_justify(c));
		}
	}

	cairo_surface_flush(cs);
	for (int i = 0; i < n_cells; i++) {
		const struct cell *c = &cells[i];

		if (c->type != CELL_PANEL_TOGGLE || cell_hidden(c))
			continue;
		int x = col_offset(c->col);
		int y = row_offset(c->row);
		int w = cols_width(c->col, cell_colspan(c));
		int h = rows_height(c->row, cell_rowspan(c));
		bool sunken = pressed_cell >= 0 && &cells[pressed_cell] == c;

		if (w <= 0 || h <= 0)
			continue;
		draw_panel_toggle(&pixbuf, c, x, y, w, h, sunken);
	}
	cairo_surface_mark_dirty(cs);
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
panel_toggle(struct cell *c)
{
	if (c->target_panel < 0 || c->target_panel >= n_selected_panels)
		return;
	selected_panels[c->target_panel].visible =
	    !selected_panels[c->target_panel].visible;
	if (win_w > natural_width())
		win_w = natural_width();
	if (win_h > natural_height())
		win_h = natural_height();
	update_toplevel_size();
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
    __unused struct xdg_toplevel *toplevel, int32_t w,
    int32_t h, __unused struct wl_array *states)
{
	if (cfg.resizable && w > 0 && h > 0) {
		if (w < natural_width())
			w = natural_width();
		if (h < natural_height())
			h = natural_height();
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
		if (idx >= 0 && (cells[idx].type == CELL_BUTTON ||
		    cells[idx].type == CELL_PANEL_TOGGLE)) {
			pressed_cell = idx;
			render();
		}
		return;
	}

	int idx = cell_at_point(ptr_x, ptr_y);
	int pressed = pressed_cell;
	pressed_cell = -1;
	render();
	if (pressed < 0 || idx != pressed)
		return;
	if (cells[pressed].type == CELL_BUTTON)
		ipc_send(cells[pressed].command);
	else if (cells[pressed].type == CELL_PANEL_TOGGLE)
		panel_toggle(&cells[pressed]);
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

	style_init();
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
	update_toplevel_size();
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
	for (size_t i = 0; i < sizeof(buffers) / sizeof(buffers[0]); i++)
		shm_buffer_destroy(&buffers[i]);
	for (int i = 0; i < n_cells; i++) {
		free(cells[i].style.image);
		free(cells[i].style.image_pressed);
	}
	free(style.image);
	free(style.image_pressed);
	free(config_dir);
	wl_display_disconnect(display);
	return 0;
}
