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
#include "cow/menu.h"
#include "cow/output.h"

struct cow_config *cow_config = NULL;

int
cow_config_repeat_delay_ms(void)
{
	if (cow_config != NULL && cow_config->repeat_delay_ms > 0)
		return (cow_config->repeat_delay_ms);
	return (COW_CFG_DEFAULT_REPEAT_DELAY_MS);
}

int
cow_config_repeat_interval_ms(void)
{
	if (cow_config != NULL && cow_config->repeat_interval_ms > 0)
		return (cow_config->repeat_interval_ms);
	return (COW_CFG_DEFAULT_REPEAT_INTERVAL_MS);
}

struct cow_output_background_config {
	struct wl_list link;
	char	      *name;
	float	       colour[4];
	bool	       has_colour;
	char	      *image;
	bool	       has_image;
};

struct cow_config_command {
	struct wl_list link;
	char *command;
};

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

static void
config_decor_free(struct cow_config *config)
{
	xfree(config->font_name);
	xfree(config->title_format);
	xfree(config->icon_image);
	cow_titlebar_config_fini(&config->titlebar);
	cow_container_config_fini(&config->container);
	cow_border_colour_fini(&config->border_active);
	cow_border_colour_fini(&config->border_inactive);
}

static void
border_images_detach(struct cow_border_colour *bc)
{
	bc->image = bc->image_n = bc->image_s = bc->image_e = NULL;
	bc->image_w = bc->image_nw = bc->image_ne = NULL;
	bc->image_sw = bc->image_se = NULL;
}

static void
config_decor_detach(struct cow_config *config)
{
	config->font_name = NULL;
	config->title_format = NULL;
	config->icon_image = NULL;
	config->titlebar.font_name = NULL;
	config->titlebar.active_image = NULL;
	config->titlebar.inactive_image = NULL;
	config->container.tab_active_image = NULL;
	config->container.tab_inactive_image = NULL;
	border_images_detach(&config->border_active);
	border_images_detach(&config->border_inactive);
	for (int i = 0; i < COW_TITLEBAR_NUM_BUTTONS; i++) {
		config->titlebar.buttons[i].image = NULL;
	}
}

static bool
parse_vector_coord(const char **p, int *percent, int *offset)
{
	char *end;

	*percent = (int)strtol(*p, &end, 10);
	if (end == *p)
		return false;
	*p = end;

	*offset = 0;
	if (**p == '+' || **p == '-') {
		int sign = **p == '-' ? -1 : 1;
		(*p)++;
		long n = strtol(*p, &end, 10);
		if (end == *p || *end != 'p')
			return false;
		*offset = (int)n * sign;
		*p = end + 1;
	}

	return true;
}

/*
 * Parse a vector button definition string.
 * Format: "N x1xy1@c1 x2xy2@c2 ..." where N is num_points,
 * coordinates are percentages (0-100), and c is colour index (0-4).
 * A coordinate may include a signed pixel offset, for example
 * 100-5px50+1p@0.
 */
bool
cow_parse_vector_button_string(struct cow_vector_button *vb, const char *str)
{
	int	    n = 0;
	const char *p = str;

	/* Skip leading whitespace */
	while (*p == ' ')
		p++;

	if (strcmp(p, "none") == 0) {
		vb->num_points = 0;
		return true;
	}

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

		if (!parse_vector_coord(&p, &x, &xoff))
			return false;

		/* Expect 'x' separator */
		if (*p == 'x' || *p == 'X')
			p++;
		else
			return false;

		if (!parse_vector_coord(&p, &y, &yoff))
			return false;

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
	config->clamp_mode	     = COW_CLAMP_TO_OUTPUT;
	config->desktop_config	     = COW_DESKTOP_GLOBAL;
	config->desk_count	     = COW_DEFAULT_DESK_COUNT;
	config->page_cols	     = COW_CFG_DEFAULT_PAGE_COLS;
	config->page_rows	     = COW_CFG_DEFAULT_PAGE_ROWS;
	config->page_visibility     = COW_PAGE_VISIBILITY_ORIGIN;
	config->page_scroll_animate  = false;
	config->page_scroll_ms	     = COW_CFG_DEFAULT_PAGE_SCROLL_MS;
	config->edge_scroll	      = false;
	config->edge_scroll_delay_ms = COW_CFG_DEFAULT_EDGE_SCROLL_DELAY;
	config->edge_scroll_modifiers = 0;
	config->raise_on_focus	     = true;
	config->preserve_desk_on_output_loss =
	    COW_CFG_DEFAULT_PRESERVE_DESK_ON_OUTPUT_LOSS;
	config->default_layer	     = COW_CFG_DEFAULT_LAYER;
	config->max_layer	     = COW_CFG_MAX_LAYER;
	config->repeat_delay_ms	     = COW_CFG_DEFAULT_REPEAT_DELAY_MS;
	config->repeat_interval_ms   = COW_CFG_DEFAULT_REPEAT_INTERVAL_MS;
	config->menu_repeat_keys = xstrdup(COW_CFG_DEFAULT_MENU_REPEAT_KEYS);
	const char *cursor_theme = getenv("XCURSOR_THEME");
	config->cursor_theme = xstrdup(cursor_theme != NULL &&
	    cursor_theme[0] != '\0' ? cursor_theme :
	    COW_CFG_DEFAULT_CURSOR_THEME);
	config->cursor_size = COW_CFG_DEFAULT_CURSOR_SIZE;
	const char *cursor_size = getenv("XCURSOR_SIZE");
	if (cursor_size != NULL) {
		char *end = NULL;
		long  size = strtol(cursor_size, &end, 10);

		if (end != cursor_size && *end == '\0' && size > 0 &&
		    size <= 1024)
			config->cursor_size = (int)size;
	}
	config->cursor_hide_timeout  = 0;
	config->icon_enabled	     = true;
	config->icon_size	     = COW_CFG_DEFAULT_ICON_SIZE;
	config->icon_label_area     = COW_CFG_DEFAULT_ICON_LABEL_AREA;
	config->icon_image	     = xstrdup("auto");
	config->icon_title_relief = COW_RELIEF_RAISED;
	config->snap_distance	     = COW_CFG_DEFAULT_SNAP_DISTANCE;
	cow_menu_style_default(&config->menu_style);
	border_colour_set_uniform(&config->border_active, 0xFFFFFF);
	border_colour_set_uniform(&config->border_inactive, 0x465457);
	cow_colour_convert(config->icon_background, 0x828282);
	cow_colour_convert(config->icon_foreground, 0xFFFFFF);

	config->font_name	= xstrdup("monospace 10");
	config->title_format	= xstrdup("[%t] %n");
	config->focus_mode	= COW_FOCUS_MODE_CLICK;
	config->reserved.top	= 0;
	config->reserved.bottom = 0;
	config->reserved.left	= 0;
	config->reserved.right	= 0;

	/* Default titlebar config */
	config->titlebar.enabled   = false;
	config->titlebar.font_name = NULL;
	config->titlebar.position  = COW_TITLEBAR_TOP;
	config->titlebar.active_title_justify = COW_TITLE_JUST_CENTRE;
	config->titlebar.inactive_title_justify = COW_TITLE_JUST_CENTRE;
	config->titlebar.button_style = COW_TITLEBAR_BUTTON_STYLE_INHERIT;
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

	/* Default buttons: close on 1, iconify on 2, maximize on 0. */
	{
		struct cow_vector_button *v =
		    &config->titlebar.buttons[1].vector;
		v->num_points = 5;
		v->points[0]  = (struct cow_vector_point) { 25, 25, 0, 0, -1 };
		v->points[1]  = (struct cow_vector_point) { 75, 75, 0, 0, 3 };
		v->points[2]  = (struct cow_vector_point) { 75, 25, 0, 0, -1 };
		v->points[3]  = (struct cow_vector_point) { 25, 75, 0, 0, 3 };
		v->points[4]  = (struct cow_vector_point) { 25, 25, 0, 0, -1 };
	}
	{
		struct cow_vector_button *v =
		    &config->titlebar.buttons[0].vector;
		v->num_points = 5;
		v->points[0]  = (struct cow_vector_point) { 25, 25, 0, 0, -1 };
		v->points[1]  = (struct cow_vector_point) { 25, 75, 0, 0, 3 };
		v->points[2]  = (struct cow_vector_point) { 75, 75, 0, 0, 3 };
		v->points[3]  = (struct cow_vector_point) { 75, 25, 0, 0, 3 };
		v->points[4]  = (struct cow_vector_point) { 25, 25, 0, 0, 3 };
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
	}

	wl_list_init(&config->keyboard_bindings);
	wl_list_init(&config->mouse_bindings);
	wl_list_init(&config->decoration_bindings);
	wl_list_init(&config->output_backgrounds);
	wl_list_init(&config->configured_commands);
	cow_icon_placement_config_init(config);

	return config;
}

void
cow_config_reset_decor(struct cow_config *config)
{
	struct cow_config *defaults;

	if (config == NULL)
		return;

	defaults = cow_config_create();
	config_decor_free(config);

	config->border = defaults->border;
	config->corner_length = defaults->corner_length;
	config->handles = defaults->handles;
	config->border_style = defaults->border_style;
	config->border_active = defaults->border_active;
	config->border_inactive = defaults->border_inactive;
	memcpy(config->border_resize, defaults->border_resize,
	    sizeof(config->border_resize));
	config->has_border_resize = defaults->has_border_resize;
	config->font_name = defaults->font_name;
	config->title_format = defaults->title_format;
	config->icon_enabled = defaults->icon_enabled;
	config->icon_image = defaults->icon_image;
	config->icon_size = defaults->icon_size;
	config->icon_label_area = defaults->icon_label_area;
	memcpy(config->icon_background, defaults->icon_background,
	    sizeof(config->icon_background));
	memcpy(config->icon_foreground, defaults->icon_foreground,
	    sizeof(config->icon_foreground));
	config->icon_title_relief = defaults->icon_title_relief;
	config->titlebar = defaults->titlebar;
	config->container = defaults->container;

	config_decor_detach(defaults);
	cow_config_destroy(defaults);
}

void
cow_config_destroy(struct cow_config *config)
{
	if (config == NULL)
		return;

	config_decor_free(config);
	cow_icon_placement_config_fini(config);

	struct cow_binding_entry *kb, *kb_tmp;
	wl_list_for_each_safe(kb, kb_tmp, &config->keyboard_bindings, link)
	{
		wl_list_remove(&kb->link);
		xfree(kb->key_string);
		xfree(kb->action_string);
		xfree(kb->release_action_string);
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

	struct cow_output_background_config *ob, *ob_tmp;
	wl_list_for_each_safe(ob, ob_tmp, &config->output_backgrounds, link)
	{
		wl_list_remove(&ob->link);
		xfree(ob->name);
		xfree(ob->image);
		xfree(ob);
	}
	struct cow_config_command *command, *command_tmp;
	wl_list_for_each_safe(command, command_tmp, &config->configured_commands,
	    link)
	{
		wl_list_remove(&command->link);
		xfree(command->command);
		xfree(command);
	}
	xfree(config->output_bg_image);
	xfree(config->menu_style.icon_path);
	xfree(config->menu_style.side_image);
	xfree(config->menu_style.font);
	xfree(config->menu_repeat_keys);
	xfree(config->cursor_theme);

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

bool
cow_config_get_output_background(struct cow_config *config, const char *name,
    float colour[4])
{
	if (config == NULL || name == NULL)
		return false;

	struct cow_output_background_config *ob;
	wl_list_for_each(ob, &config->output_backgrounds, link)
	{
		if (strcmp(ob->name, name) != 0)
			continue;
		if (!ob->has_colour)
			return false;
		memcpy(colour, ob->colour, sizeof(float) * 4);
		return true;
	}

	return false;
}

void
cow_config_set_output_background(struct cow_config *config, const char *name,
    const float colour[4])
{
	if (config == NULL || name == NULL || colour == NULL)
		return;

	struct cow_output_background_config *ob;
	wl_list_for_each(ob, &config->output_backgrounds, link)
	{
		if (strcmp(ob->name, name) != 0)
			continue;
		memcpy(ob->colour, colour, sizeof(float) * 4);
		ob->has_colour = true;
		return;
	}

	ob = xcalloc(1, sizeof(*ob));
	ob->name = xstrdup(name);
	memcpy(ob->colour, colour, sizeof(float) * 4);
	ob->has_colour = true;
	wl_list_insert(&config->output_backgrounds, &ob->link);
}

bool
cow_config_get_output_background_image(struct cow_config *config,
    const char *name, const char **image)
{
	if (config == NULL || name == NULL || image == NULL)
		return false;

	struct cow_output_background_config *ob;
	wl_list_for_each(ob, &config->output_backgrounds, link)
	{
		if (strcmp(ob->name, name) != 0)
			continue;
		if (!ob->has_image)
			return false;
		*image = ob->image;
		return true;
	}

	return false;
}

void
cow_config_set_output_background_image(struct cow_config *config,
    const char *name, const char *image)
{
	if (config == NULL || name == NULL)
		return;

	struct cow_output_background_config *ob;
	wl_list_for_each(ob, &config->output_backgrounds, link)
	{
		if (strcmp(ob->name, name) != 0)
			continue;
		xstrset(&ob->image, image);
		ob->has_image = true;
		return;
	}

	ob = xcalloc(1, sizeof(*ob));
	ob->name = xstrdup(name);
	ob->image = image != NULL ? xstrdup(image) : NULL;
	ob->has_image = true;
	wl_list_insert(&config->output_backgrounds, &ob->link);
}

void
cow_config_command_add(struct cow_config *config, const char *text)
{
	if (config == NULL || text == NULL || text[0] == '\0')
		return;
	struct cow_config_command *command = xcalloc(1, sizeof(*command));
	command->command = xstrdup(text);
	wl_list_insert(config->configured_commands.prev, &command->link);
}

char *
cow_config_commands_string(const struct cow_config *config)
{
	char *result = xstrdup("");
	size_t len = 0;
	if (config == NULL)
		return result;
	struct cow_config_command *command;
	wl_list_for_each(command, &config->configured_commands, link) {
		size_t add = strlen(command->command);
		result = xrealloc(result, 1, len + add + 2);
		memcpy(result + len, command->command, add);
		len += add;
		result[len++] = '\n';
		result[len] = '\0';
	}
	return result;
}

void
cow_config_visit_commands(const struct cow_config *config,
    cow_config_command_cb callback, void *data)
{
	if (config == NULL || callback == NULL)
		return;
	struct cow_config_command *command;
	wl_list_for_each(command, &config->configured_commands, link)
		callback(command->command, data);
}

/*
 * Decoration-region helpers.
 *
 * Region names are matched case-insensitively.  Both compass shorthands
 * (N / S / E / W / NE / NW / SE / SW) and full names (north / south /
 * east / west / north-east etc.) are accepted, plus "titlebar" / "title"
 * for the title bar zone and 0 through 9 for titlebar buttons.
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
	{ COW_DEC_REGION_BUTTON_0, "0",        NULL         },
	{ COW_DEC_REGION_BUTTON_1, "1",        NULL         },
	{ COW_DEC_REGION_BUTTON_2, "2",        NULL         },
	{ COW_DEC_REGION_BUTTON_3, "3",        NULL         },
	{ COW_DEC_REGION_BUTTON_4, "4",        NULL         },
	{ COW_DEC_REGION_BUTTON_5, "5",        NULL         },
	{ COW_DEC_REGION_BUTTON_6, "6",        NULL         },
	{ COW_DEC_REGION_BUTTON_7, "7",        NULL         },
	{ COW_DEC_REGION_BUTTON_8, "8",        NULL         },
	{ COW_DEC_REGION_BUTTON_9, "9",        NULL         },
};

enum cow_dec_region
cow_dec_region_button(int idx)
{
	if (idx < 0 || idx >= COW_TITLEBAR_NUM_BUTTONS)
		return COW_DEC_REGION_NONE;
	return (enum cow_dec_region)(COW_DEC_REGION_BUTTON_0 + idx);
}

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
