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
#include <limits.h>
#include <string.h>

#include "cow/cmd.h"
#include "cow/config.h"
#include "cow/desk_ops.h"
#include "cow/ipc.h"
#include "cow/keybinding.h"
#include "cow/output.h"
#include "cow/view.h"
#include "cow/wm.h"

enum maximize_type {
	MAXIMIZE,
	MAXIMIZE_KEEP,
	UNMAXIMIZE,
	MAX_TOGGLE,
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
static enum cmd_retval cmd_window_expand_exec(struct cmd *, struct cmd_q *);
static enum cmd_retval cmd_window_maximize_exec(struct cmd *, struct cmd_q *);
static enum cmd_retval cmd_window_shade_exec(struct cmd *, struct cmd_q *);

static void		    do_identify(struct cow_view *, struct cmd_q *);
static void		    do_raise(struct cow_view *);
static void		    do_lower(struct cow_view *);
static void		    do_expand(struct cow_view *, enum expand_dir, bool);
static void		    do_maximize(struct cow_view *, enum maximize_type);
static void		    do_shade(struct cow_view *, const char *);
static enum expand_dir	    resolve_direction(const char *);
static enum shade_direction resolve_shade_direction(const char *);

struct cmd_entry cmd_winops_entry = { "winops", "t:IcrlsiS", 0, 0,
	"[-Icilrs] [-t target window]", cmd_winops_exec };

static enum cmd_retval
cmd_winops_exec(struct cmd *self, struct cmd_q *cmdq)
{
	struct args	*args = self->args;
	struct cow_view *view = cmd_ctx_target_window(cmdq);

	if (view == NULL || view->closed) {
		cmdq_error(cmdq, "winops: no target window");
		return (CMD_RETURN_ERROR);
	}

	/* Close -- deferred to manage callback */
	if (args_has(args, 'c')) {
		view->pending_close	  = true;
		cow_wm.needs_manage_dirty = true;
		return (CMD_RETURN_NORMAL);
	}

	if (args_has(args, 'I'))
		do_identify(view, cmdq);

	if (args_has(args, 'r'))
		do_raise(view);
	if (args_has(args, 'l'))
		do_lower(view);

	if (args_has(args, 's')) {
		/* -s: toggle desk-sticky (visible on all desks) */
		bool was_sticky = (view->flags & COW_VIEW_STICKY_DESK_FLAG) !=
		    0;

		view->flags ^= COW_VIEW_STICKY_DESK_FLAG;
		view->dec.dirty		  = true;
		cow_wm.needs_manage_dirty = true;

		if (was_sticky) {
			uint8_t target = (view->output != NULL) ?
			    view->output->current_desk->nr :
			    view->desk->nr;
			cow_view_assign_desk(view, target);
		}
	}

	if (args_has(args, 'S')) {
		/* -S: toggle page-sticky (visible on all pages of current desk)
		 */
		view->flags ^= COW_VIEW_STICKY_PAGE_FLAG;
		view->dec.dirty		  = true;
		cow_wm.needs_manage_dirty = true;
	}

	if (args_has(args, 'i'))
		cow_view_iconify(view);

	return (CMD_RETURN_NORMAL);
}

struct cmd_entry cmd_window_expand_entry = {
	"window-expand",
	"t:d:T",
	0,
	0,
	"[-t target] [-d direction] [-T]",
	cmd_window_expand_exec,
};

static enum cmd_retval
cmd_window_expand_exec(struct cmd *self, struct cmd_q *cmdq)
{
	struct args	*args = self->args;
	struct cow_view *view = cmd_ctx_target_window(cmdq);
	bool		 toggle;

	if (view == NULL || view->closed) {
		cmdq_error(cmdq, "window-expand: no target window");
		return (CMD_RETURN_ERROR);
	}

	enum expand_dir dir = resolve_direction(args_get(args, 'd'));
	if (dir == EXPAND_ERROR) {
		cmdq_error(cmdq, "window-expand: invalid direction");
		return (CMD_RETURN_ERROR);
	}

	toggle = args_has(args, 'T');

	do_expand(view, dir, toggle);

	cow_wm.needs_manage_dirty = true;
	view->dec.dirty		  = true;

	return (CMD_RETURN_NORMAL);
}

struct cmd_entry cmd_window_maximize_entry = {
	"window-maximize",
	"t:Tmu",
	0,
	0,
	"window-maximize [-t target] [-T toggle] [-m maximize] [-u unmaximize]",
	cmd_window_maximize_exec,
};

static enum cmd_retval
cmd_window_maximize_exec(struct cmd *self, struct cmd_q *cmdq)
{
	struct args	*args = self->args;
	struct cow_view *view = cmd_ctx_target_window(cmdq);

	if (view == NULL || view->closed) {
		cmdq_error(cmdq, "window-maximize: no target window");
		return (CMD_RETURN_ERROR);
	}

	if (args_has(args, 'T'))
		do_maximize(view, MAX_TOGGLE);
	else if (args_has(args, 'u'))
		do_maximize(view, UNMAXIMIZE);
	else
		do_maximize(view, MAXIMIZE);

	cow_wm.needs_manage_dirty = true;
	view->dec.dirty		  = true;

	return (CMD_RETURN_NORMAL);
}

struct cmd_entry cmd_window_shade_entry = {
	"window-shade",
	"t:d:",
	0,
	0,
	"window-shade [-t target] [-d direction]",
	cmd_window_shade_exec,
};

static enum cmd_retval
cmd_window_shade_exec(struct cmd *self, struct cmd_q *cmdq)
{
	struct args	*args = self->args;
	struct cow_view *view = cmd_ctx_target_window(cmdq);

	if (view == NULL || view->closed) {
		cmdq_error(cmdq, "window-maximize: no target window");
		return (CMD_RETURN_ERROR);
	}

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
	view->needs_raise	  = true;
	cow_wm.needs_manage_dirty = true;
}

static void
do_lower(struct cow_view *view)
{
	view->pending_lower	  = true;
	cow_wm.needs_manage_dirty = true;
}

static void
do_maximize(struct cow_view *view, enum maximize_type mt)
{
	if (view->output == NULL)
		return;

	if ((mt == UNMAXIMIZE || mt == MAX_TOGGLE) && view->maximized) {
		/* saved_x/y are virtual (vx/vy) coordinates — convert to
		 * global for pending_resize, which will re-sync vx/vy. */
		int gx = view->saved_x, gy = view->saved_y;
		if (view->output != NULL && view->desk != NULL) {
			gx = view->output->x +
			    (view->saved_x - view->desk->page_vx);
			gy = view->output->y +
			    (view->saved_y - view->desk->page_vy);
		}
		cow_view_set_pending_geometry(view, gx, gy, view->saved_width,
		    view->saved_height);
		view->maximized		   = false;
		view->icon_inverted_button = -1;
		return;
	}

	struct cow_box *ua = &view->output->usable_area;
	int		bw = view->dec.border_width;
	int		tt = view->dec.title_thickness;

	cow_view_save_geometry(view);

	if (mt != MAXIMIZE_KEEP) {
		int w = ua->width - 2 * bw;
		int h = ua->height - 2 * bw - tt;
		if (w < 50)
			w = 50;
		if (h < 50)
			h = 50;

		cow_view_set_pending_geometry(view, ua->x + bw, ua->y + bw + tt,
		    w, h);
	}
	view->maximized = true;

	int mbi = cow_find_maximize_button();
	if (mbi >= 0)
		view->icon_inverted_button = mbi;
}

static enum expand_dir
resolve_direction(const char *dir_str)
{
	enum expand_dir dir = EXPAND_ALL;

	if (dir_str == NULL)
		return (dir);

	if (strcmp(dir_str, "left") == 0)
		dir = EXPAND_LEFT;
	else if (strcmp(dir_str, "right") == 0)
		dir = EXPAND_RIGHT;
	else if (strcmp(dir_str, "up") == 0)
		dir = EXPAND_UP;
	else if (strcmp(dir_str, "down") == 0)
		dir = EXPAND_DOWN;
	else
		dir = EXPAND_ERROR;
	return (dir);
}

static enum shade_direction
resolve_shade_direction(const char *dir_str)
{
	int dir = COW_SHADE_NORTH;

	if (dir_str == NULL)
		return (dir);

	if (strcmp(dir_str, "north") == 0)
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
	if (toggle && view->maximized) {
		cow_view_set_pending_geometry(view, view->saved_x,
		    view->saved_y, view->saved_width, view->saved_height);
		view->maximized		   = false;
		view->icon_inverted_button = -1;
		return;
	}

	struct cow_box *ua = &view->output->usable_area;
	int		bw = view->dec.border_width;
	int		tt = view->dec.title_thickness;

	if (!view->maximized)
		cow_view_save_geometry(view);

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

	struct cow_view *other;
	wl_list_for_each(other, &cow_wm.views, wm_link)
	{
		if (other == view || other->closed || !other->has_dimensions)
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

	int new_x = grow_left + bw;
	int new_y = grow_top + bw + tt;
	int new_w = (grow_right - grow_left) - 2 * bw;
	int new_h = (grow_bottom - grow_top) - 2 * bw - tt;

	if (new_w < 50)
		new_w = 50;
	if (new_h < 50)
		new_h = 50;

	cow_view_set_pending_geometry(view, new_x, new_y, new_w, new_h);
	view->maximized = true;

	int mbi = cow_find_maximize_button();
	if (mbi >= 0)
		view->icon_inverted_button = mbi;
}

static void
do_shade(struct cow_view *view, const char *dir_str)
{
	enum shade_direction sd = resolve_shade_direction(dir_str);

	cow_view_shade(view, sd);
	cow_wm.needs_manage_dirty = true;
}

void
do_identify(struct cow_view *view, struct cmd_q *cmdq)
{
	int    fd   = cmdq->client_fd;
	cJSON *info = cJSON_CreateObject();

	/* Identity */
	cJSON_AddStringToObject(info, "id",
	    view->identifier ? view->identifier : "");
	cJSON_AddStringToObject(info, "app_id",
	    view->app_id ? view->app_id : "");
	cJSON_AddStringToObject(info, "title", view->title ? view->title : "");

	/* Geometry */
	cJSON *geo = cJSON_CreateObject();
	cJSON_AddNumberToObject(geo, "x", view->x);
	cJSON_AddNumberToObject(geo, "y", view->y);
	cJSON_AddNumberToObject(geo, "width", view->content_width);
	cJSON_AddNumberToObject(geo, "height", view->content_height);
	cJSON_AddItemToObject(info, "geometry", geo);

	/* Frame */
	struct cow_box fb    = cow_view_frame_box(view);
	cJSON	      *frame = cJSON_CreateObject();
	cJSON_AddNumberToObject(frame, "x", fb.x);
	cJSON_AddNumberToObject(frame, "y", fb.y);
	cJSON_AddNumberToObject(frame, "width", fb.width);
	cJSON_AddNumberToObject(frame, "height", fb.height);
	cJSON_AddItemToObject(info, "frame", frame);

	/* Decoration */
	cJSON *dec = cJSON_CreateObject();
	cJSON_AddNumberToObject(dec, "border_width", view->dec.border_width);
	cJSON_AddNumberToObject(dec, "title_thickness",
	    view->dec.title_thickness);
	cJSON_AddBoolToObject(dec, "ssd", view->use_ssd);
	cJSON_AddItemToObject(info, "decoration", dec);

	/* Desk/output */
	cJSON_AddNumberToObject(info, "desk", view->desk ? view->desk->nr : -1);
	if (view->output != NULL && view->output->name != NULL)
		cJSON_AddStringToObject(info, "output", view->output->name);

	/* Flags */
	cJSON *flags = cJSON_CreateObject();
	cJSON_AddBoolToObject(flags, "sticky",
	    (view->flags & COW_VIEW_STICKY_DESK_FLAG) != 0);
	cJSON_AddBoolToObject(flags, "page_sticky",
	    (view->flags & COW_VIEW_STICKY_PAGE_FLAG) != 0);
	cJSON_AddBoolToObject(flags, "iconified",
	    (view->flags & COW_VIEW_ICONIFIED_FLAG) != 0);
	cJSON_AddBoolToObject(flags, "maximized", view->maximized);
	cJSON_AddBoolToObject(flags, "shaded", view->sd != COW_SHADE_NONE);
	cJSON_AddBoolToObject(flags, "fullscreen", view->fullscreen);
	cJSON_AddBoolToObject(flags, "focused", view == cow_wm.focused_view);
	cJSON_AddItemToObject(info, "flags", flags);

	/* Size hints */
	if (view->min_width > 0 || view->min_height > 0 ||
	    view->max_width > 0 || view->max_height > 0) {
		cJSON *hints = cJSON_CreateObject();
		cJSON_AddNumberToObject(hints, "min_width", view->min_width);
		cJSON_AddNumberToObject(hints, "min_height", view->min_height);
		cJSON_AddNumberToObject(hints, "max_width", view->max_width);
		cJSON_AddNumberToObject(hints, "max_height", view->max_height);
		cJSON_AddItemToObject(info, "size_hints", hints);
	}

	/* Style */
	if (view->style.resolved) {
		cJSON *style = cJSON_CreateObject();
		cJSON_AddNumberToObject(style, "border", view->style.border);
		cJSON_AddBoolToObject(style, "titlebar_enabled",
		    view->style.titlebar_enabled);
		cJSON_AddItemToObject(info, "style", style);
	}

	cow_ipc_respond_data(fd, info);
}
