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
 * Command: set -- update configuration fields at runtime.
 *
 * usage: set [-v] key value
 */

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cow/compat.h"
#include "cow/cmd.h"
#include "cow/colour.h"
#include "cow/config.h"
#include "cow/desk_ops.h"
#include "cow/font.h"
#include "cow/gradient.h"
#include "cow/image.h"
#include "cow/ipc.h"
#include "cow/memory.h"
#include "cow/options.h"
#include "cow/output.h"
#include "cow/pan_frames.h"
#include "cow/shm.h"
#include "cow/stack.h"
#include "cow/status.h"
#include "cow/titlebar.h"
#include "cow/view.h"
#include "cow/wm.h"

static enum cmd_retval cmd_set_exec(struct cmd *, struct cmd_q *);
static enum cmd_retval set_option_exec(const struct cow_option_entry *, const char *,
    const char *, struct args *, struct cmd_q *);
static const struct cow_option_entry *set_option_find(const char *);
static bool	       set_option_scope_ok(const struct cow_option_entry *,
			   struct cmd_q *);
static enum cmd_retval set_scalar_bool(const struct cow_option_entry *,
    const char *, struct cmd_q *);
static bool	       set_option_is_decor(const struct cow_option_entry *,
			   const char *);
static enum cmd_retval set_scalar_int(const struct cow_option_entry *, const char *,
    const char *, struct cmd_q *);
static enum cmd_retval set_desks_count(const char *, struct cmd_q *);
static enum cmd_retval set_desktop_size(struct args *, struct cmd_q *);
static enum cmd_retval set_titlebar_height(const char *, struct cmd_q *);
static enum cmd_retval set_titlebar_squeeze(const struct cow_option_entry *,
    const char *, const char *, struct cmd_q *);
static enum cmd_retval set_layer(const char *, const char *, struct cmd_q *);
static void finish_visual_update(enum cow_view_cfg_option,
    struct cow_view_cfg *);
static struct cow_titlebar_config *target_titlebar(struct cmd_q *,
    struct cow_view_cfg **);
static struct cow_container_config *target_container(struct cmd_q *,
    struct cow_view_cfg **);
static void set_colour_option(const char *, const char *, struct cmd_q *);
static enum cmd_retval set_image_option(const char *, const char *,
    struct cmd_q *);
static bool set_gradient_option(const char *, const char *, struct cmd_q *);
static bool set_output_colour(const char *, const char *, struct cmd_q *);
static bool set_border_colour(const char *, const char *, struct cmd_q *);
static bool set_icon_colour(const char *, const char *, struct cmd_q *);
static bool set_titlebar_colour(const char *, const char *, struct cmd_q *);
static bool set_container_tab_colour(const char *, const char *, struct cmd_q *);
static void set_border_style_option(const char *, struct cmd_q *);
static void set_desktop_config_option(const char *, struct cmd_q *);
static void set_desktop_config_global(void);
static void set_desktop_config_shared(void);
static bool desk_nr_used_except(struct cow_output *, uint8_t);
static void set_focus_option(const char *);
static void set_geometry_window_option(const char *, struct cmd_q *);
static void set_placement_option(const char *, struct cmd_q *);
static void set_font_option(const char *, struct cmd_q *);
static void set_button_option(const char *, const char *, struct cmd_q *);
static bool set_key_is_image(const char *);
static char *set_image_resolve_path(const char *, struct cmd_q *);
static void refresh_visuals(void);
static int  refreshed_title_thickness(struct cow_view *);
static void refresh_view_visuals(struct cow_view *);
static void refresh_output_visuals(struct cow_output *);

struct set_titlebar_colour_option {
	const char *name;
	enum cow_view_cfg_option id;
	size_t	    offset;
};

struct set_container_colour_option {
	const char *name;
	enum cow_view_cfg_option id;
	size_t	    colour_offset;
	size_t	    has_offset;
};

struct set_border_edge {
	const char *name;
	enum cow_view_cfg_option active_id;
	enum cow_view_cfg_option inactive_id;
	enum cow_view_cfg_option active_gradient_id;
	enum cow_view_cfg_option inactive_gradient_id;
	enum cow_view_cfg_option active_image_id;
	enum cow_view_cfg_option inactive_image_id;
	size_t	    colour_offset;
	size_t	    image_offset;
	size_t	    has_offset;
};

struct set_enum_option {
	const char *name;
	int	    value;
};

static const struct set_enum_option set_border_style_options[] = {
	{ "fvwm", COW_BORDER_STYLE_FVWM },
	{ "mwm", COW_BORDER_STYLE_MWM },
	{ NULL, COW_BORDER_STYLE_NONE },
};

static const struct set_enum_option set_focus_options[] = {
	{ "sloppy", COW_FOCUS_MODE_SLOPPY },
	{ NULL, COW_FOCUS_MODE_CLICK },
};

static const struct set_enum_option set_geometry_window_options[] = {
	{ "both", COW_GEOMETRY_WINDOW_BOTH },
	{ "move", COW_GEOMETRY_WINDOW_MOVE },
	{ "resize", COW_GEOMETRY_WINDOW_RESIZE },
	{ "none", COW_GEOMETRY_WINDOW_NONE },
	{ NULL, -1 },
};

static const struct set_enum_option set_placement_options[] = {
	{ "cascade", COW_PLACEMENT_CASCADE },
	{ "undermouse", COW_PLACEMENT_UNDERMOUSE },
	{ "centre", COW_PLACEMENT_CENTRE },
	{ "center", COW_PLACEMENT_CENTRE },
	{ NULL, -1 },
};

static const struct set_titlebar_colour_option set_titlebar_colour_options[] = {
	{
		.name = "titlebar.active_colour",
		.id = COW_VIEW_CFG_TITLEBAR_ACTIVE_COLOUR,
		.offset = offsetof(struct cow_titlebar_config, active_colour),
	},
	{
		.name = "titlebar.inactive_colour",
		.id = COW_VIEW_CFG_TITLEBAR_INACTIVE_COLOUR,
		.offset = offsetof(struct cow_titlebar_config, inactive_colour),
	},
	{
		.name = "titlebar.fg_active",
		.id = COW_VIEW_CFG_TITLEBAR_FG_ACTIVE,
		.offset = offsetof(struct cow_titlebar_config, fg_active),
	},
	{
		.name = "titlebar.fg_inactive",
		.id = COW_VIEW_CFG_TITLEBAR_FG_INACTIVE,
		.offset = offsetof(struct cow_titlebar_config, fg_inactive),
	},
	{ .name = NULL },
};

static const struct set_container_colour_option
    set_container_colour_options[] = {
	{
		.name = "container.tab.active_colour",
		.id = COW_VIEW_CFG_CONTAINER_TAB_ACTIVE_COLOUR,
		.colour_offset = offsetof(struct cow_container_config,
		    tab_active),
		.has_offset = offsetof(struct cow_container_config,
		    has_tab_active),
	},
	{
		.name = "container.tab.inactive_colour",
		.id = COW_VIEW_CFG_CONTAINER_TAB_INACTIVE_COLOUR,
		.colour_offset = offsetof(struct cow_container_config,
		    tab_inactive),
		.has_offset = offsetof(struct cow_container_config,
		    has_tab_inactive),
	},
	{
		.name = "container.tab.fg_active",
		.id = COW_VIEW_CFG_CONTAINER_TAB_FG_ACTIVE,
		.colour_offset = offsetof(struct cow_container_config,
		    tab_fg_active),
		.has_offset = offsetof(struct cow_container_config,
		    has_tab_fg_active),
	},
	{
		.name = "container.tab.fg_inactive",
		.id = COW_VIEW_CFG_CONTAINER_TAB_FG_INACTIVE,
		.colour_offset = offsetof(struct cow_container_config,
		    tab_fg_inactive),
		.has_offset = offsetof(struct cow_container_config,
		    has_tab_fg_inactive),
	},
	{ .name = NULL },
};

static const struct set_border_edge set_border_edges[] = {
	{ .name = "n", .active_id = COW_VIEW_CFG_BORDER_ACTIVE_N,
	    .inactive_id = COW_VIEW_CFG_BORDER_INACTIVE_N,
	    .active_gradient_id = COW_VIEW_CFG_BORDER_ACTIVE_GRADIENT_N,
	    .inactive_gradient_id = COW_VIEW_CFG_BORDER_INACTIVE_GRADIENT_N,
	    .active_image_id = COW_VIEW_CFG_BORDER_ACTIVE_IMAGE_N,
	    .inactive_image_id = COW_VIEW_CFG_BORDER_INACTIVE_IMAGE_N,
	    .colour_offset = offsetof(struct cow_border_colour, n),
	    .image_offset = offsetof(struct cow_border_colour, image_n),
	    .has_offset = COW_OPTION_NO_OFFSET },
	{ .name = "s", .active_id = COW_VIEW_CFG_BORDER_ACTIVE_S,
	    .inactive_id = COW_VIEW_CFG_BORDER_INACTIVE_S,
	    .active_gradient_id = COW_VIEW_CFG_BORDER_ACTIVE_GRADIENT_S,
	    .inactive_gradient_id = COW_VIEW_CFG_BORDER_INACTIVE_GRADIENT_S,
	    .active_image_id = COW_VIEW_CFG_BORDER_ACTIVE_IMAGE_S,
	    .inactive_image_id = COW_VIEW_CFG_BORDER_INACTIVE_IMAGE_S,
	    .colour_offset = offsetof(struct cow_border_colour, s),
	    .image_offset = offsetof(struct cow_border_colour, image_s),
	    .has_offset = COW_OPTION_NO_OFFSET },
	{ .name = "e", .active_id = COW_VIEW_CFG_BORDER_ACTIVE_E,
	    .inactive_id = COW_VIEW_CFG_BORDER_INACTIVE_E,
	    .active_gradient_id = COW_VIEW_CFG_BORDER_ACTIVE_GRADIENT_E,
	    .inactive_gradient_id = COW_VIEW_CFG_BORDER_INACTIVE_GRADIENT_E,
	    .active_image_id = COW_VIEW_CFG_BORDER_ACTIVE_IMAGE_E,
	    .inactive_image_id = COW_VIEW_CFG_BORDER_INACTIVE_IMAGE_E,
	    .colour_offset = offsetof(struct cow_border_colour, e),
	    .image_offset = offsetof(struct cow_border_colour, image_e),
	    .has_offset = COW_OPTION_NO_OFFSET },
	{ .name = "w", .active_id = COW_VIEW_CFG_BORDER_ACTIVE_W,
	    .inactive_id = COW_VIEW_CFG_BORDER_INACTIVE_W,
	    .active_gradient_id = COW_VIEW_CFG_BORDER_ACTIVE_GRADIENT_W,
	    .inactive_gradient_id = COW_VIEW_CFG_BORDER_INACTIVE_GRADIENT_W,
	    .active_image_id = COW_VIEW_CFG_BORDER_ACTIVE_IMAGE_W,
	    .inactive_image_id = COW_VIEW_CFG_BORDER_INACTIVE_IMAGE_W,
	    .colour_offset = offsetof(struct cow_border_colour, w),
	    .image_offset = offsetof(struct cow_border_colour, image_w),
	    .has_offset = COW_OPTION_NO_OFFSET },
	{ .name = "nw", .active_id = COW_VIEW_CFG_BORDER_ACTIVE_NW,
	    .inactive_id = COW_VIEW_CFG_BORDER_INACTIVE_NW,
	    .active_gradient_id = COW_VIEW_CFG_BORDER_ACTIVE_GRADIENT_NW,
	    .inactive_gradient_id = COW_VIEW_CFG_BORDER_INACTIVE_GRADIENT_NW,
	    .active_image_id = COW_VIEW_CFG_BORDER_ACTIVE_IMAGE_NW,
	    .inactive_image_id = COW_VIEW_CFG_BORDER_INACTIVE_IMAGE_NW,
	    .colour_offset = offsetof(struct cow_border_colour, nw),
	    .image_offset = offsetof(struct cow_border_colour, image_nw),
	    .has_offset = offsetof(struct cow_border_colour, has_nw) },
	{ .name = "ne", .active_id = COW_VIEW_CFG_BORDER_ACTIVE_NE,
	    .inactive_id = COW_VIEW_CFG_BORDER_INACTIVE_NE,
	    .active_gradient_id = COW_VIEW_CFG_BORDER_ACTIVE_GRADIENT_NE,
	    .inactive_gradient_id = COW_VIEW_CFG_BORDER_INACTIVE_GRADIENT_NE,
	    .active_image_id = COW_VIEW_CFG_BORDER_ACTIVE_IMAGE_NE,
	    .inactive_image_id = COW_VIEW_CFG_BORDER_INACTIVE_IMAGE_NE,
	    .colour_offset = offsetof(struct cow_border_colour, ne),
	    .image_offset = offsetof(struct cow_border_colour, image_ne),
	    .has_offset = offsetof(struct cow_border_colour, has_ne) },
	{ .name = "sw", .active_id = COW_VIEW_CFG_BORDER_ACTIVE_SW,
	    .inactive_id = COW_VIEW_CFG_BORDER_INACTIVE_SW,
	    .active_gradient_id = COW_VIEW_CFG_BORDER_ACTIVE_GRADIENT_SW,
	    .inactive_gradient_id = COW_VIEW_CFG_BORDER_INACTIVE_GRADIENT_SW,
	    .active_image_id = COW_VIEW_CFG_BORDER_ACTIVE_IMAGE_SW,
	    .inactive_image_id = COW_VIEW_CFG_BORDER_INACTIVE_IMAGE_SW,
	    .colour_offset = offsetof(struct cow_border_colour, sw),
	    .image_offset = offsetof(struct cow_border_colour, image_sw),
	    .has_offset = offsetof(struct cow_border_colour, has_sw) },
	{ .name = "se", .active_id = COW_VIEW_CFG_BORDER_ACTIVE_SE,
	    .inactive_id = COW_VIEW_CFG_BORDER_INACTIVE_SE,
	    .active_gradient_id = COW_VIEW_CFG_BORDER_ACTIVE_GRADIENT_SE,
	    .inactive_gradient_id = COW_VIEW_CFG_BORDER_INACTIVE_GRADIENT_SE,
	    .active_image_id = COW_VIEW_CFG_BORDER_ACTIVE_IMAGE_SE,
	    .inactive_image_id = COW_VIEW_CFG_BORDER_INACTIVE_IMAGE_SE,
	    .colour_offset = offsetof(struct cow_border_colour, se),
	    .image_offset = offsetof(struct cow_border_colour, image_se),
	    .has_offset = offsetof(struct cow_border_colour, has_se) },
	{ .name = NULL },
};


static struct cow_view_cfg *
cfg_target(struct cmd_q *cmdq)
{
	if (cmdq == NULL || cmdq->target.window == NULL)
		return NULL;
	return &cmdq->target.window->cfg;
}

static void
propagate_global_option(enum cow_view_cfg_option option)
{
	if (cow_ipc.startup_mode)
		return;
	struct cow_view *view;
	wl_list_for_each(view, &cow_wm.views, wm_link)
	{
		if (!view->closed)
			cow_view_cfg_apply_global_option(&view->cfg, option);
	}
}

static void
finish_visual_update(enum cow_view_cfg_option option, struct cow_view_cfg *cfg)
{
	if (cfg != NULL)
		cow_view_cfg_set_override(cfg, option);
	else
		propagate_global_option(option);
	refresh_visuals();
}

static struct cow_titlebar_config *
target_titlebar(struct cmd_q *cmdq, struct cow_view_cfg **cfgp)
{
	struct cow_view_cfg *cfg = cfg_target(cmdq);

	*cfgp = cfg;
	return (cfg != NULL ? &cfg->titlebar : &cow_config->titlebar);
}

static struct cow_container_config *
target_container(struct cmd_q *cmdq, struct cow_view_cfg **cfgp)
{
	struct cow_view_cfg *cfg = cfg_target(cmdq);

	*cfgp = cfg;
	return (cfg != NULL ? &cfg->container : &cow_config->container);
}

struct cmd_entry cmd_set_entry = {
	"set",
	"v",
	2,
	-1,
	"set [-v] key value",
	cmd_set_exec,
	0,
};

static const struct cow_option_entry *
set_option_find(const char *key)
{
	for (const struct cow_option_entry *opt = cow_options_table; opt->name != NULL;
	    opt++) {
		if (opt->match == COW_OPTION_EXACT && strcmp(opt->name, key) == 0)
			return (opt);
	}
	for (const struct cow_option_entry *opt = cow_options_table; opt->name != NULL;
	    opt++) {
		size_t len;

		if (opt->match != COW_OPTION_PREFIX)
			continue;
		len = strlen(opt->name);
		if (strncmp(opt->name, key, len) == 0)
			return (opt);
	}
	return (NULL);
}

bool
cmd_set_is_decor_key(const char *key)
{
	const struct cow_option_entry *opt = set_option_find(key);

	return (opt != NULL && set_option_is_decor(opt, key));
}

char *
cmd_set_normalize_decor_value(const char *key, const char *value,
    struct cmd_q *cmdq)
{
	if (!set_key_is_image(key))
		return xstrdup(value);
	return set_image_resolve_path(value, cmdq);
}

void
cmd_set_reset_decor_target(struct cmd_q *cmdq)
{
	struct cow_view_cfg *cfg = cfg_target(cmdq);

	if (cfg != NULL) {
		cow_view_cfg_apply_global(cfg);
		refresh_visuals();
		return;
	}

	cow_config_reset_decor(cow_config);
	if (!cow_ipc.startup_mode) {
		struct cow_view *view;
		wl_list_for_each(view, &cow_wm.views, wm_link)
		{
			if (!view->closed)
				cow_view_cfg_apply_global(&view->cfg);
		}
		refresh_visuals();
	}
}

static enum cow_view_cfg_option
button_vector_option(int idx)
{
	return COW_VIEW_CFG_TITLEBAR_BUTTON_0_VECTOR + idx * 3;
}

static enum cow_view_cfg_option
button_action_option(int idx)
{
	return COW_VIEW_CFG_TITLEBAR_BUTTON_0_ACTION + idx * 3;
}

static enum cow_view_cfg_option
button_image_option(int idx)
{
	return COW_VIEW_CFG_TITLEBAR_BUTTON_0_IMAGE + idx * 3;
}

static bool
parse_button_key(const char *key, int *idx, const char **field)
{
	static const char prefix[] = "titlebar.button.";
	const char	 *p;
	const char	 *errstr;
	long long	  ll;
	char		  nbuf[3];
	size_t		  len;

	if (strncmp(key, prefix, strlen(prefix)) != 0)
		return false;
	p = key + strlen(prefix);
	len = strcspn(p, ".");
	if (len == 0 || len >= sizeof nbuf || p[len] != '.')
		return false;
	memcpy(nbuf, p, len);
	nbuf[len] = '\0';
	ll = strtonum(nbuf, 0, COW_TITLEBAR_NUM_BUTTONS - 1, &errstr);
	if (errstr != NULL)
		return false;
	*idx = (int)ll;
	*field = p + len + 1;
	return true;
}

static bool
set_option_is_decor(const struct cow_option_entry *opt, const char *key)
{
	if (opt->scope != COW_OPTION_MIXED)
		return (false);
	if (strcmp(key, "border.corner_length") == 0 ||
	    strcmp(key, "border.width") == 0 ||
	    strcmp(key, "icon.size") == 0 ||
	    strcmp(key, "icon.image") == 0 ||
	    strcmp(key, "icon.background") == 0 ||
	    strcmp(key, "icon.foreground") == 0 ||
	    strcmp(key, "icon.title_relief") == 0 ||
	    strcmp(key, "border.style") == 0 ||
	    strcmp(key, "titlebar.font") == 0 ||
	    strcmp(key, "titlebar.enabled") == 0 ||
	    strcmp(key, "border.handles") == 0 ||
	    strcmp(key, "titlebar.height") == 0 ||
	    strcmp(key, "titlebar.squeeze") == 0 ||
	    strcmp(key, "titlebar.squeeze.justify") == 0 ||
	    strcmp(key, "titlebar.squeeze.width") == 0 ||
	    strcmp(key, "titlebar.squeeze.offset") == 0 ||
	    strcmp(key, "titlebar.squeeze.min_width") == 0 ||
	    strcmp(key, "container.tab.sunken") == 0)
		return (true);
	if (strcmp(key, "titlebar.active_colour") == 0 ||
	    strcmp(key, "titlebar.inactive_colour") == 0 ||
	    strcmp(key, "titlebar.active_gradient") == 0 ||
	    strcmp(key, "titlebar.inactive_gradient") == 0 ||
	    strcmp(key, "titlebar.fg_active") == 0 ||
	    strcmp(key, "titlebar.fg_inactive") == 0 ||
	    strcmp(key, "titlebar.active_image") == 0 ||
	    strcmp(key, "titlebar.inactive_image") == 0)
		return (true);
	if (strcmp(key, "border.active") == 0 ||
	    strcmp(key, "border.inactive") == 0 ||
	    strcmp(key, "border.active.gradient") == 0 ||
	    strcmp(key, "border.inactive.gradient") == 0 ||
	    strcmp(key, "border.active.image") == 0 ||
	    strcmp(key, "border.inactive.image") == 0)
		return (true);
	if (strncmp(key, "border.active.", 14) == 0 ||
	    strncmp(key, "border.inactive.", 16) == 0) {
		const char *edge = key + (key[7] == 'a' ? 14 : 16);
		for (const struct set_border_edge *e = set_border_edges;
		    e->name != NULL; e++) {
			size_t len = strlen(e->name);
			if (strcmp(edge, e->name) == 0 ||
			    (strncmp(edge, e->name, len) == 0 &&
				strcmp(edge + len, ".gradient") == 0) ||
			    (strncmp(edge, e->name, len) == 0 &&
				strcmp(edge + len, ".image") == 0))
				return (true);
		}
		return (false);
	}
	if (strncmp(key, "titlebar.button.", 16) == 0) {
		int idx;
		const char *field;
		return parse_button_key(key, &idx, &field) &&
		    (strcmp(field, "vector") == 0 ||
			strcmp(field, "action") == 0 ||
			strcmp(field, "image") == 0);
	}
	if (strncmp(key, "container.tab.", 14) == 0) {
		if (strcmp(key, "container.tab.active_image") == 0 ||
		    strcmp(key, "container.tab.inactive_image") == 0 ||
		    strcmp(key, "container.tab.active_gradient") == 0 ||
		    strcmp(key, "container.tab.inactive_gradient") == 0)
			return (true);
		for (const struct set_container_colour_option *c =
		    set_container_colour_options; c->name != NULL; c++) {
			if (strcmp(key, c->name) == 0)
				return (true);
		}
		return (false);
	}
	return (false);
}

static bool
set_option_scope_ok(const struct cow_option_entry *opt, struct cmd_q *cmdq)
{
	if (opt->scope != COW_OPTION_WINDOW)
		return (true);
	return (cmdq != NULL && cmdq->target.window != NULL);
}

static void
set_option_post_update(const struct cow_option_entry *opt)
{
	if ((opt->post & COW_OPTION_POST_PAN_FRAMES) != 0 && !cow_ipc.startup_mode)
		pan_frames_update_all();
	if ((opt->post & COW_OPTION_POST_REFRESH_VISUALS) != 0)
		refresh_visuals();
}

static enum cmd_retval
set_scalar_bool(const struct cow_option_entry *opt, const char *val,
    struct cmd_q *cmdq)
{
	bool b;

	if (!cmd_parse_bool(val, &b))
		return (CMD_RETURN_NORMAL);

	struct cow_view_cfg *cfg = cfg_target(cmdq);
	if (cfg != NULL && opt->view_offset != COW_OPTION_NO_OFFSET) {
		bool *ptr = (bool *)((char *)cfg + opt->view_offset);
		*ptr = b;
		cow_view_cfg_set_override(cfg, opt->id);
	} else if (cow_config != NULL) {
		bool *ptr = (bool *)((char *)cow_config + opt->config_offset);
		*ptr = b;
		if (opt->scope == COW_OPTION_MIXED)
			propagate_global_option(opt->id);
	}
	set_option_post_update(opt);
	return (CMD_RETURN_NORMAL);
}

static enum cmd_retval
set_scalar_int(const struct cow_option_entry *opt, const char *key, const char *val,
    struct cmd_q *cmdq)
{
	const char *errstr;
	long long   v = strtonum(val, opt->lo, opt->hi, &errstr);

	if (errstr != NULL) {
		if (strncmp(key, "repeat_", 7) == 0)
			cmdq_error(cmdq, "set: %s: %s: %s", key, val, errstr);
		else
			cmdq_error(cmdq, "set: %s: bad value: %s", key, val);
		return (CMD_RETURN_ERROR);
	}

	struct cow_view_cfg *cfg = cfg_target(cmdq);
	if (cfg != NULL && opt->view_offset != COW_OPTION_NO_OFFSET) {
		int *ptr = (int *)((char *)cfg + opt->view_offset);
		*ptr = (int)v;
		if ((opt->post & COW_OPTION_POST_ZERO_DISABLE_HANDLES) != 0 && v == 0)
			cfg->handles = false;
		cow_view_cfg_set_override(cfg, opt->id);
	} else if (cow_config != NULL) {
		int *ptr = (int *)((char *)cow_config + opt->config_offset);
		*ptr = (int)v;
		if ((opt->post & COW_OPTION_POST_ZERO_DISABLE_HANDLES) != 0 && v == 0)
			cow_config->handles = false;
		if (opt->scope == COW_OPTION_MIXED)
			propagate_global_option(opt->id);
	}
	set_option_post_update(opt);
	return (CMD_RETURN_NORMAL);
}

static enum cmd_retval
set_option_exec(const struct cow_option_entry *opt, const char *key, const char *val,
    struct args *args, struct cmd_q *cmdq)
{
	if (set_key_is_image(key))
		return (set_image_option(key, val, cmdq));

	switch (opt->action) {
	case COW_OPTION_SCALAR_INT:
		return (set_scalar_int(opt, key, val, cmdq));
	case COW_OPTION_SCALAR_BOOL:
		return (set_scalar_bool(opt, val, cmdq));
	case COW_OPTION_COLOUR:
		set_colour_option(key, val, cmdq);
		return (CMD_RETURN_NORMAL);
	case COW_OPTION_IMAGE:
		return (set_image_option(key, val, cmdq));
	case COW_OPTION_BORDER_STYLE:
		set_border_style_option(val, cmdq);
		return (CMD_RETURN_NORMAL);
	case COW_OPTION_DESKTOP_CONFIG:
		set_desktop_config_option(val, cmdq);
		return (CMD_RETURN_NORMAL);
	case COW_OPTION_FOCUS:
		set_focus_option(val);
		return (CMD_RETURN_NORMAL);
	case COW_OPTION_GEOMETRY_WINDOW:
		set_geometry_window_option(val, cmdq);
		return (CMD_RETURN_NORMAL);
	case COW_OPTION_PLACEMENT:
		set_placement_option(val, cmdq);
		return (CMD_RETURN_NORMAL);
	case COW_OPTION_FONT:
		set_font_option(val, cmdq);
		return (CMD_RETURN_NORMAL);
	case COW_OPTION_BUTTON:
		set_button_option(key, val, cmdq);
		return (CMD_RETURN_NORMAL);
	case COW_OPTION_DESKS_COUNT:
		return (set_desks_count(val, cmdq));
	case COW_OPTION_DESKTOP_SIZE:
		return (set_desktop_size(args, cmdq));
	case COW_OPTION_TITLEBAR_HEIGHT:
		return (set_titlebar_height(val, cmdq));
	case COW_OPTION_TITLEBAR_SQUEEZE:
	case COW_OPTION_TITLEBAR_SQUEEZE_JUSTIFY:
	case COW_OPTION_TITLEBAR_SQUEEZE_INT:
		return (set_titlebar_squeeze(opt, key, val, cmdq));
	case COW_OPTION_LAYER:
		return (set_layer(key, val, cmdq));
	}
	return (CMD_RETURN_ERROR);
}

static enum cmd_retval
cmd_set_exec(struct cmd *self, struct cmd_q *cmdq)
{
	struct args *args = self->args;

	if (args->argc < 2) {
		cmdq_error(cmdq, "usage: %s", self->entry->usage);
		return (CMD_RETURN_ERROR);
	}

	const char *key = args->argv[0];
	char	   *val = cmd_argv_to_string(args->argc, args->argv, 1);
	const struct cow_option_entry *opt = set_option_find(key);

	if (opt != NULL && set_option_is_decor(opt, key)) {
		cmdq_error(cmdq, "set: %s is decoration-only; use decor", key);
		xfree(val);
		return (CMD_RETURN_ERROR);
	}

	if (opt != NULL && set_option_scope_ok(opt, cmdq)) {
		enum cmd_retval ret = set_option_exec(opt, key, val, args, cmdq);
		xfree(val);
		return (ret);
	}

	if (opt == NULL)
		cmdq_error(cmdq, "set: unknown key: %s", key);
	else
		cmdq_error(cmdq, "set: %s: wrong target scope", key);
	xfree(val);
	return (CMD_RETURN_ERROR);
}

enum cmd_retval
cmd_set_apply_decor(const char *key, const char *val, struct cmd_q *cmdq)
{
	const struct cow_option_entry *opt = set_option_find(key);

	if (opt == NULL || !set_option_is_decor(opt, key)) {
		cmdq_error(cmdq, "decor: unknown key: %s", key);
		return (CMD_RETURN_ERROR);
	}
	if (!set_option_scope_ok(opt, cmdq)) {
		cmdq_error(cmdq, "decor: %s: wrong target scope", key);
		return (CMD_RETURN_ERROR);
	}
	return (set_option_exec(opt, key, val, NULL, cmdq));
}

static enum cmd_retval
set_desks_count(const char *val, struct cmd_q *cmdq)
{
	char *end;
	long  n = strtol(val, &end, 10);
	if (val[0] == '\0' || *end != '\0' || n <= 0 ||
	    n > COW_MAX_DESK_COUNT) {
		cmdq_error(cmdq, "set: desks.count: expected 1..%d, got '%s'",
		    COW_MAX_DESK_COUNT, val);
		return (CMD_RETURN_ERROR);
	}

	if (cow_ipc.startup_mode) {
		cow_config->desk_count = (uint8_t)n;
		return (CMD_RETURN_NORMAL);
	}

	char *cause = NULL;
	if (!cow_desk_sets_resize_all((uint8_t)n, &cause)) {
		cmdq_error(cmdq, "%s", cause != NULL ? cause :
			    "set: desks.count: failed");
		xfree(cause);
		return (CMD_RETURN_ERROR);
	}
	cow_config->desk_count = (uint8_t)n;
	xfree(cause);
	cow_status_emit();
	cow_wm_request_manage_dirty();
	return (CMD_RETURN_NORMAL);
}

static enum cmd_retval
set_desktop_size(struct args *args, struct cmd_q *cmdq)
{
	if (args->argc < 3) {
		cmdq_error(cmdq, "set: desktop_size: expected N M (e.g. 3 2)");
		return (CMD_RETURN_ERROR);
	}

	const char *errstr;
	int	    cols = (int)strtonum(args->argv[1], 1, 99, &errstr);
	if (errstr != NULL) {
		cmdq_error(cmdq, "set: desktop_size: bad column count: %s",
		    args->argv[1]);
		return (CMD_RETURN_ERROR);
	}
	int rows = (int)strtonum(args->argv[2], 1, 99, &errstr);
	if (errstr != NULL) {
		cmdq_error(cmdq, "set: desktop_size: bad row count: %s",
		    args->argv[2]);
		return (CMD_RETURN_ERROR);
	}

	if (cow_config != NULL) {
		cow_config->page_cols = cols;
		cow_config->page_rows = rows;
		if (!cow_ipc.startup_mode) {
			struct cow_output *out;
			wl_list_for_each(out, &cow_wm.outputs, wm_link)
			    cow_page_clamp_views(out);
			cow_wm_request_manage_dirty();
			pan_frames_update_all();
		}
	}
	return (CMD_RETURN_NORMAL);
}

static enum cmd_retval
set_titlebar_height(const char *val, struct cmd_q *cmdq)
{
	const char *errstr;
	long long   v = strtonum(val, 0, INT_MAX, &errstr);
	if (errstr != NULL) {
		cmdq_error(cmdq, "set: titlebar.height: bad value: %s", val);
		return (CMD_RETURN_ERROR);
	}

	struct cow_view_cfg	    *cfg;
	struct cow_titlebar_config *tb = target_titlebar(cmdq, &cfg);
	tb->height = (int)v;
	finish_visual_update(COW_VIEW_CFG_TITLEBAR_HEIGHT, cfg);
	return (CMD_RETURN_NORMAL);
}

static enum cmd_retval
set_titlebar_squeeze(const struct cow_option_entry *opt, const char *key,
    const char *val, struct cmd_q *cmdq)
{
	const char *errstr;
	struct cow_view_cfg	    *cfg;
	struct cow_titlebar_config *tb = target_titlebar(cmdq, &cfg);

	if (opt->action == COW_OPTION_TITLEBAR_SQUEEZE) {
		enum cow_squeeze_mode m;
		if (!cow_parse_squeeze_mode(val, &m)) {
			cmdq_error(cmdq,
			    "set: titlebar.squeeze: bad value: %s "
			    "(expected none|natural|fixed)",
			    val);
			return (CMD_RETURN_ERROR);
		}
		tb->squeeze.mode = m;
	} else if (opt->action == COW_OPTION_TITLEBAR_SQUEEZE_JUSTIFY) {
		enum cow_squeeze_just j;
		if (!cow_parse_squeeze_just(val, &j)) {
			cmdq_error(cmdq,
			    "set: titlebar.squeeze.justify: bad value: %s "
			    "(expected left|centre|right)",
			    val);
			return (CMD_RETURN_ERROR);
		}
		tb->squeeze.justify = j;
	} else {
		long long v = strtonum(val, opt->lo, opt->hi, &errstr);
		if (errstr != NULL) {
			cmdq_error(cmdq, "set: %s: bad value: %s", key, val);
			return (CMD_RETURN_ERROR);
		}
		int *ptr = (int *)((char *)&tb->squeeze + opt->config_offset);
		*ptr = (int)v;
	}

	finish_visual_update(opt->id, cfg);
	return (CMD_RETURN_NORMAL);
}

static enum cmd_retval
set_layer(const char *key, const char *val, struct cmd_q *cmdq)
{
	const char *errstr;
	long long   v = strtonum(val, 0, 255, &errstr);
	if (errstr != NULL) {
		cmdq_error(cmdq, "set: %s: bad value: %s", key, val);
		return (CMD_RETURN_ERROR);
	}
	if (cow_config == NULL)
		return (CMD_RETURN_NORMAL);

	if (strcmp(key, "default_layer") == 0) {
		int max = (int)cow_config->max_layer;
		if ((int)v > max)
			v = max;
		cow_config->default_layer = (uint8_t)v;
		return (CMD_RETURN_NORMAL);
	}

	cow_config->max_layer = (uint8_t)v;
	if (cow_config->default_layer > (uint8_t)v)
		cow_config->default_layer = (uint8_t)v;

	struct cow_view *vw;
	bool		 any = false;
	wl_list_for_each(vw, &cow_wm.views, wm_link)
	{
		if (vw->layer > (uint8_t)v) {
			vw->layer = (uint8_t)v;
			any	  = true;
		}
	}
	if (any) {
		cow_stack_mark_dirty();
		cow_wm_request_manage_dirty();
	}
	return (CMD_RETURN_NORMAL);
}

static bool
has_suffix(const char *s, const char *suffix)
{
	size_t slen = strlen(s), suffix_len = strlen(suffix);

	return (slen >= suffix_len &&
	    strcmp(s + slen - suffix_len, suffix) == 0);
}

static bool
set_key_is_image(const char *key)
{
	return (has_suffix(key, "_image") || has_suffix(key, ".image"));
}

static char *
set_image_resolve_path(const char *val, struct cmd_q *cmdq)
{
	char *path = NULL;

	if (strcmp(val, "none") == 0)
		return xstrdup(val);
	if (val[0] == '/') {
		path = xstrdup(val);
	} else if (val[0] == '~' && val[1] == '/') {
		const char *home = getenv("HOME");
		if (home == NULL) {
			cmdq_error(cmdq, "set: image: HOME is not set");
			return NULL;
		}
		xasprintf(&path, "%s/%s", home, val + 2);
	} else if (cmdq != NULL && cmdq->cmd != NULL &&
	    cmdq->cmd->file != NULL) {
		const char *file = cmdq->cmd->file;
		const char *slash = strrchr(file, '/');
		if (slash != NULL) {
			size_t len = slash - file;
			path = xmalloc(len + strlen(val) + 2);
			memcpy(path, file, len);
			path[len] = '/';
			strcpy(path + len + 1, val);
		} else
			path = xstrdup(val);
	} else
		path = xstrdup(val);

	char *cause = NULL;
	if (!cow_image_validate(path, &cause)) {
		cmdq_error(cmdq, "set: image: %s", cause);
		xfree(cause);
		xfree(path);
		return NULL;
	}
	return path;
}

static enum cmd_retval
set_image_value(char **slot, enum cow_view_cfg_option id, const char *val,
    struct cow_view_cfg *cfg, struct cmd_q *cmdq)
{
	char *path = set_image_resolve_path(val, cmdq);

	if (path == NULL)
		return (CMD_RETURN_ERROR);
	xfree(*slot);
	if (strcmp(path, "none") == 0) {
		xfree(path);
		*slot = NULL;
	} else
		*slot = path;
	finish_visual_update(id, cfg);
	return (CMD_RETURN_NORMAL);
}

static enum cmd_retval
set_icon_image(const char *key, const char *val, struct cmd_q *cmdq)
{
	struct cow_view_cfg *cfg = cfg_target(cmdq);
	char		  **slot = cfg != NULL ? &cfg->icon_image :
				       &cow_config->icon_image;

	if (strcmp(key, "icon.image") != 0)
		return (CMD_RETURN_ERROR);
	return (set_image_value(slot, COW_VIEW_CFG_ICON_IMAGE, val, cfg, cmdq));
}

static enum cmd_retval
set_titlebar_image(const char *key, const char *val, struct cmd_q *cmdq)
{
	struct cow_view_cfg	    *cfg;
	struct cow_titlebar_config *tb = target_titlebar(cmdq, &cfg);

	if (strcmp(key, "titlebar.active_image") == 0)
		return (set_image_value(&tb->active_image,
		    COW_VIEW_CFG_TITLEBAR_ACTIVE_IMAGE, val, cfg, cmdq));
	if (strcmp(key, "titlebar.inactive_image") == 0)
		return (set_image_value(&tb->inactive_image,
		    COW_VIEW_CFG_TITLEBAR_INACTIVE_IMAGE, val, cfg, cmdq));
	return (CMD_RETURN_ERROR);
}

static enum cmd_retval
set_container_tab_image(const char *key, const char *val, struct cmd_q *cmdq)
{
	struct cow_view_cfg	     *cfg;
	struct cow_container_config *cc = target_container(cmdq, &cfg);

	if (strcmp(key, "container.tab.active_image") == 0)
		return (set_image_value(&cc->tab_active_image,
		    COW_VIEW_CFG_CONTAINER_TAB_ACTIVE_IMAGE, val, cfg, cmdq));
	if (strcmp(key, "container.tab.inactive_image") == 0)
		return (set_image_value(&cc->tab_inactive_image,
		    COW_VIEW_CFG_CONTAINER_TAB_INACTIVE_IMAGE, val, cfg, cmdq));
	return (CMD_RETURN_ERROR);
}

static enum cmd_retval
set_button_image(const char *key, const char *val, struct cmd_q *cmdq)
{
	int			  idx;
	const char		 *field;
	struct cow_view_cfg	 *cfg;
	struct cow_titlebar_config *tb = target_titlebar(cmdq, &cfg);

	if (!parse_button_key(key, &idx, &field) || strcmp(field, "image") != 0)
		return (CMD_RETURN_ERROR);
	return (set_image_value(&tb->buttons[idx].image,
	    button_image_option(idx), val, cfg, cmdq));
}

static enum cmd_retval
set_border_image(const char *key, const char *val, struct cmd_q *cmdq)
{
	bool is_active = (strcmp(key, "border.active.image") == 0 ||
	    strncmp(key, "border.active.", 14) == 0);
	bool is_inactive = (strcmp(key, "border.inactive.image") == 0 ||
	    strncmp(key, "border.inactive.", 16) == 0);

	if (!is_active && !is_inactive)
		return (CMD_RETURN_ERROR);

	struct cow_view_cfg *cfg = cfg_target(cmdq);
	struct cow_border_colour *bc = cfg != NULL ?
	    (is_active ? &cfg->border_active : &cfg->border_inactive) :
	    (is_active ? &cow_config->border_active :
			 &cow_config->border_inactive);

	if (strcmp(key, "border.active.image") == 0 ||
	    strcmp(key, "border.inactive.image") == 0)
		return (set_image_value(&bc->image, is_active ?
		    COW_VIEW_CFG_BORDER_ACTIVE_IMAGE :
		    COW_VIEW_CFG_BORDER_INACTIVE_IMAGE, val, cfg, cmdq));

	const char *edge = key + (is_active ? 14 : 16);
	for (const struct set_border_edge *opt = set_border_edges;
	    opt->name != NULL; opt++) {
		size_t len = strlen(opt->name);
		if (strncmp(edge, opt->name, len) != 0 ||
		    strcmp(edge + len, ".image") != 0)
			continue;
		char **slot = (char **)((char *)bc + opt->image_offset);
		return (set_image_value(slot, is_active ?
		    opt->active_image_id : opt->inactive_image_id, val, cfg,
		    cmdq));
	}
	return (CMD_RETURN_ERROR);
}

static enum cmd_retval
set_image_option(const char *key, const char *val, struct cmd_q *cmdq)
{
	if (strncmp(key, "titlebar.button.", 16) == 0)
		return (set_button_image(key, val, cmdq));
	if (strcmp(key, "icon.image") == 0)
		return (set_icon_image(key, val, cmdq));
	if (strncmp(key, "border.", 7) == 0)
		return (set_border_image(key, val, cmdq));
	if (strncmp(key, "container.tab.", 14) == 0)
		return (set_container_tab_image(key, val, cmdq));
	if (strncmp(key, "titlebar.", 9) == 0)
		return (set_titlebar_image(key, val, cmdq));
	cmdq_error(cmdq, "set: unknown key: %s", key);
	return (CMD_RETURN_ERROR);
}

static bool
set_gradient_value(struct cow_gradient *g, const char *key, const char *val,
    struct cmd_q *cmdq)
{
	if (!cow_gradient_parse(g, val)) {
		cmdq_error(cmdq,
		    "set: %s: expected none|HGradient colour colour|"
		    "VGradient colour colour",
		    key);
		return (false);
	}
	return (true);
}

static bool
set_titlebar_gradient(const char *key, const char *val, struct cmd_q *cmdq)
{
	struct cow_view_cfg	    *cfg;
	struct cow_titlebar_config *tb = target_titlebar(cmdq, &cfg);
	struct cow_gradient	    *g;
	enum cow_view_cfg_option   id;

	if (strcmp(key, "titlebar.active_gradient") == 0) {
		g = &tb->active_gradient;
		id = COW_VIEW_CFG_TITLEBAR_ACTIVE_GRADIENT;
	} else if (strcmp(key, "titlebar.inactive_gradient") == 0) {
		g = &tb->inactive_gradient;
		id = COW_VIEW_CFG_TITLEBAR_INACTIVE_GRADIENT;
	} else
		return (false);

	if (set_gradient_value(g, key, val, cmdq))
		finish_visual_update(id, cfg);
	return (true);
}

static bool
set_container_tab_gradient(const char *key, const char *val, struct cmd_q *cmdq)
{
	struct cow_view_cfg	     *cfg;
	struct cow_container_config *cc = target_container(cmdq, &cfg);
	struct cow_gradient	     *g;
	enum cow_view_cfg_option    id;

	if (strcmp(key, "container.tab.active_gradient") == 0) {
		g = &cc->tab_active_gradient;
		id = COW_VIEW_CFG_CONTAINER_TAB_ACTIVE_GRADIENT;
	} else if (strcmp(key, "container.tab.inactive_gradient") == 0) {
		g = &cc->tab_inactive_gradient;
		id = COW_VIEW_CFG_CONTAINER_TAB_INACTIVE_GRADIENT;
	} else
		return (false);

	if (set_gradient_value(g, key, val, cmdq))
		finish_visual_update(id, cfg);
	return (true);
}

static bool
set_border_gradient(const char *key, const char *val, struct cmd_q *cmdq)
{
	bool is_active = (strcmp(key, "border.active.gradient") == 0 ||
	    strncmp(key, "border.active.", 14) == 0);
	bool is_inactive = (strcmp(key, "border.inactive.gradient") == 0 ||
	    strncmp(key, "border.inactive.", 16) == 0);

	if (!is_active && !is_inactive)
		return (false);

	struct cow_view_cfg *cfg = cfg_target(cmdq);
	struct cow_border_colour *bc = cfg != NULL ?
	    (is_active ? &cfg->border_active : &cfg->border_inactive) :
	    (is_active ? &cow_config->border_active :
			 &cow_config->border_inactive);
	struct cow_gradient *g = NULL;
	enum cow_view_cfg_option id = is_active ?
	    COW_VIEW_CFG_BORDER_ACTIVE_GRADIENT :
	    COW_VIEW_CFG_BORDER_INACTIVE_GRADIENT;

	if (strcmp(key, "border.active.gradient") == 0 ||
	    strcmp(key, "border.inactive.gradient") == 0) {
		g = cow_border_gradient_for_option(bc, id);
	} else {
		const char *edge = key + (is_active ? 14 : 16);
		for (const struct set_border_edge *opt = set_border_edges;
		    opt->name != NULL; opt++) {
			size_t len = strlen(opt->name);
			if (strncmp(edge, opt->name, len) == 0 &&
			    strcmp(edge + len, ".gradient") == 0) {
				id = is_active ? opt->active_gradient_id :
				    opt->inactive_gradient_id;
				g = cow_border_gradient_for_option(bc, id);
				break;
			}
		}
	}
	if (g == NULL) {
		cmdq_error(cmdq, "set: unknown key: %s", key);
		return (true);
	}
	if (set_gradient_value(g, key, val, cmdq))
		finish_visual_update(id, cfg);
	return (true);
}

static bool
set_gradient_option(const char *key, const char *val, struct cmd_q *cmdq)
{
	return (set_titlebar_gradient(key, val, cmdq) ||
	    set_container_tab_gradient(key, val, cmdq) ||
	    set_border_gradient(key, val, cmdq));
}

static void
set_colour_option(const char *key, const char *val, struct cmd_q *cmdq)
{
	if (has_suffix(key, "_gradient") || has_suffix(key, ".gradient")) {
		(void)set_gradient_option(key, val, cmdq);
		return;
	}
	if (set_output_colour(key, val, cmdq))
		return;
	if (set_icon_colour(key, val, cmdq))
		return;
	if (set_titlebar_colour(key, val, cmdq))
		return;
	if (set_container_tab_colour(key, val, cmdq))
		return;
	(void)set_border_colour(key, val, cmdq);
}

static bool
set_output_colour(const char *key, const char *val, struct cmd_q *cmdq)
{
	if (strncmp(key, "output.colour.background.", 25) == 0) {
		const char *oname = key + 25;
		float	    c[4];
		if (!cow_colour_parse(val, c)) {
			cmdq_error(cmdq, "set: invalid colour value");
			return (true);
		}
		struct cow_output *out;
		wl_list_for_each(out, &cow_wm.outputs, wm_link)
		{
			if (out->name != NULL &&
			    strcmp(out->name, oname) == 0) {
				memcpy(out->bg_colour, c, sizeof(float) * 4);
				out->has_bg_colour = true;
				out->bg_dirty	   = true;
				refresh_visuals();
				return (true);
			}
		}
		cmdq_error(cmdq, "set: unknown output: %s", oname);
		return (true);
	}

	if (strcmp(key, "output.colour.background") != 0)
		return (false);

	if (!cow_colour_parse(val, cow_config->output_bg_colour)) {
		cmdq_error(cmdq, "set: invalid colour value");
		return (true);
	}
	cow_config->has_output_bg_colour = true;
	refresh_visuals();
	return (true);
}

static bool
set_icon_colour(const char *key, const char *val, struct cmd_q *cmdq)
{
	struct cow_view_cfg *cfg = cfg_target(cmdq);
	float *slot;
	enum cow_view_cfg_option id;

	if (strcmp(key, "icon.background") == 0) {
		slot = cfg != NULL ? cfg->icon_background :
				     cow_config->icon_background;
		id = COW_VIEW_CFG_ICON_BACKGROUND;
	} else if (strcmp(key, "icon.foreground") == 0) {
		slot = cfg != NULL ? cfg->icon_foreground :
				     cow_config->icon_foreground;
		id = COW_VIEW_CFG_ICON_FOREGROUND;
	} else
		return (false);

	if (!cow_colour_parse(val, slot)) {
		cmdq_error(cmdq, "set: invalid colour value");
		return (true);
	}
	finish_visual_update(id, cfg);
	return (true);
}

static bool
set_titlebar_colour(const char *key, const char *val, struct cmd_q *cmdq)
{
	for (const struct set_titlebar_colour_option *opt =
	    set_titlebar_colour_options; opt->name != NULL; opt++) {
		if (strcmp(key, opt->name) != 0)
			continue;
		struct cow_view_cfg	    *cfg;
		struct cow_titlebar_config *tb = target_titlebar(cmdq, &cfg);
		float *ptr = (float *)((char *)tb + opt->offset);
		if (!cow_colour_parse(val, ptr)) {
			cmdq_error(cmdq, "set: invalid colour value");
			return (true);
		}
		finish_visual_update(opt->id, cfg);
		return (true);
	}
	return (false);
}

static bool
set_container_tab_colour(const char *key, const char *val, struct cmd_q *cmdq)
{
	for (const struct set_container_colour_option *opt =
	    set_container_colour_options; opt->name != NULL; opt++) {
		if (strcmp(key, opt->name) != 0)
			continue;
		struct cow_view_cfg	     *cfg;
		struct cow_container_config *cc = target_container(cmdq, &cfg);
		float *ptr = (float *)((char *)cc + opt->colour_offset);
		bool  *has = (bool *)((char *)cc + opt->has_offset);
		if (!cow_colour_parse(val, ptr)) {
			cmdq_error(cmdq, "set: invalid colour value");
			return (true);
		}
		*has = true;
		finish_visual_update(opt->id, cfg);
		return (true);
	}
	return (false);
}

static bool
set_border_colour(const char *key, const char *val, struct cmd_q *cmdq)
{
	bool is_active = (strcmp(key, "border.active") == 0 ||
	    strncmp(key, "border.active.", 14) == 0);
	bool is_inactive = (strcmp(key, "border.inactive") == 0 ||
	    strncmp(key, "border.inactive.", 16) == 0);

	if (!is_active && !is_inactive)
		return (false);

	struct cow_view_cfg *cfg = cfg_target(cmdq);
	struct cow_border_colour *bc;
	if (cfg != NULL) {
		bc = is_active ? &cfg->border_active : &cfg->border_inactive;
	} else {
		bc = is_active ? &cow_config->border_active :
				 &cow_config->border_inactive;
	}

	float c[4];
	if (strcmp(key, "border.active") == 0 ||
	    strcmp(key, "border.inactive") == 0) {
		if (!cow_colour_parse(val, c)) {
			cmdq_error(cmdq, "set: invalid colour value");
			return (true);
		}
		memcpy(bc->n, c, sizeof(float) * 4);
		memcpy(bc->s, c, sizeof(float) * 4);
		memcpy(bc->e, c, sizeof(float) * 4);
		memcpy(bc->w, c, sizeof(float) * 4);
		finish_visual_update(is_active ? COW_VIEW_CFG_BORDER_ACTIVE :
		    COW_VIEW_CFG_BORDER_INACTIVE, cfg);
		return (true);
	}

	const char *edge = key + (is_active ? 14 : 16);
	const struct set_border_edge *edge_opt = NULL;
	for (const struct set_border_edge *opt = set_border_edges;
	    opt->name != NULL; opt++) {
		if (strcmp(edge, opt->name) == 0) {
			edge_opt = opt;
			break;
		}
	}
	if (edge_opt == NULL)
		return (true);

	float *ptr = (float *)((char *)bc + edge_opt->colour_offset);
	if (!cow_colour_parse(val, ptr)) {
		cmdq_error(cmdq, "set: invalid colour value");
		return (true);
	}
	if (edge_opt->has_offset != COW_OPTION_NO_OFFSET) {
		bool *has = (bool *)((char *)bc + edge_opt->has_offset);
		*has = true;
	}
	if (bc->has_nw || bc->has_ne || bc->has_sw || bc->has_se)
		bc->has_corners = true;
	finish_visual_update(is_active ? edge_opt->active_id :
	    edge_opt->inactive_id, cfg);
	return (true);
}

static void
set_border_style_option(const char *val, struct cmd_q *cmdq)
{
	enum cow_border_style style = COW_BORDER_STYLE_NONE;
	for (const struct set_enum_option *opt = set_border_style_options;
	    opt->name != NULL; opt++) {
		if (strcmp(val, opt->name) == 0) {
			style = opt->value;
			break;
		}
	}

	struct cow_view_cfg *cfg = cfg_target(cmdq);
	if (cfg != NULL)
		cfg->border_style = style;
	else
		cow_config->border_style = style;
	finish_visual_update(COW_VIEW_CFG_BORDER_STYLE, cfg);
}

static void
set_desktop_config_option(const char *val, struct cmd_q *cmdq)
{
	if (strcmp(val, "global") == 0) {
		set_desktop_config_global();
	} else if (strcmp(val, "per-output") == 0) {
		cow_config->desktop_config = COW_DESKTOP_PER_OUTPUT;
	} else if (strcmp(val, "shared") == 0) {
		set_desktop_config_shared();
	} else {
		cmdq_error(cmdq,
		    "set: desktop-configuration: expected "
		    "global|per-output|shared");
		return;
	}
	cow_wm_request_manage_dirty();
	cow_status_emit();
}

static void
set_desktop_config_global(void)
{
	cow_config->desktop_config = COW_DESKTOP_GLOBAL;

	if (cow_ipc.startup_mode)
		return;

	struct cow_output *cur = cow_wm.current_output;
	if (cur == NULL)
		return;

	uint8_t		   desk = cur->current_desk->nr;
	struct cow_output *out;
	wl_list_for_each(out, &cow_wm.outputs, wm_link)
	{
		if (out != cur)
			cow_desk_switch(out, desk);
	}
}

static void
set_desktop_config_shared(void)
{
	cow_config->desktop_config = COW_DESKTOP_PER_OUTPUT;

	struct cow_output *out;
	wl_list_for_each(out, &cow_wm.outputs, wm_link)
	{
		struct cow_output *other;
		wl_list_for_each(other, &cow_wm.outputs, wm_link)
		{
			if (other == out)
				continue;
			if (other->current_desk->nr != out->current_desk->nr)
				continue;

			struct cow_desk *cand;
			wl_list_for_each(cand, &other->desk_set.desks, set_link)
			{
				if (desk_nr_used_except(other, cand->nr))
					continue;
				cow_desk_switch(other, cand->nr);
				break;
			}
		}
	}
	cow_config->desktop_config = COW_DESKTOP_SHARED;
}

static bool
desk_nr_used_except(struct cow_output *skip, uint8_t desk)
{
	struct cow_output *out;
	wl_list_for_each(out, &cow_wm.outputs, wm_link)
	{
		if (out != skip && out->current_desk->nr == desk)
			return (true);
	}
	return (false);
}

static void
set_focus_option(const char *val)
{
	cow_config->focus_mode = COW_FOCUS_MODE_CLICK;
	for (const struct set_enum_option *opt = set_focus_options;
	    opt->name != NULL; opt++) {
		if (strcmp(val, opt->name) == 0) {
			cow_config->focus_mode = opt->value;
			return;
		}
	}
}

static void
set_geometry_window_option(const char *val, struct cmd_q *cmdq)
{
	for (const struct set_enum_option *opt = set_geometry_window_options;
	    opt->name != NULL; opt++) {
		if (strcmp(val, opt->name) == 0) {
			cow_config->geometry_window = opt->value;
			return;
		}
	}
	cmdq_error(cmdq, "set: geometry_window: unknown mode: %s", val);
}

static void
set_placement_option(const char *val, struct cmd_q *cmdq)
{
	for (const struct set_enum_option *opt = set_placement_options;
	    opt->name != NULL; opt++) {
		if (strcmp(val, opt->name) == 0) {
			cow_config->placement = opt->value;
			return;
		}
	}
	cmdq_error(cmdq, "set: placement: unknown mode: %s", val);
}

static void
set_font_option(const char *val, struct cmd_q *cmdq)
{
	struct cow_view_cfg *cfg = cfg_target(cmdq);
	if (cfg != NULL) {
		xfree(cfg->font_name);
		cfg->font_name = xstrdup(val);
	} else {
		xfree(cow_config->font_name);
		cow_config->font_name = xstrdup(val);
	}
	finish_visual_update(COW_VIEW_CFG_FONT, cfg);
}

static void
set_button_option(const char *key, const char *val, struct cmd_q *cmdq)
{
	int			  idx;
	const char		 *field;
	struct cow_view_cfg	 *cfg;
	struct cow_titlebar_config *tb = target_titlebar(cmdq, &cfg);

	if (!parse_button_key(key, &idx, &field)) {
		cmdq_error(cmdq, "set: unknown key: %s", key);
		return;
	}

	if (strcmp(field, "vector") == 0) {
		if (!cow_parse_vector_button_string(&tb->buttons[idx].vector,
		    val)) {
			cmdq_error(cmdq, "set: %s: bad vector", key);
			return;
		}
		finish_visual_update(button_vector_option(idx), cfg);
		return;
	}
	if (strcmp(field, "action") == 0) {
		xfree(tb->buttons[idx].action);
		tb->buttons[idx].action = xstrdup(val);
		finish_visual_update(button_action_option(idx), cfg);
		return;
	}
	cmdq_error(cmdq, "set: unknown key: %s", key);
}

static void
refresh_visuals(void)
{
	if (cow_ipc.startup_mode)
		return;

	struct cow_view *view;
	wl_list_for_each(view, &cow_wm.views, wm_link)
		refresh_view_visuals(view);

	struct cow_output *output;
	wl_list_for_each(output, &cow_wm.outputs, wm_link)
		refresh_output_visuals(output);

	cow_wm_manage_dirty_now();
}

static int
refreshed_title_thickness(struct cow_view *view)
{
	if (!view->cfg.titlebar.enabled)
		return (0);

	if (view->cfg.titlebar.height > 0)
		return (view->cfg.titlebar.height);

	struct cow_font tb_font;
	const char     *fname = view->cfg.font_name != NULL ?
	      view->cfg.font_name :
	      "monospace 10";
	cow_font_init(&tb_font, fname);
	return (cow_titlebar_compute_thickness(&tb_font));
}

static void
refresh_view_visuals(struct cow_view *view)
{
	if (view->closed)
		return;

	view->dec.dirty		      = true;
	view->dec.border_width	      = view->cfg.border;
	view->dec.title_thickness     = refreshed_title_thickness(view);
	view->dec.last_content_width  = -1;
	view->dec.last_content_height = -1;
	if (view->dec.buffer != NULL) {
		cow_shm_buffer_destroy(view->dec.buffer);
		view->dec.buffer = NULL;
	}
}

static void
refresh_output_visuals(struct cow_output *output)
{
	output->usable_area.x	    = output->x;
	output->usable_area.y	    = output->y;
	output->usable_area.width  = output->width;
	output->usable_area.height = output->height;
	cow_output_apply_reserved_area(output);
	output->bg_dirty = true;
}
