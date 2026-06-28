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
#include "cow/config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "cow/colour.h"
#include "cow/compat.h"
#include "cow/memory.h"
#include "cow/output.h"

struct cow_config *cow_config = NULL;

bool
cow_parse_squeeze_mode(const char *s, enum cow_squeeze_mode *out)
{
	if (s == NULL)
		return false;
	if (strcmp(s, "none") == 0)
		*out = COW_SQUEEZE_NONE;
	else if (strcmp(s, "natural") == 0)
		*out = COW_SQUEEZE_NATURAL;
	else if (strcmp(s, "fixed") == 0)
		*out = COW_SQUEEZE_FIXED;
	else
		return false;
	return true;
}

bool
cow_parse_squeeze_just(const char *s, enum cow_squeeze_just *out)
{
	if (s == NULL)
		return false;
	if (strcmp(s, "left") == 0)
		*out = COW_JUST_LEFT;
	else if (strcmp(s, "centre") == 0 || strcmp(s, "center") == 0)
		*out = COW_JUST_CENTRE;
	else if (strcmp(s, "right") == 0)
		*out = COW_JUST_RIGHT;
	else
		return false;
	return true;
}

/*
 * Set all 8 edges of a border_colour to a uniform colour.
 */
static void
border_colour_set_uniform(struct cow_border_colour *bc, uint32_t colour)
{
	cow_colour_convert(bc->n, colour);
	cow_colour_convert(bc->s, colour);
	cow_colour_convert(bc->e, colour);
	cow_colour_convert(bc->w, colour);
	memcpy(bc->nw, bc->n, sizeof(float) * 4);
	memcpy(bc->ne, bc->n, sizeof(float) * 4);
	memcpy(bc->sw, bc->n, sizeof(float) * 4);
	memcpy(bc->se, bc->n, sizeof(float) * 4);
	bc->has_corners = false;
	bc->has_nw = bc->has_ne = bc->has_sw = bc->has_se = false;
}

/*
 * Parse a vector button definition string.
 * Format: "N x1xy1@c1 x2xy2@c2 ..." where N is num_points,
 * coordinates are percentages (0-100), and c is colour index (0-4).
 * Optional pixel offsets not yet supported in this parser.
 */
bool
cow_parse_vector_button_string(struct cow_vector_button *vb, const char *str)
{
	int	    n = 0;
	const char *p = str;

	/* Skip leading whitespace */
	while (*p == ' ')
		p++;

	/* Parse point count */
	n = (int)strtol(p, (char **)&p, 10);
	if (n <= 0 || n > COW_VECTOR_MAX_POINTS)
		return false;

	vb->num_points = n;

	for (int i = 0; i < n; i++) {
		while (*p == ' ')
			p++;
		if (*p == '\0')
			break;

		int x = 0, y = 0, xoff = 0, yoff = 0, colour = -1;

		/* Parse x coordinate */
		x = (int)strtol(p, (char **)&p, 10);

		/* Expect 'x' separator */
		if (*p == 'x' || *p == 'X')
			p++;

		/* Parse y coordinate */
		y = (int)strtol(p, (char **)&p, 10);

		/* Optional '@' for colour */
		if (*p == '@') {
			p++;
			colour = (int)strtol(p, (char **)&p, 10);
		}

		vb->points[i].x	     = x;
		vb->points[i].y	     = y;
		vb->points[i].xoff   = xoff;
		vb->points[i].yoff   = yoff;
		vb->points[i].colour = colour;
	}

	return true;
}

struct cow_config *
cow_config_create(void)
{
	struct cow_config *config = xcalloc(1, sizeof(*config));

	/* Defaults matching original cow */
	config->border		     = COW_CFG_DEFAULT_BORDER;
	config->gap		     = COW_CFG_DEFAULT_GAP;
	config->step		     = COW_CFG_DEFAULT_STEP;
	config->corner_length	     = COW_CFG_DEFAULT_CORNER_LENGTH;
	config->handles		     = true;
	config->border_style	     = COW_BORDER_STYLE_NONE;
	config->placement	     = COW_PLACEMENT_CASCADE;
	config->geometry_window	     = COW_GEOMETRY_WINDOW_BOTH;
	config->desktop_config	     = COW_DESKTOP_GLOBAL;
	config->desk_count	     = COW_DEFAULT_DESK_COUNT;
	config->page_cols	     = COW_CFG_DEFAULT_PAGE_COLS;
	config->page_rows	     = COW_CFG_DEFAULT_PAGE_ROWS;
	config->page_scroll_animate  = false;
	config->page_scroll_ms	     = COW_CFG_DEFAULT_PAGE_SCROLL_MS;
	config->edge_scroll	      = false;
	config->edge_scroll_delay_ms = COW_CFG_DEFAULT_EDGE_SCROLL_DELAY;
	config->raise_on_focus	     = true;
	config->preserve_desk_on_output_loss =
	    COW_CFG_DEFAULT_PRESERVE_DESK_ON_OUTPUT_LOSS;
	config->default_layer	     = COW_CFG_DEFAULT_LAYER;
	config->max_layer	     = COW_CFG_MAX_LAYER;
	config->repeat_delay_ms	     = COW_CFG_DEFAULT_REPEAT_DELAY_MS;
	config->repeat_interval_ms   = COW_CFG_DEFAULT_REPEAT_INTERVAL_MS;
	config->icon_size	     = COW_CFG_DEFAULT_ICON_SIZE;
	config->snap_distance	     = COW_CFG_DEFAULT_SNAP_DISTANCE;

	border_colour_set_uniform(&config->border_active, 0xFFFFFF);
	border_colour_set_uniform(&config->border_inactive, 0x465457);

	config->font_name	= xstrdup("monospace 10");
	config->focus_mode	= COW_FOCUS_MODE_CLICK;
	config->reserved.top	= 0;
	config->reserved.bottom = 0;
	config->reserved.left	= 0;
	config->reserved.right	= 0;

	/* Default titlebar config */
	config->titlebar.enabled   = false;
	config->titlebar.font_name = NULL;
	cow_colour_convert(config->titlebar.active_colour, 0xC0C0C0);
	cow_colour_convert(config->titlebar.inactive_colour, 0xA0A0A0);
	cow_colour_convert(config->titlebar.fg_active, 0x000000);
	cow_colour_convert(config->titlebar.fg_inactive, 0x000000);

	/* Default squeeze config: disabled. */
	config->titlebar.squeeze.mode	   = COW_SQUEEZE_NONE;
	config->titlebar.squeeze.justify   = COW_JUST_LEFT;
	config->titlebar.squeeze.width	   = COW_CFG_DEFAULT_SQUEEZE_WIDTH;
	config->titlebar.squeeze.offset	   = 0;
	config->titlebar.squeeze.min_width = COW_CFG_DEFAULT_SQUEEZE_MIN_WIDTH;

	/*
	 * Container tab colours are intentionally left unset (has_* = false).
	 * titlebar.c falls back to the titlebar's base/dk/fg colours so that
	 * tabs inherit the window's own titlebar style automatically.  Users
	 * can override any or all four values with decor profiles.
	 */

	/* Default button vectors: close (X), maximize (rect), iconify (bar) */
	{
		struct cow_vector_button *v =
		    &config->titlebar.buttons[0].vector;
		v->num_points = 5;
		v->points[0]  = (struct cow_vector_point) { 25, 25, 0, 0, -1 };
		v->points[1]  = (struct cow_vector_point) { 75, 75, 0, 0, 3 };
		v->points[2]  = (struct cow_vector_point) { 75, 25, 0, 0, -1 };
		v->points[3]  = (struct cow_vector_point) { 25, 75, 0, 0, 3 };
		v->points[4]  = (struct cow_vector_point) { 25, 25, 0, 0, -1 };
		config->titlebar.buttons[0].action = COW_BUTTON_ACTION_CLOSE;
	}
	{
		struct cow_vector_button *v =
		    &config->titlebar.buttons[1].vector;
		v->num_points = 5;
		v->points[0]  = (struct cow_vector_point) { 25, 25, 0, 0, -1 };
		v->points[1]  = (struct cow_vector_point) { 25, 75, 0, 0, 3 };
		v->points[2]  = (struct cow_vector_point) { 75, 75, 0, 0, 3 };
		v->points[3]  = (struct cow_vector_point) { 75, 25, 0, 0, 3 };
		v->points[4]  = (struct cow_vector_point) { 25, 25, 0, 0, 3 };
		config->titlebar.buttons[1].action = COW_BUTTON_ACTION_MAXIMIZE;
	}
	{
		struct cow_vector_button *v =
		    &config->titlebar.buttons[2].vector;
		v->num_points = 5;
		v->points[0]  = (struct cow_vector_point) { 25, 65, 0, 0, -1 };
		v->points[1]  = (struct cow_vector_point) { 75, 65, 0, 0, 3 };
		v->points[2]  = (struct cow_vector_point) { 75, 75, 0, 0, 3 };
		v->points[3]  = (struct cow_vector_point) { 25, 75, 0, 0, 3 };
		v->points[4]  = (struct cow_vector_point) { 25, 65, 0, 0, 3 };
		config->titlebar.buttons[2].action = COW_BUTTON_ACTION_ICONIFY;
	}

	wl_list_init(&config->keyboard_bindings);
	wl_list_init(&config->mouse_bindings);
	wl_list_init(&config->decoration_bindings);

	return config;
}

void
cow_config_destroy(struct cow_config *config)
{
	if (config == NULL)
		return;

	xfree(config->font_name);
	xfree(config->titlebar.font_name);

	struct cow_binding_entry *kb, *kb_tmp;
	wl_list_for_each_safe(kb, kb_tmp, &config->keyboard_bindings, link)
	{
		wl_list_remove(&kb->link);
		xfree(kb->key_string);
		xfree(kb->action_string);
		xfree(kb);
	}

	struct cow_mouse_binding_entry *mb, *mb_tmp;
	wl_list_for_each_safe(mb, mb_tmp, &config->mouse_bindings, link)
	{
		wl_list_remove(&mb->link);
		xfree(mb->key_string);
		xfree(mb->action_string);
		xfree(mb);
	}

	struct cow_dec_binding_entry *db, *db_tmp;
	wl_list_for_each_safe(db, db_tmp, &config->decoration_bindings, link)
	{
		wl_list_remove(&db->link);
		xfree(db->action_string);
		xfree(db);
	}

	xfree(config);
}

const char *
cow_config_default_path(void)
{
	static char path[4096];

	/* $COW_CONFIG */
	const char *env = getenv("COW_CONFIG");
	if (env != NULL && access(env, R_OK) == 0)
		return env;

	/* ~/cow.conf (direct) */
	const char *home = getenv("HOME");
	if (home != NULL) {
		snprintf(path, sizeof(path), "%s/cow.conf", home);
		if (access(path, R_OK) == 0)
			return path;
	}

	/* $XDG_CONFIG_HOME/cow/cow.conf */
	const char *xdg = getenv("XDG_CONFIG_HOME");
	if (xdg != NULL) {
		snprintf(path, sizeof(path), "%s/cow/cow.conf", xdg);
		if (access(path, R_OK) == 0)
			return path;
	}

	/* ~/.config/cow/cow.conf */
	if (home != NULL) {
		snprintf(path, sizeof(path), "%s/.config/cow/cow.conf", home);
		if (access(path, R_OK) == 0)
			return path;
	}

	/* Compiled-in sysconfdir (e.g. /usr/local/etc/cow/cow.conf) */
	if (access(COW_SYSCONFDIR "/cow.conf", R_OK) == 0)
		return COW_SYSCONFDIR "/cow.conf";

	return NULL;
}

/*
 * Decoration-region helpers.
 *
 * Region names are matched case-insensitively.  Both compass shorthands
 * (N / S / E / W / NE / NW / SE / SW) and full names (north / south /
 * east / west / north-east etc.) are accepted, plus "titlebar" / "title"
 * for the title bar zone.
 */
static const struct {
	enum cow_dec_region region;
	const char	   *primary; /* canonical name returned by _name */
	const char	   *alt;     /* additional accepted spelling */
} region_table[] = {
	{ COW_DEC_REGION_N,	    "n",	 "north"	 },
	{ COW_DEC_REGION_S,	    "s",	 "south"	 },
	{ COW_DEC_REGION_E,	    "e",	 "east"	},
	{ COW_DEC_REGION_W,	    "w",	 "west"	},
	{ COW_DEC_REGION_NE,	     "ne",	   "north-east" },
	{ COW_DEC_REGION_NW,	     "nw",	   "north-west" },
	{ COW_DEC_REGION_SE,	     "se",	   "south-east" },
	{ COW_DEC_REGION_SW,	     "sw",	   "south-west" },
	{ COW_DEC_REGION_TITLEBAR, "titlebar", "title"      },
};

enum cow_dec_region
cow_dec_region_parse(const char *s)
{
	if (s == NULL || *s == '\0')
		return COW_DEC_REGION_NONE;
	for (size_t i = 0; i < sizeof(region_table) / sizeof(region_table[0]);
	    i++) {
		if (strcasecmp(s, region_table[i].primary) == 0 ||
		    (region_table[i].alt != NULL &&
			strcasecmp(s, region_table[i].alt) == 0))
			return region_table[i].region;
	}
	return COW_DEC_REGION_NONE;
}

const char *
cow_dec_region_name(enum cow_dec_region r)
{
	for (size_t i = 0; i < sizeof(region_table) / sizeof(region_table[0]);
	    i++) {
		if (region_table[i].region == r)
			return region_table[i].primary;
	}
	return "none";
}

const char *
cow_dec_binding_lookup(enum cow_dec_region region, int clicks)
{
	if (cow_config == NULL || region == COW_DEC_REGION_NONE)
		return NULL;
	if (clicks != 1 && clicks != 2)
		return NULL;
	struct cow_dec_binding_entry *e;
	wl_list_for_each(e, &cow_config->decoration_bindings, link)
	{
		if (e->region == region && e->clicks == clicks)
			return e->action_string;
	}
	return NULL;
}
