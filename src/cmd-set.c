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
#include "cow/ipc.h"
#include "cow/memory.h"
#include "cow/output.h"
#include "cow/pan_frames.h"
#include "cow/shm.h"
#include "cow/stack.h"
#include "cow/status.h"
#include "cow/titlebar.h"
#include "cow/view.h"
#include "cow/wm.h"

static enum cmd_retval cmd_set_exec(struct cmd *, struct cmd_q *);
struct set_option;
static enum cmd_retval set_option_exec(const struct set_option *, const char *,
    const char *, struct args *, struct cmd_q *);
static const struct set_option *set_option_find(const char *);
static bool	       set_option_scope_ok(const struct set_option *,
			   struct cmd_q *);
static enum cmd_retval set_scalar_bool(const struct set_option *,
    const char *, struct cmd_q *);
static enum cmd_retval set_scalar_int(const struct set_option *, const char *,
    const char *, struct cmd_q *);
static enum cmd_retval set_desks_count(const char *, struct cmd_q *);
static enum cmd_retval set_desktop_size(struct args *, struct cmd_q *);
static enum cmd_retval set_titlebar_height(const char *, struct cmd_q *);
static enum cmd_retval set_titlebar_squeeze(const struct set_option *,
    const char *, const char *, struct cmd_q *);
static enum cmd_retval set_layer(const char *, const char *, struct cmd_q *);
static void finish_visual_update(enum cow_view_cfg_option,
    struct cow_view_cfg *);
static struct cow_titlebar_config *target_titlebar(struct cmd_q *,
    struct cow_view_cfg **);
static struct cow_container_config *target_container(struct cmd_q *,
    struct cow_view_cfg **);
static void set_colour_option(const char *, const char *, struct cmd_q *);
static bool set_output_colour(const char *, const char *, struct cmd_q *);
static bool set_border_colour(const char *, const char *, struct cmd_q *);
static bool set_titlebar_colour(const char *, const char *, struct cmd_q *);
static bool set_container_tab_colour(const char *, const char *, struct cmd_q *);
static void set_border_style_option(const char *, struct cmd_q *);
static void set_desktop_config_option(const char *, struct cmd_q *);
static void set_desktop_config_global(void);
static void set_desktop_config_shared(void);
static bool desk_nr_used_except(struct cow_output *, uint8_t);
static void set_focus_option(const char *);
static void set_placement_option(const char *, struct cmd_q *);
static void set_font_option(const char *, struct cmd_q *);
static void set_button_option(const char *, const char *, struct cmd_q *);
static enum cow_button_action set_button_action(const char *);
static void refresh_visuals(void);
static int  refreshed_title_thickness(struct cow_view *);
static void refresh_view_visuals(struct cow_view *);
static void refresh_output_visuals(struct cow_output *);

enum set_option_scope {
	SET_SCOPE_GLOBAL,
	SET_SCOPE_WINDOW,
	SET_SCOPE_OUTPUT,
	SET_SCOPE_MIXED,
};

enum set_option_match {
	SET_MATCH_EXACT,
	SET_MATCH_PREFIX,
};

enum set_option_action {
	SET_ACTION_SCALAR_BOOL,
	SET_ACTION_SCALAR_INT,
	SET_ACTION_COLOUR,
	SET_ACTION_BORDER_STYLE,
	SET_ACTION_DESKTOP_CONFIG,
	SET_ACTION_FOCUS,
	SET_ACTION_PLACEMENT,
	SET_ACTION_FONT,
	SET_ACTION_BUTTON,
	SET_ACTION_DESKS_COUNT,
	SET_ACTION_DESKTOP_SIZE,
	SET_ACTION_TITLEBAR_HEIGHT,
	SET_ACTION_TITLEBAR_SQUEEZE,
	SET_ACTION_TITLEBAR_SQUEEZE_JUSTIFY,
	SET_ACTION_TITLEBAR_SQUEEZE_INT,
	SET_ACTION_LAYER,
};

enum set_option_post {
	SET_POST_NONE		     = 0,
	SET_POST_REFRESH_VISUALS     = 1 << 0,
	SET_POST_PAN_FRAMES	     = 1 << 1,
	SET_POST_ZERO_DISABLE_HANDLES = 1 << 2,
};

#define SET_NO_OFFSET ((size_t)-1)

struct set_option {
	const char	   *name;
	enum set_option_match match;
	enum set_option_scope scope;
	enum set_option_action action;
	enum cow_view_cfg_option id;
	size_t		    config_offset;
	size_t		    view_offset;
	long long	    lo;
	long long	    hi;
	unsigned int	    post;
};

enum set_button_part {
	SET_BUTTON_VECTOR,
	SET_BUTTON_ACTION,
};

struct set_button_option {
	const char	     *key;
	enum cow_view_cfg_option id;
	enum set_button_part part;
	int		      idx;
};

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
	size_t	    colour_offset;
	size_t	    has_offset;
};

struct set_enum_option {
	const char *name;
	int	    value;
};

static const struct set_button_option set_button_options[] = {
	{
		.key = "titlebar.button_left.vector",
		.id = COW_VIEW_CFG_TITLEBAR_BUTTON_LEFT_VECTOR,
		.part = SET_BUTTON_VECTOR,
		.idx = 0,
	},
	{
		.key = "titlebar.button_left.action",
		.id = COW_VIEW_CFG_TITLEBAR_BUTTON_LEFT_ACTION,
		.part = SET_BUTTON_ACTION,
		.idx = 0,
	},
	{
		.key = "titlebar.button_right.vector",
		.id = COW_VIEW_CFG_TITLEBAR_BUTTON_RIGHT_VECTOR,
		.part = SET_BUTTON_VECTOR,
		.idx = 1,
	},
	{
		.key = "titlebar.button_right.action",
		.id = COW_VIEW_CFG_TITLEBAR_BUTTON_RIGHT_ACTION,
		.part = SET_BUTTON_ACTION,
		.idx = 1,
	},
	{
		.key = "titlebar.button_right2.vector",
		.id = COW_VIEW_CFG_TITLEBAR_BUTTON_RIGHT2_VECTOR,
		.part = SET_BUTTON_VECTOR,
		.idx = 2,
	},
	{
		.key = "titlebar.button_right2.action",
		.id = COW_VIEW_CFG_TITLEBAR_BUTTON_RIGHT2_ACTION,
		.part = SET_BUTTON_ACTION,
		.idx = 2,
	},
	{ .key = NULL },
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

static const struct set_enum_option set_placement_options[] = {
	{ "cascade", COW_PLACEMENT_CASCADE },
	{ "undermouse", COW_PLACEMENT_UNDERMOUSE },
	{ "centre", COW_PLACEMENT_CENTRE },
	{ "center", COW_PLACEMENT_CENTRE },
	{ NULL, -1 },
};

static const struct set_enum_option set_button_action_options[] = {
	{ "close", COW_BUTTON_ACTION_CLOSE },
	{ "maximize", COW_BUTTON_ACTION_MAXIMIZE },
	{ "iconify", COW_BUTTON_ACTION_ICONIFY },
	{ NULL, COW_BUTTON_ACTION_NONE },
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
	    .colour_offset = offsetof(struct cow_border_colour, n),
	    .has_offset = SET_NO_OFFSET },
	{ .name = "s", .active_id = COW_VIEW_CFG_BORDER_ACTIVE_S,
	    .inactive_id = COW_VIEW_CFG_BORDER_INACTIVE_S,
	    .colour_offset = offsetof(struct cow_border_colour, s),
	    .has_offset = SET_NO_OFFSET },
	{ .name = "e", .active_id = COW_VIEW_CFG_BORDER_ACTIVE_E,
	    .inactive_id = COW_VIEW_CFG_BORDER_INACTIVE_E,
	    .colour_offset = offsetof(struct cow_border_colour, e),
	    .has_offset = SET_NO_OFFSET },
	{ .name = "w", .active_id = COW_VIEW_CFG_BORDER_ACTIVE_W,
	    .inactive_id = COW_VIEW_CFG_BORDER_INACTIVE_W,
	    .colour_offset = offsetof(struct cow_border_colour, w),
	    .has_offset = SET_NO_OFFSET },
	{ .name = "nw", .active_id = COW_VIEW_CFG_BORDER_ACTIVE_NW,
	    .inactive_id = COW_VIEW_CFG_BORDER_INACTIVE_NW,
	    .colour_offset = offsetof(struct cow_border_colour, nw),
	    .has_offset = offsetof(struct cow_border_colour, has_nw) },
	{ .name = "ne", .active_id = COW_VIEW_CFG_BORDER_ACTIVE_NE,
	    .inactive_id = COW_VIEW_CFG_BORDER_INACTIVE_NE,
	    .colour_offset = offsetof(struct cow_border_colour, ne),
	    .has_offset = offsetof(struct cow_border_colour, has_ne) },
	{ .name = "sw", .active_id = COW_VIEW_CFG_BORDER_ACTIVE_SW,
	    .inactive_id = COW_VIEW_CFG_BORDER_INACTIVE_SW,
	    .colour_offset = offsetof(struct cow_border_colour, sw),
	    .has_offset = offsetof(struct cow_border_colour, has_sw) },
	{ .name = "se", .active_id = COW_VIEW_CFG_BORDER_ACTIVE_SE,
	    .inactive_id = COW_VIEW_CFG_BORDER_INACTIVE_SE,
	    .colour_offset = offsetof(struct cow_border_colour, se),
	    .has_offset = offsetof(struct cow_border_colour, has_se) },
	{ .name = NULL },
};

static const struct set_option set_options[] = {
	{
		.name = "desks.count",
		.match = SET_MATCH_EXACT,
		.scope = SET_SCOPE_GLOBAL,
		.action = SET_ACTION_DESKS_COUNT,
	},
	{
		.name = "border",
		.match = SET_MATCH_EXACT,
		.scope = SET_SCOPE_MIXED,
		.action = SET_ACTION_SCALAR_INT,
		.id = COW_VIEW_CFG_BORDER,
		.config_offset = offsetof(struct cow_config, border),
		.view_offset = offsetof(struct cow_view_cfg, border),
		.lo = 0,
		.hi = INT_MAX,
		.post = SET_POST_REFRESH_VISUALS,
	},
	{
		.name = "step",
		.match = SET_MATCH_EXACT,
		.scope = SET_SCOPE_GLOBAL,
		.action = SET_ACTION_SCALAR_INT,
		.config_offset = offsetof(struct cow_config, step),
		.view_offset = SET_NO_OFFSET,
		.lo = 0,
		.hi = INT_MAX,
		.post = SET_POST_REFRESH_VISUALS,
	},
	{
		.name = "corner_length",
		.match = SET_MATCH_EXACT,
		.scope = SET_SCOPE_MIXED,
		.action = SET_ACTION_SCALAR_INT,
		.id = COW_VIEW_CFG_CORNER_LENGTH,
		.config_offset = offsetof(struct cow_config, corner_length),
		.view_offset = offsetof(struct cow_view_cfg, corner_length),
		.lo = 0,
		.hi = INT_MAX,
		.post = SET_POST_REFRESH_VISUALS | SET_POST_ZERO_DISABLE_HANDLES,
	},
	{
		.name = "reserved.top",
		.match = SET_MATCH_EXACT,
		.scope = SET_SCOPE_GLOBAL,
		.action = SET_ACTION_SCALAR_INT,
		.config_offset = offsetof(struct cow_config, reserved.top),
		.view_offset = SET_NO_OFFSET,
		.lo = 0,
		.hi = INT_MAX,
		.post = SET_POST_REFRESH_VISUALS,
	},
	{
		.name = "reserved.bottom",
		.match = SET_MATCH_EXACT,
		.scope = SET_SCOPE_GLOBAL,
		.action = SET_ACTION_SCALAR_INT,
		.config_offset = offsetof(struct cow_config, reserved.bottom),
		.view_offset = SET_NO_OFFSET,
		.lo = 0,
		.hi = INT_MAX,
		.post = SET_POST_REFRESH_VISUALS,
	},
	{
		.name = "reserved.left",
		.match = SET_MATCH_EXACT,
		.scope = SET_SCOPE_GLOBAL,
		.action = SET_ACTION_SCALAR_INT,
		.config_offset = offsetof(struct cow_config, reserved.left),
		.view_offset = SET_NO_OFFSET,
		.lo = 0,
		.hi = INT_MAX,
		.post = SET_POST_REFRESH_VISUALS,
	},
	{
		.name = "reserved.right",
		.match = SET_MATCH_EXACT,
		.scope = SET_SCOPE_GLOBAL,
		.action = SET_ACTION_SCALAR_INT,
		.config_offset = offsetof(struct cow_config, reserved.right),
		.view_offset = SET_NO_OFFSET,
		.lo = 0,
		.hi = INT_MAX,
		.post = SET_POST_REFRESH_VISUALS,
	},
	{
		.name = "icon.size",
		.match = SET_MATCH_EXACT,
		.scope = SET_SCOPE_MIXED,
		.action = SET_ACTION_SCALAR_INT,
		.id = COW_VIEW_CFG_ICON_SIZE,
		.config_offset = offsetof(struct cow_config, icon_size),
		.view_offset = offsetof(struct cow_view_cfg, icon_size),
		.lo = 0,
		.hi = INT_MAX,
		.post = SET_POST_REFRESH_VISUALS,
	},
	{
		.name = "snap_distance",
		.match = SET_MATCH_EXACT,
		.scope = SET_SCOPE_GLOBAL,
		.action = SET_ACTION_SCALAR_INT,
		.config_offset = offsetof(struct cow_config, snap_distance),
		.view_offset = SET_NO_OFFSET,
		.lo = 0,
		.hi = INT_MAX,
		.post = SET_POST_REFRESH_VISUALS,
	},
	{
		.name = "colour.",
		.match = SET_MATCH_PREFIX,
		.scope = SET_SCOPE_MIXED,
		.action = SET_ACTION_COLOUR,
	},
	{
		.name = "output.colour.background.",
		.match = SET_MATCH_PREFIX,
		.scope = SET_SCOPE_OUTPUT,
		.action = SET_ACTION_COLOUR,
	},
	{
		.name = "output.colour.",
		.match = SET_MATCH_PREFIX,
		.scope = SET_SCOPE_GLOBAL,
		.action = SET_ACTION_COLOUR,
	},
	{
		.name = "titlebar.active_colour",
		.match = SET_MATCH_EXACT,
		.scope = SET_SCOPE_MIXED,
		.action = SET_ACTION_COLOUR,
	},
	{
		.name = "titlebar.inactive_colour",
		.match = SET_MATCH_EXACT,
		.scope = SET_SCOPE_MIXED,
		.action = SET_ACTION_COLOUR,
	},
	{
		.name = "titlebar.fg_active",
		.match = SET_MATCH_EXACT,
		.scope = SET_SCOPE_MIXED,
		.action = SET_ACTION_COLOUR,
	},
	{
		.name = "titlebar.fg_inactive",
		.match = SET_MATCH_EXACT,
		.scope = SET_SCOPE_MIXED,
		.action = SET_ACTION_COLOUR,
	},
	{
		.name = "container.tab.",
		.match = SET_MATCH_PREFIX,
		.scope = SET_SCOPE_MIXED,
		.action = SET_ACTION_COLOUR,
	},
	{
		.name = "border_style",
		.match = SET_MATCH_EXACT,
		.scope = SET_SCOPE_MIXED,
		.action = SET_ACTION_BORDER_STYLE,
		.id = COW_VIEW_CFG_BORDER_STYLE,
	},
	{
		.name = "desktop-configuration",
		.match = SET_MATCH_EXACT,
		.scope = SET_SCOPE_GLOBAL,
		.action = SET_ACTION_DESKTOP_CONFIG,
	},
	{
		.name = "desktop_size",
		.match = SET_MATCH_EXACT,
		.scope = SET_SCOPE_GLOBAL,
		.action = SET_ACTION_DESKTOP_SIZE,
	},
	{
		.name = "focus",
		.match = SET_MATCH_EXACT,
		.scope = SET_SCOPE_GLOBAL,
		.action = SET_ACTION_FOCUS,
	},
	{
		.name = "placement",
		.match = SET_MATCH_EXACT,
		.scope = SET_SCOPE_GLOBAL,
		.action = SET_ACTION_PLACEMENT,
	},
	{
		.name = "font",
		.match = SET_MATCH_EXACT,
		.scope = SET_SCOPE_MIXED,
		.action = SET_ACTION_FONT,
		.id = COW_VIEW_CFG_FONT,
	},
	{
		.name = "titlebar.enabled",
		.match = SET_MATCH_EXACT,
		.scope = SET_SCOPE_MIXED,
		.action = SET_ACTION_SCALAR_BOOL,
		.id = COW_VIEW_CFG_TITLEBAR_ENABLED,
		.config_offset = offsetof(struct cow_config, titlebar.enabled),
		.view_offset = offsetof(struct cow_view_cfg, titlebar.enabled),
		.post = SET_POST_REFRESH_VISUALS,
	},
	{
		.name = "handles",
		.match = SET_MATCH_EXACT,
		.scope = SET_SCOPE_MIXED,
		.action = SET_ACTION_SCALAR_BOOL,
		.id = COW_VIEW_CFG_HANDLES,
		.config_offset = offsetof(struct cow_config, handles),
		.view_offset = offsetof(struct cow_view_cfg, handles),
		.post = SET_POST_REFRESH_VISUALS,
	},
	{
		.name = "titlebar.height",
		.match = SET_MATCH_EXACT,
		.scope = SET_SCOPE_MIXED,
		.action = SET_ACTION_TITLEBAR_HEIGHT,
		.id = COW_VIEW_CFG_TITLEBAR_HEIGHT,
	},
	{
		.name = "titlebar.squeeze",
		.match = SET_MATCH_EXACT,
		.scope = SET_SCOPE_MIXED,
		.action = SET_ACTION_TITLEBAR_SQUEEZE,
		.id = COW_VIEW_CFG_TITLEBAR_SQUEEZE,
	},
	{
		.name = "titlebar.squeeze.justify",
		.match = SET_MATCH_EXACT,
		.scope = SET_SCOPE_MIXED,
		.action = SET_ACTION_TITLEBAR_SQUEEZE_JUSTIFY,
		.id = COW_VIEW_CFG_TITLEBAR_SQUEEZE_JUSTIFY,
	},
	{
		.name = "titlebar.squeeze.width",
		.match = SET_MATCH_EXACT,
		.scope = SET_SCOPE_MIXED,
		.action = SET_ACTION_TITLEBAR_SQUEEZE_INT,
		.id = COW_VIEW_CFG_TITLEBAR_SQUEEZE_WIDTH,
		.config_offset = offsetof(struct cow_titlebar_squeeze, width),
		.lo = 0,
		.hi = 32767,
	},
	{
		.name = "titlebar.squeeze.offset",
		.match = SET_MATCH_EXACT,
		.scope = SET_SCOPE_MIXED,
		.action = SET_ACTION_TITLEBAR_SQUEEZE_INT,
		.id = COW_VIEW_CFG_TITLEBAR_SQUEEZE_OFFSET,
		.config_offset = offsetof(struct cow_titlebar_squeeze, offset),
		.lo = -32768,
		.hi = 32767,
	},
	{
		.name = "titlebar.squeeze.min_width",
		.match = SET_MATCH_EXACT,
		.scope = SET_SCOPE_MIXED,
		.action = SET_ACTION_TITLEBAR_SQUEEZE_INT,
		.id = COW_VIEW_CFG_TITLEBAR_SQUEEZE_MIN_WIDTH,
		.config_offset = offsetof(struct cow_titlebar_squeeze, min_width),
		.lo = 0,
		.hi = 32767,
	},
	{
		.name = "container.tab.sunken",
		.match = SET_MATCH_EXACT,
		.scope = SET_SCOPE_MIXED,
		.action = SET_ACTION_SCALAR_BOOL,
		.id = COW_VIEW_CFG_CONTAINER_TAB_SUNKEN,
		.config_offset = offsetof(struct cow_config,
		    container.tab_sunken_inactive),
		.view_offset = offsetof(struct cow_view_cfg,
		    container.tab_sunken_inactive),
		.post = SET_POST_REFRESH_VISUALS,
	},
	{
		.name = "edge_scroll",
		.match = SET_MATCH_EXACT,
		.scope = SET_SCOPE_GLOBAL,
		.action = SET_ACTION_SCALAR_BOOL,
		.config_offset = offsetof(struct cow_config, edge_scroll),
		.view_offset = SET_NO_OFFSET,
		.post = SET_POST_PAN_FRAMES,
	},
	{
		.name = "edge_scroll_delay_ms",
		.match = SET_MATCH_EXACT,
		.scope = SET_SCOPE_GLOBAL,
		.action = SET_ACTION_SCALAR_INT,
		.config_offset = offsetof(struct cow_config, edge_scroll_delay_ms),
		.view_offset = SET_NO_OFFSET,
		.lo = 50,
		.hi = 5000,
	},
	{
		.name = "page_scroll_animate",
		.match = SET_MATCH_EXACT,
		.scope = SET_SCOPE_GLOBAL,
		.action = SET_ACTION_SCALAR_BOOL,
		.config_offset = offsetof(struct cow_config, page_scroll_animate),
		.view_offset = SET_NO_OFFSET,
	},
	{
		.name = "page_scroll_ms",
		.match = SET_MATCH_EXACT,
		.scope = SET_SCOPE_GLOBAL,
		.action = SET_ACTION_SCALAR_INT,
		.config_offset = offsetof(struct cow_config, page_scroll_ms),
		.view_offset = SET_NO_OFFSET,
		.lo = 0,
		.hi = 5000,
	},
	{
		.name = "repeat_delay_ms",
		.match = SET_MATCH_EXACT,
		.scope = SET_SCOPE_GLOBAL,
		.action = SET_ACTION_SCALAR_INT,
		.config_offset = offsetof(struct cow_config, repeat_delay_ms),
		.view_offset = SET_NO_OFFSET,
		.lo = 0,
		.hi = 10000,
	},
	{
		.name = "repeat_interval_ms",
		.match = SET_MATCH_EXACT,
		.scope = SET_SCOPE_GLOBAL,
		.action = SET_ACTION_SCALAR_INT,
		.config_offset = offsetof(struct cow_config, repeat_interval_ms),
		.view_offset = SET_NO_OFFSET,
		.lo = 1,
		.hi = 10000,
	},
	{
		.name = "raise_on_focus",
		.match = SET_MATCH_EXACT,
		.scope = SET_SCOPE_GLOBAL,
		.action = SET_ACTION_SCALAR_BOOL,
		.config_offset = offsetof(struct cow_config, raise_on_focus),
		.view_offset = SET_NO_OFFSET,
	},
	{
		.name = "preserve_desk_on_output_loss",
		.match = SET_MATCH_EXACT,
		.scope = SET_SCOPE_GLOBAL,
		.action = SET_ACTION_SCALAR_BOOL,
		.config_offset = offsetof(struct cow_config,
		    preserve_desk_on_output_loss),
		.view_offset = SET_NO_OFFSET,
	},
	{
		.name = "titlebar.button_",
		.match = SET_MATCH_PREFIX,
		.scope = SET_SCOPE_MIXED,
		.action = SET_ACTION_BUTTON,
	},
	{
		.name = "default_layer",
		.match = SET_MATCH_EXACT,
		.scope = SET_SCOPE_GLOBAL,
		.action = SET_ACTION_LAYER,
	},
	{
		.name = "max_layer",
		.match = SET_MATCH_EXACT,
		.scope = SET_SCOPE_GLOBAL,
		.action = SET_ACTION_LAYER,
	},
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

static const struct set_option *
set_option_find(const char *key)
{
	for (const struct set_option *opt = set_options; opt->name != NULL;
	    opt++) {
		if (opt->match == SET_MATCH_EXACT && strcmp(opt->name, key) == 0)
			return (opt);
	}
	for (const struct set_option *opt = set_options; opt->name != NULL;
	    opt++) {
		size_t len;

		if (opt->match != SET_MATCH_PREFIX)
			continue;
		len = strlen(opt->name);
		if (strncmp(opt->name, key, len) == 0)
			return (opt);
	}
	return (NULL);
}

static bool
set_option_scope_ok(const struct set_option *opt, struct cmd_q *cmdq)
{
	if (opt->scope != SET_SCOPE_WINDOW)
		return (true);
	return (cmdq != NULL && cmdq->target.window != NULL);
}

static void
set_option_post_update(const struct set_option *opt)
{
	if ((opt->post & SET_POST_PAN_FRAMES) != 0 && !cow_ipc.startup_mode)
		pan_frames_update_all();
	if ((opt->post & SET_POST_REFRESH_VISUALS) != 0)
		refresh_visuals();
}

static enum cmd_retval
set_scalar_bool(const struct set_option *opt, const char *val,
    struct cmd_q *cmdq)
{
	bool b;

	if (!cmd_parse_bool(val, &b))
		return (CMD_RETURN_NORMAL);

	struct cow_view_cfg *cfg = cfg_target(cmdq);
	if (cfg != NULL && opt->view_offset != SET_NO_OFFSET) {
		bool *ptr = (bool *)((char *)cfg + opt->view_offset);
		*ptr = b;
		cow_view_cfg_set_override(cfg, opt->id);
	} else if (cow_config != NULL) {
		bool *ptr = (bool *)((char *)cow_config + opt->config_offset);
		*ptr = b;
		if (opt->scope == SET_SCOPE_MIXED)
			propagate_global_option(opt->id);
	}
	set_option_post_update(opt);
	return (CMD_RETURN_NORMAL);
}

static enum cmd_retval
set_scalar_int(const struct set_option *opt, const char *key, const char *val,
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
	if (cfg != NULL && opt->view_offset != SET_NO_OFFSET) {
		int *ptr = (int *)((char *)cfg + opt->view_offset);
		*ptr = (int)v;
		if ((opt->post & SET_POST_ZERO_DISABLE_HANDLES) != 0 && v == 0)
			cfg->handles = false;
		cow_view_cfg_set_override(cfg, opt->id);
	} else if (cow_config != NULL) {
		int *ptr = (int *)((char *)cow_config + opt->config_offset);
		*ptr = (int)v;
		if ((opt->post & SET_POST_ZERO_DISABLE_HANDLES) != 0 && v == 0)
			cow_config->handles = false;
		if (opt->scope == SET_SCOPE_MIXED)
			propagate_global_option(opt->id);
	}
	set_option_post_update(opt);
	return (CMD_RETURN_NORMAL);
}

static enum cmd_retval
set_option_exec(const struct set_option *opt, const char *key, const char *val,
    struct args *args, struct cmd_q *cmdq)
{
	switch (opt->action) {
	case SET_ACTION_SCALAR_INT:
		return (set_scalar_int(opt, key, val, cmdq));
	case SET_ACTION_SCALAR_BOOL:
		return (set_scalar_bool(opt, val, cmdq));
	case SET_ACTION_COLOUR:
		set_colour_option(key, val, cmdq);
		return (CMD_RETURN_NORMAL);
	case SET_ACTION_BORDER_STYLE:
		set_border_style_option(val, cmdq);
		return (CMD_RETURN_NORMAL);
	case SET_ACTION_DESKTOP_CONFIG:
		set_desktop_config_option(val, cmdq);
		return (CMD_RETURN_NORMAL);
	case SET_ACTION_FOCUS:
		set_focus_option(val);
		return (CMD_RETURN_NORMAL);
	case SET_ACTION_PLACEMENT:
		set_placement_option(val, cmdq);
		return (CMD_RETURN_NORMAL);
	case SET_ACTION_FONT:
		set_font_option(val, cmdq);
		return (CMD_RETURN_NORMAL);
	case SET_ACTION_BUTTON:
		set_button_option(key, val, cmdq);
		return (CMD_RETURN_NORMAL);
	case SET_ACTION_DESKS_COUNT:
		return (set_desks_count(val, cmdq));
	case SET_ACTION_DESKTOP_SIZE:
		return (set_desktop_size(args, cmdq));
	case SET_ACTION_TITLEBAR_HEIGHT:
		return (set_titlebar_height(val, cmdq));
	case SET_ACTION_TITLEBAR_SQUEEZE:
	case SET_ACTION_TITLEBAR_SQUEEZE_JUSTIFY:
	case SET_ACTION_TITLEBAR_SQUEEZE_INT:
		return (set_titlebar_squeeze(opt, key, val, cmdq));
	case SET_ACTION_LAYER:
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
	const struct set_option *opt = set_option_find(key);

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
set_titlebar_squeeze(const struct set_option *opt, const char *key,
    const char *val, struct cmd_q *cmdq)
{
	const char *errstr;
	struct cow_view_cfg	    *cfg;
	struct cow_titlebar_config *tb = target_titlebar(cmdq, &cfg);

	if (opt->action == SET_ACTION_TITLEBAR_SQUEEZE) {
		enum cow_squeeze_mode m;
		if (!cow_parse_squeeze_mode(val, &m)) {
			cmdq_error(cmdq,
			    "set: titlebar.squeeze: bad value: %s "
			    "(expected none|natural|fixed)",
			    val);
			return (CMD_RETURN_ERROR);
		}
		tb->squeeze.mode = m;
	} else if (opt->action == SET_ACTION_TITLEBAR_SQUEEZE_JUSTIFY) {
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

static void
set_colour_option(const char *key, const char *val, struct cmd_q *cmdq)
{
	if (set_output_colour(key, val, cmdq))
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
	bool is_active = (strcmp(key, "colour.active") == 0 ||
	    strncmp(key, "colour.active.", 14) == 0);
	bool is_inactive = (strcmp(key, "colour.inactive") == 0 ||
	    strncmp(key, "colour.inactive.", 16) == 0);

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
	if (strcmp(key, "colour.active") == 0 ||
	    strcmp(key, "colour.inactive") == 0) {
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
	if (edge_opt->has_offset != SET_NO_OFFSET) {
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
	struct cow_view_cfg	    *cfg;
	struct cow_titlebar_config *tb = target_titlebar(cmdq, &cfg);

	for (const struct set_button_option *opt = set_button_options;
	    opt->key != NULL; opt++) {
		if (strcmp(key, opt->key) != 0)
			continue;
		if (opt->part == SET_BUTTON_VECTOR) {
			cow_parse_vector_button_string(
			    &tb->buttons[opt->idx].vector, val);
			finish_visual_update(opt->id, cfg);
			return;
		}
		tb->buttons[opt->idx].action = set_button_action(val);
		finish_visual_update(opt->id, cfg);
		return;
	}
}

static enum cow_button_action
set_button_action(const char *val)
{
	for (const struct set_enum_option *opt = set_button_action_options;
	    opt->name != NULL; opt++) {
		if (strcmp(val, opt->name) == 0)
			return (opt->value);
	}
	return (COW_BUTTON_ACTION_NONE);
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
