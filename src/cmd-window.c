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

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "cow/cmd.h"
#include "cow/cmd-filter.h"
#include "cow/compat.h"
#include "cow/config.h"
#include "cow/desk_ops.h"
#include "cow/keybinding.h"
#include "cow/output.h"
#include "cow/titlebar.h"
#include "cow/view.h"
#include "cow/update.h"
#include "cow/wm.h"

#include "river-window-management-v1-client-protocol.h"

enum maximize_type {
	MAXIMIZE,
	MAXIMIZE_KEEP,
	UNMAXIMIZE,
	MAX_TOGGLE,
	MAX_TOGGLE_KEEP,
};

/* Maximize axes. */
enum maximize_axis {
	MAX_AXIS_BOTH,
	MAX_AXIS_H,
	MAX_AXIS_V,
};

enum expand_dir {
	EXPAND_ERROR = -1,
	EXPAND_ALL   = 0,
	EXPAND_LEFT  = 1,
	EXPAND_RIGHT = 2,
	EXPAND_UP    = 3,
	EXPAND_DOWN  = 4,
};

static enum cmd_retval cmd_winops_exec(struct cmd *, struct cmd_q *);
static enum cmd_retval winops_apply(struct cow_view *, struct args *,
    struct cmd_q *);
static enum cmd_retval cmd_window_expand_exec(struct cmd *, struct cmd_q *);
static enum cmd_retval cmd_window_maximize_exec(struct cmd *, struct cmd_q *);
static enum cmd_retval cmd_window_shade_exec(struct cmd *, struct cmd_q *);
static enum cmd_retval cmd_window_tile_exec(struct cmd *, struct cmd_q *);

static void		    do_raise(struct cow_view *);
static void		    do_lower(struct cow_view *);
static enum cmd_retval	    do_set_layer(struct cow_view *, const char *,
	 struct cmd_q *);
static enum cmd_retval	    do_set_circulate_skip(struct cow_view *,
	 const char *, struct cmd_q *);
static void		    do_expand(struct cow_view *, enum expand_dir, bool);
static bool		    do_maximize(struct cow_view *, enum maximize_type,
		    enum maximize_axis, bool, struct cmd_q *);
static void		    do_shade(struct cow_view *, const char *);
static bool		    parse_tiled_edges(const char *, uint32_t *);
static enum expand_dir	    resolve_direction(const char *);
static uint32_t		    expand_tiled_edges(enum expand_dir);
static enum shade_direction resolve_shade_direction(struct cow_view *,
    const char *);

struct cmd_entry cmd_winops_entry = {
	"winops",
	"F:t:crlsiSL:C:",
	0,
	0,
	"[-cilrs] [-C on|off|toggle] [-L layer] [-t target window] "
	"[-F expr]",
	cmd_winops_exec,
	0,
};

static enum cmd_retval
cmd_winops_exec(struct cmd *self, struct cmd_q *cmdq)
{
	struct args *args = self->args;
	const char *filter_expr = args_get(args, 'F');

	if (filter_expr == NULL) {
		struct cow_view *view = cmd_require_target_window(cmdq, "winops");
		if (view == NULL)
			return (CMD_RETURN_ERROR);
		return (winops_apply(view, args, cmdq));
	}
	if (args_has(args, 't')) {
		cmdq_error(cmdq, "winops: -F cannot be combined with -t");
		return (CMD_RETURN_ERROR);
	}

	struct cmd_filter filter;
	if (!cmd_filter_parse(&filter, cmdq, "winops", "-F", filter_expr))
		return (CMD_RETURN_ERROR);

	enum cmd_retval retval = CMD_RETURN_NORMAL;
	struct cow_view *view;
	wl_list_for_each(view, &cow_wm.views, wm_link)
	{
		if (view->closed)
			continue;
		bool matches = false;
		if (!cmd_filter_view(&filter, view, &matches))
			break;
		if (!matches)
			continue;
		retval = winops_apply(view, args, cmdq);
		if (retval != CMD_RETURN_NORMAL)
			break;
	}
	if (!cmd_filter_finish(&filter, cmdq))
		retval = CMD_RETURN_ERROR;
	cmd_filter_free(&filter);
	return (retval);
}

static enum cmd_retval
winops_apply(struct cow_view *view, struct args *args, struct cmd_q *cmdq)
{
	/* Close -- deferred to manage callback */
	if (args_has(args, 'c')) {
		view->pending_close = true;
		cow_update_request(COW_UPDATE_MANAGE);
		return (CMD_RETURN_NORMAL);
	}

	if (args_has(args, 'r'))
		do_raise(view);
	if (args_has(args, 'l'))
		do_lower(view);

	 /* Sticky-flag.  -s toggles desk-sticky, -S toggles page-sticky. */
	if (args_has(args, 's')) {
		if (!cmdq->in_style)
			view->policy_overrides |= COW_VIEW_POLICY_STICKY_DESK;
		bool currently = (view->flags & COW_VIEW_STICKY_DESK_FLAG) != 0;
		bool target    = cmdq->in_rule ? true : !currently;
		if (target != currently) {
			view->flags ^= COW_VIEW_STICKY_DESK_FLAG;
			cow_container_sync_flags(view,
			    COW_VIEW_STICKY_DESK_FLAG);
			view->dec.dirty = true;
			cow_update_request(COW_UPDATE_MANAGE | COW_UPDATE_STATUS);
			/* Becoming non-sticky: assign view back to the
			 * output's current desk so it's actually on the
			 * desk it appears to be on.
			 */
			if (currently) {
				uint8_t nr = (view->output != NULL) ?
				    view->output->current_desk->nr :
				    view->desk->nr;
				cow_view_assign_desk(view, nr);
			}
		}
	}

	if (args_has(args, 'S')) {
		if (!cmdq->in_style)
			view->policy_overrides |= COW_VIEW_POLICY_STICKY_PAGE;
		bool currently = (view->flags & COW_VIEW_STICKY_PAGE_FLAG) != 0;
		bool target    = cmdq->in_rule ? true : !currently;
		cow_view_set_page_sticky(view, target);
	}

	if (args_has(args, 'i')) {
		if (cow_view_is_iconified(view))
			cow_view_deiconify(view);
		else
			cow_view_iconify(view);
	}

	if (args_has(args, 'L')) {
		const char     *lval = args_get(args, 'L');
		enum cmd_retval r    = do_set_layer(view, lval, cmdq);
		if (r != CMD_RETURN_NORMAL)
			return (r);
		if (!cmdq->in_style) {
			view->policy_overrides |= COW_VIEW_POLICY_LAYER;
			if (view->container_member != NULL) {
				struct cow_container *container =
				    view->container_member->container;
				struct cow_container_member *member;
				wl_list_for_each(member, &container->members, link)
				{
					if (member->view != NULL)
						member->view->policy_overrides |=
						    COW_VIEW_POLICY_LAYER;
				}
			}
		}
	}

	if (args_has(args, 'C')) {
		const char     *cval = args_get(args, 'C');
		enum cmd_retval r    =
		    do_set_circulate_skip(view, cval, cmdq);
		if (r != CMD_RETURN_NORMAL)
			return (r);
		if (!cmdq->in_style)
			view->policy_overrides |= COW_VIEW_POLICY_CIRCULATE_SKIP;
	}

	return (CMD_RETURN_NORMAL);
}

struct cmd_entry cmd_window_tile_entry = {
	"window-tile",
	"t:e:u",
	0,
	0,
	"window-tile [-t target] [-e edges] [-u]",
	cmd_window_tile_exec,
	CMD_MODULE,
};

static enum cmd_retval
cmd_window_tile_exec(struct cmd *self, struct cmd_q *cmdq)
{
	struct args	*args = self->args;
	struct cow_view *view = cmd_require_target_window(cmdq, "window-tile");

	if (view == NULL)
		return (CMD_RETURN_ERROR);

	uint32_t edges = RIVER_WINDOW_V1_EDGES_TOP |
	    RIVER_WINDOW_V1_EDGES_BOTTOM | RIVER_WINDOW_V1_EDGES_LEFT |
	    RIVER_WINDOW_V1_EDGES_RIGHT;

	if (args_has(args, 'u'))
		edges = RIVER_WINDOW_V1_EDGES_NONE;
	else if (args_has(args, 'e') &&
	    !parse_tiled_edges(args_get(args, 'e'), &edges)) {
		cmdq_error(cmdq,
		    "window-tile: -e: bad edge spec '%s' "
		    "(expected all|none|n|s|e|w combinations)",
		    args_get(args, 'e'));
		return (CMD_RETURN_ERROR);
	}

	view->expanded = false;
	cow_view_set_tiled_edges(view, edges);
	view->dec.dirty = true;
	cow_update_request(COW_UPDATE_MANAGE);

	return (CMD_RETURN_NORMAL);
}

struct cmd_entry cmd_window_expand_entry = {
	"window-expand",
	"t:d:T",
	0,
	0,
	"[-t target] [-d direction] [-T]",
	cmd_window_expand_exec,
	0,
};

static enum cmd_retval
cmd_window_expand_exec(struct cmd *self, struct cmd_q *cmdq)
{
	struct args	*args = self->args;
	struct cow_view *view = cmd_require_target_window(cmdq,
	    "window-expand");
	bool		 toggle;

	if (view == NULL)
		return (CMD_RETURN_ERROR);

	struct cow_container *c = cow_view_container(view);
	if (c && c->frame_view)
		view = c->frame_view;

	enum expand_dir dir = resolve_direction(args_get(args, 'd'));
	if (dir == EXPAND_ERROR) {
		cmdq_error(cmdq, "window-expand: invalid direction");
		return (CMD_RETURN_ERROR);
	}

	toggle = args_has(args, 'T');

	do_expand(view, dir, toggle);

	cow_update_request(COW_UPDATE_MANAGE);
	view->dec.dirty = true;

	return (CMD_RETURN_NORMAL);
}

struct cmd_entry cmd_window_maximize_entry = {
	"window-maximize",
	"t:TmuhvkR",
	0,
	0,
	"window-maximize [-t target] [-T] [-m] [-u] [-h] [-v] [-k] [-R]",
	cmd_window_maximize_exec,
	0,
};

static enum cmd_retval
cmd_window_maximize_exec(struct cmd *self, struct cmd_q *cmdq)
{
	struct args	*args = self->args;
	struct cow_view *view = cmd_require_target_window(cmdq,
	    "window-maximize");

	if (view == NULL)
		return (CMD_RETURN_ERROR);
	struct cow_container *c = cow_view_container(view);
	if (c && c->frame_view)
		view = c->frame_view;

	bool		   want_h = args_has(args, 'h');
	bool		   want_v = args_has(args, 'v');
	enum maximize_axis axis;
	if (want_h && !want_v)
		axis = MAX_AXIS_H;
	else if (want_v && !want_h)
		axis = MAX_AXIS_V;
	else
		axis = MAX_AXIS_BOTH;

	bool ok;
	if (args_has(args, 'T'))
		ok = do_maximize(view, args_has(args, 'k') ? MAX_TOGGLE_KEEP :
		    MAX_TOGGLE, axis, args_has(args, 'R'), cmdq);
	else if (args_has(args, 'u'))
		ok = do_maximize(view, UNMAXIMIZE, axis, false, cmdq);
	else
		ok = do_maximize(view, args_has(args, 'k') ? MAXIMIZE_KEEP :
		    MAXIMIZE, axis, args_has(args, 'R'), cmdq);
	if (!ok)
		return (CMD_RETURN_ERROR);

	cow_update_request(COW_UPDATE_MANAGE | COW_UPDATE_STATUS);
	view->dec.dirty = true;

	return (CMD_RETURN_NORMAL);
}

struct cmd_entry cmd_window_shade_entry = {
	"window-shade",
	"t:d:",
	0,
	0,
	"window-shade [-t target] [-d direction]",
	cmd_window_shade_exec,
	0,
};

static enum cmd_retval
cmd_window_shade_exec(struct cmd *self, struct cmd_q *cmdq)
{
	struct args	*args = self->args;
	struct cow_view *view = cmd_require_target_window(cmdq, "window-shade");

	if (view == NULL)
		return (CMD_RETURN_ERROR);

	if (!args_has(args, 'd')) {
		cmdq_error(cmdq, "No direction given");
		return (CMD_RETURN_ERROR);
	}

	do_shade(view, args_get(args, 'd'));

	return (CMD_RETURN_NORMAL);
}

static void
do_raise(struct cow_view *view)
{
	view->needs_raise = true;
	cow_update_request(COW_UPDATE_MANAGE);
}

static void
do_lower(struct cow_view *view)
{
	view->pending_lower = true;
	cow_update_request(COW_UPDATE_MANAGE);
}

/*
 * Parse the -L argument and apply.  Grammar:
 *
 *   N           -- absolute layer (clamped to [0, max_layer]).
 *   =N          -- absolute layer (explicit; never toggles).
 *   +N / -N     -- relative bump (clamped at the edges).
 *   top         -- shorthand for max_layer; toggles back to default
 *                  when view->layer == max_layer.
 *   bottom      -- shorthand for 0;         toggles back to default
 *                  when view->layer == 0.
 *   =top        -- max_layer, never toggles (useful inside rules).
 *   =bottom     -- 0,         never toggles.
 *
 * The toggle only fires at the exact endpoint; layer 5 with `-L top`
 * goes to max_layer, never back via the same keystroke.
 */
static enum cmd_retval
do_set_layer(struct cow_view *view, const char *arg, struct cmd_q *cmdq)
{
	if (arg == NULL || arg[0] == '\0') {
		cmdq_error(cmdq, "winops: -L: missing layer value");
		return (CMD_RETURN_ERROR);
	}

	int max	 = (cow_config != NULL) ? (int)cow_config->max_layer :
					  COW_CFG_MAX_LAYER;
	int defl = (cow_config != NULL) ? (int)cow_config->default_layer :
					  COW_CFG_DEFAULT_LAYER;
	if (defl > max)
		defl = max;

	bool	    explicit_set = cmdq->in_rule;
	const char *p		 = arg;
	if (*p == '=') {
		explicit_set = true;
		p++;
	}

	int target;
	if (strcmp(p, "top") == 0) {
		target = max;
		if (!explicit_set && view->layer == (uint8_t)max)
			target = defl;
	} else if (strcmp(p, "bottom") == 0) {
		target = 0;
		if (!explicit_set && view->layer == 0)
			target = defl;
	} else if (p[0] == '+' || p[0] == '-') {
		const char *errstr;
		long long   delta = strtonum(p + 1, 0, 255, &errstr);
		if (errstr != NULL) {
			cmdq_error(cmdq, "winops: -L: bad relative value: %s",
			    arg);
			return (CMD_RETURN_ERROR);
		}
		target = (int)view->layer +
		    ((p[0] == '+') ? (int)delta : -(int)delta);
	} else {
		const char *errstr;
		long long   v = strtonum(p, 0, 255, &errstr);
		if (errstr != NULL) {
			cmdq_error(cmdq,
			    "winops: -L: expected [+|-|=]N or "
			    "[=]top|[=]bottom, got '%s'",
			    arg);
			return (CMD_RETURN_ERROR);
		}
		target = (int)v;
	}

	cow_view_set_layer(view, target);
	return (CMD_RETURN_NORMAL);
}

static enum cmd_retval
do_set_circulate_skip(struct cow_view *view, const char *arg,
    struct cmd_q *cmdq)
{
	if (arg == NULL || arg[0] == '\0') {
		cmdq_error(cmdq,
		    "winops: -C: expected on, off, or toggle");
		return (CMD_RETURN_ERROR);
	}

	bool currently =
	    (view->flags & COW_VIEW_CIRCULATE_SKIP_FLAG) != 0;
	bool target;
	if (strcmp(arg, "on") == 0)
		target = true;
	else if (strcmp(arg, "off") == 0)
		target = false;
	else if (strcmp(arg, "toggle") == 0)
		target = !currently;
	else {
		cmdq_error(cmdq,
		    "winops: -C: expected on, off, or toggle, got '%s'",
		    arg);
		return (CMD_RETURN_ERROR);
	}

	if (target == currently)
		return (CMD_RETURN_NORMAL);

	if (target)
		view->flags |= COW_VIEW_CIRCULATE_SKIP_FLAG;
	else
		view->flags &= ~COW_VIEW_CIRCULATE_SKIP_FLAG;
	cow_update_request(COW_UPDATE_STATUS);

	return (CMD_RETURN_NORMAL);
}

/* Resolve which maximized axes a restore operation should touch. */
static bool
maxaxis_to_restore(const struct cow_view *view, enum maximize_axis axis,
    bool *touch_h, bool *touch_v)
{
	switch (axis) {
	case MAX_AXIS_BOTH:
		*touch_h = view->maximized_h;
		*touch_v = view->maximized_v;
		break;
	case MAX_AXIS_H:
		*touch_h = view->maximized_h;
		*touch_v = false;
		break;
	case MAX_AXIS_V:
		*touch_h = false;
		*touch_v = view->maximized_v;
		break;
	}
	return *touch_h || *touch_v;
}

static bool
do_maximize(struct cow_view *view, enum maximize_type mt,
    enum maximize_axis axis, bool ignore_reserved, struct cmd_q *cmdq)
{
	if (cow_container_is_split(view)) {
		bool h = axis != MAX_AXIS_V, v = axis != MAX_AXIS_H;
		bool restore = mt == UNMAXIMIZE ||
		    ((mt == MAX_TOGGLE || mt == MAX_TOGGLE_KEEP) &&
			((h && view->maximized_h) || (v && view->maximized_v)));
		if (!cow_container_maximize(view, h, v, restore,
			mt == MAXIMIZE_KEEP || mt == MAX_TOGGLE_KEEP,
			ignore_reserved)) {
			cmdq_error(cmdq,
			    "container does not fit the requested area");
			return false;
		}
		return true;
	}
	if (view->output == NULL) {
		cmdq_error(cmdq, "window-maximize: target window has no output");
		return false;
	}
	if (view->river_window == NULL) {
		cmdq_error(cmdq, "window-maximize: target window is not managed");
		return false;
	}

	cow_view_clear_expanded_state(view);

	/* Restore only the requested maximized axes. */
	if (mt == UNMAXIMIZE || mt == MAX_TOGGLE || mt == MAX_TOGGLE_KEEP) {
		bool touch_h, touch_v;
		if (maxaxis_to_restore(view, axis, &touch_h, &touch_v)) {
			/* Preserve the untouched axis. */
			int gx = view->x;
			int gy = view->y;
			int gw = view->content_width;
			int gh = view->content_height;
			if (touch_h && view->output != NULL &&
			    view->desk != NULL) {
				gx = view->output->x +
				    (view->saved_x - view->desk->page_vx);
				gw = view->saved_width;
			}
			if (touch_v && view->output != NULL &&
			    view->desk != NULL) {
				gy = view->output->y +
				    (view->saved_y - view->desk->page_vy);
				gh = view->saved_height;
			}
			cow_view_set_pending_geometry(view, gx, gy, gw, gh);

			if (view->container_member != NULL)
				cow_view_sync_container_geometry(view, gx, gy,
				    gw, gh);
			if (touch_h)
				view->maximized_h = false;
			if (touch_v)
				view->maximized_v = false;
			if (touch_h)
				view->maximized_h_ignore_reserved = false;
			if (touch_v)
				view->maximized_v_ignore_reserved = false;
			if (touch_h)
				view->maximized_h_keep_geometry = false;
			if (touch_v)
				view->maximized_v_keep_geometry = false;
			view->maximized = view->maximized_h ||
			    view->maximized_v;
			if (!view->maximized) {
				cow_view_set_pending_maximized_hint(view, false);
				view->icon_inverted_button = -1;
			}
			return true;
		}
		/* Toggle with no restore state becomes maximize. */
		if (mt == UNMAXIMIZE)
			return true;
		if (mt == MAX_TOGGLE_KEEP)
			mt = MAXIMIZE_KEEP;
	}

	struct cow_box area = ignore_reserved ? (struct cow_box) {
	    .x = view->output->x,
	    .y = view->output->y,
	    .width = view->output->width,
	    .height = view->output->height,
	} : view->output->usable_area;
	int frame_w, frame_h, content_x, content_y;
	cow_titlebar_frame_geometry(view, view->content_width,
	    view->content_height, &frame_w, &frame_h, &content_x, &content_y);
	int frame_extra_w = frame_w - view->content_width;
	int frame_extra_h = frame_h - view->content_height;

	/* Save only axes that do not already have pre-maximize geometry. */
	if ((axis == MAX_AXIS_BOTH || axis == MAX_AXIS_H) &&
	    !view->maximized_h) {
		view->saved_x	  = view->vx;
		view->saved_width = view->content_width;
	}
	if ((axis == MAX_AXIS_BOTH || axis == MAX_AXIS_V) &&
	    !view->maximized_v) {
		view->saved_y	   = view->vy;
		view->saved_height = view->content_height;
	}

	if (mt != MAXIMIZE_KEEP) {
		int new_x = view->x;
		int new_y = view->y;
		int new_w = view->content_width;
		int new_h = view->content_height;
		if (axis == MAX_AXIS_BOTH || axis == MAX_AXIS_H) {
			new_x = area.x + content_x;
			new_w = area.width - frame_extra_w;
		}
		if (axis == MAX_AXIS_BOTH || axis == MAX_AXIS_V) {
			new_y = area.y + content_y;
			new_h = area.height - frame_extra_h;
		}
		cow_clamp_dimensions(&new_w, &new_h);
		cow_view_set_pending_geometry(view, new_x, new_y, new_w, new_h);

		if (view->container_member != NULL)
			cow_view_sync_container_geometry(view, new_x, new_y,
			    new_w, new_h);
	}

	if (axis == MAX_AXIS_BOTH || axis == MAX_AXIS_H) {
		view->maximized_h = true;
		view->maximized_h_ignore_reserved = ignore_reserved;
		view->maximized_h_keep_geometry = mt == MAXIMIZE_KEEP;
	}
	if (axis == MAX_AXIS_BOTH || axis == MAX_AXIS_V) {
		view->maximized_v = true;
		view->maximized_v_ignore_reserved = ignore_reserved;
		view->maximized_v_keep_geometry = mt == MAXIMIZE_KEEP;
	}
	view->maximized = view->maximized_h || view->maximized_v;
	cow_view_set_pending_maximized_hint(view, true);

	int mbi = cow_view_maximize_button_index();
	if (mbi >= 0)
		view->icon_inverted_button = mbi;
	return true;
}

static uint32_t
expand_tiled_edges(enum expand_dir dir)
{
	switch (dir) {
	case EXPAND_ALL:
		return RIVER_WINDOW_V1_EDGES_TOP |
		    RIVER_WINDOW_V1_EDGES_BOTTOM |
		    RIVER_WINDOW_V1_EDGES_LEFT |
		    RIVER_WINDOW_V1_EDGES_RIGHT;
	case EXPAND_LEFT:
		return RIVER_WINDOW_V1_EDGES_LEFT;
	case EXPAND_RIGHT:
		return RIVER_WINDOW_V1_EDGES_RIGHT;
	case EXPAND_UP:
		return RIVER_WINDOW_V1_EDGES_TOP;
	case EXPAND_DOWN:
		return RIVER_WINDOW_V1_EDGES_BOTTOM;
	case EXPAND_ERROR:
	default:
		return RIVER_WINDOW_V1_EDGES_NONE;
	}
}

static enum expand_dir
resolve_direction(const char *dir_str)
{
	if (dir_str == NULL)
		return (EXPAND_ALL);

	switch (cow_parse_direction(dir_str)) {
	case COW_DIR_LEFT:
		return (EXPAND_LEFT);
	case COW_DIR_RIGHT:
		return (EXPAND_RIGHT);
	case COW_DIR_UP:
		return (EXPAND_UP);
	case COW_DIR_DOWN:
		return (EXPAND_DOWN);
	case COW_DIR_NONE:
	default:
		return (EXPAND_ERROR);
	}
}

static enum shade_direction
resolve_shade_direction(struct cow_view *view, const char *dir_str)
{
	int dir = COW_SHADE_NORTH;

	if (dir_str == NULL)
		return (dir);

	if (strcmp(dir_str, "titlebar") == 0) {
		switch (view->cfg.titlebar.position) {
		case COW_TITLEBAR_BOTTOM:
			dir = COW_SHADE_SOUTH;
			break;
		case COW_TITLEBAR_LEFT:
			dir = COW_SHADE_WEST;
			break;
		case COW_TITLEBAR_RIGHT:
			dir = COW_SHADE_EAST;
			break;
		case COW_TITLEBAR_TOP:
		default:
			dir = COW_SHADE_NORTH;
			break;
		}
	} else if (strcmp(dir_str, "north") == 0)
		dir = COW_SHADE_NORTH;
	else if (strcmp(dir_str, "south") == 0)
		dir = COW_SHADE_SOUTH;
	else if (strcmp(dir_str, "east") == 0)
		dir = COW_SHADE_EAST;
	else if (strcmp(dir_str, "west") == 0)
		dir = COW_SHADE_WEST;
	else if (strcmp(dir_str, "nw") == 0)
		dir = COW_SHADE_NW;
	else if (strcmp(dir_str, "ne") == 0)
		dir = COW_SHADE_NE;
	else if (strcmp(dir_str, "sw") == 0)
		dir = COW_SHADE_SW;
	else if (strcmp(dir_str, "se") == 0)
		dir = COW_SHADE_SE;
	else
		dir = COW_SHADE_ERROR;

	return (dir);
}

static void
do_expand(struct cow_view *view, enum expand_dir dir, bool toggle)
{
	if (view->output == NULL)
		return;

	if (dir == EXPAND_ERROR)
		return;

	/* Toggle: if already expanded, restore saved geometry. */
	if (toggle && view->expanded) {
		cow_view_restore_expanded_geometry(view);
		return;
	}

	struct cow_box *ua = &view->output->usable_area;
	int frame_w, frame_h, content_x, content_y;
	cow_titlebar_frame_geometry(view, view->content_width,
	    view->content_height, &frame_w, &frame_h, &content_x, &content_y);
	int frame_extra_w = frame_w - view->content_width;
	int frame_extra_h = frame_h - view->content_height;

	if (!view->expanded)
		cow_view_save_expanded_geometry(view);

	struct cow_box fb  = cow_view_frame_box(view);
	int	       fx2 = fb.x + fb.width;
	int	       fy2 = fb.y + fb.height;

	/* Start with current frame edges for constrained directions,
	 * usable area for unconstrained directions. */
	int grow_left  = (dir == EXPAND_ALL || dir == EXPAND_LEFT) ? ua->x :
								     fb.x;
	int grow_top   = (dir == EXPAND_ALL || dir == EXPAND_UP) ? ua->y : fb.y;
	int grow_right = (dir == EXPAND_ALL || dir == EXPAND_RIGHT) ?
	    ua->x + ua->width :
	    fx2;
	int grow_bottom = (dir == EXPAND_ALL || dir == EXPAND_DOWN) ?
	    ua->y + ua->height :
	    fy2;

	struct cow_container *group = cow_container_top_group(view);
	struct cow_view *other;
	wl_list_for_each(other, &cow_wm.views, wm_link)
	{
		if (other == view || other->closed || !other->has_dimensions)
			continue;
		if (group && cow_container_top_group(other) == group)
			continue;
		if (other->output != view->output)
			continue;
		if (other->desk != view->desk)
			continue;
		if (other->flags & COW_VIEW_ICONIFIED_FLAG)
			continue;

		struct cow_box ob  = cow_view_frame_box(other);
		int	       ox2 = ob.x + ob.width;
		int	       oy2 = ob.y + ob.height;

		/* Horizontal constraints (if growing left or right) */
		if (ob.y < fy2 && oy2 > fb.y) {
			if ((dir == EXPAND_ALL || dir == EXPAND_LEFT) &&
			    ox2 <= fb.x && ox2 > grow_left)
				grow_left = ox2;
			if ((dir == EXPAND_ALL || dir == EXPAND_RIGHT) &&
			    ob.x >= fx2 && ob.x < grow_right)
				grow_right = ob.x;
		}

		/* Vertical constraints (if growing up or down) */
		if (ob.x < fx2 && ox2 > fb.x) {
			if ((dir == EXPAND_ALL || dir == EXPAND_UP) &&
			    oy2 <= fb.y && oy2 > grow_top)
				grow_top = oy2;
			if ((dir == EXPAND_ALL || dir == EXPAND_DOWN) &&
			    ob.y >= fy2 && ob.y < grow_bottom)
				grow_bottom = ob.y;
		}
	}

	int new_x = grow_left + content_x;
	int new_y = grow_top + content_y;
	int new_w = (grow_right - grow_left) - frame_extra_w;
	int new_h = (grow_bottom - grow_top) - frame_extra_h;

	if (new_w < 50)
		new_w = 50;
	if (new_h < 50)
		new_h = 50;

	cow_view_set_pending_geometry(view, new_x, new_y, new_w, new_h);
	/* Container parity: see do_maximize.  Without this, expand on a
	 * container member leaves the container's stored origin stale
	 * and the expanded content renders at the wrong position. */
	if (view->container_member != NULL)
		cow_view_sync_container_geometry(view, new_x, new_y, new_w,
		    new_h);
	if (view->maximized) {
		cow_view_set_maximized(view, false);
		cow_view_set_pending_maximized_hint(view, false);
		view->icon_inverted_button = -1;
		view->dec.dirty		   = true;
	}
	view->expanded = true;
	cow_view_set_tiled_edges(view, expand_tiled_edges(dir));
}

static void
do_shade(struct cow_view *view, const char *dir_str)
{
	enum shade_direction sd = resolve_shade_direction(view, dir_str);

	cow_view_shade(view, sd);
}

static bool
parse_tiled_edges(const char *spec, uint32_t *out)
{
	if (spec == NULL || *spec == '\0')
		return false;
	if (strcmp(spec, "all") == 0) {
		*out = RIVER_WINDOW_V1_EDGES_TOP |
		    RIVER_WINDOW_V1_EDGES_BOTTOM |
		    RIVER_WINDOW_V1_EDGES_LEFT | RIVER_WINDOW_V1_EDGES_RIGHT;
		return true;
	}
	if (strcmp(spec, "none") == 0) {
		*out = RIVER_WINDOW_V1_EDGES_NONE;
		return true;
	}

	uint32_t edges = RIVER_WINDOW_V1_EDGES_NONE;
	for (const char *p = spec; *p != '\0'; p++) {
		switch (*p) {
		case 'n':
		case 'N':
			edges |= RIVER_WINDOW_V1_EDGES_TOP;
			break;
		case 's':
		case 'S':
			edges |= RIVER_WINDOW_V1_EDGES_BOTTOM;
			break;
		case 'e':
		case 'E':
			edges |= RIVER_WINDOW_V1_EDGES_RIGHT;
			break;
		case 'w':
		case 'W':
			edges |= RIVER_WINDOW_V1_EDGES_LEFT;
			break;
		default:
			return false;
		}
	}

	*out = edges;
	return true;
}
