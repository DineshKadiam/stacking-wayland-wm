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

#include <fnmatch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cow/colour.h"
#include "cow/compat.h"
#include "cow/memory.h"
#include "cow/output.h"

struct cow_config *cow_config = NULL;

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
	config->desktop_config	     = COW_DESKTOP_GLOBAL;
	config->page_cols	     = COW_CFG_DEFAULT_PAGE_COLS;
	config->page_rows	     = COW_CFG_DEFAULT_PAGE_ROWS;
	config->page_scroll_animate  = false;
	config->page_scroll_ms	     = COW_CFG_DEFAULT_PAGE_SCROLL_MS;
	config->edge_scroll	     = false;
	config->edge_scroll_delay_ms = COW_CFG_DEFAULT_EDGE_SCROLL_DELAY;
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

	/*
	 * Container tab colours are intentionally left unset (has_* = false).
	 * titlebar.c falls back to the titlebar's base/dk/fg colours so that
	 * tabs inherit the window's own titlebar style automatically.  Users
	 * can override any or all four values with 'set container.tab.*'.
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
	wl_list_init(&config->view_configs);

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

	struct cow_view_config_entry *vc, *vc_tmp;
	wl_list_for_each_safe(vc, vc_tmp, &config->view_configs, link)
	{
		wl_list_remove(&vc->link);
		xfree(vc->app_id);
		xfree(vc->title);
		xfree(vc->output_name);
		xfree(vc);
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

static bool
pattern_match(const char *pattern, const char *str, bool use_glob)
{
	if (pattern == NULL || str == NULL)
		return false;
	if (use_glob)
		return fnmatch(pattern, str, 0) == 0;
	return strcmp(pattern, str) == 0;
}

struct cow_view_config_entry *
cow_config_find_view(struct cow_config *config, const char *app_id,
    const char *title, struct cow_output *output, struct cow_desk *desk)
{
	struct cow_view_config_entry *entry;
	wl_list_for_each(entry, &config->view_configs, link)
	{
		/* Check output constraint */
		if (entry->output_name != NULL && output != NULL &&
		    output->name != NULL &&
		    strcmp(entry->output_name, output->name) != 0)
			continue;

		/* Check desk constraint */
		if (entry->desk_nr >= 0 && desk != NULL &&
		    desk->nr != entry->desk_nr)
			continue;

		/* Match on title if title pattern is set */
		if (entry->title != NULL) {
			if (pattern_match(entry->title, title, entry->use_glob))
				return entry;
			continue;
		}

		/* Match on app_id */
		if (entry->app_id != NULL &&
		    pattern_match(entry->app_id, app_id, entry->use_glob))
			return entry;
	}
	return NULL;
}
