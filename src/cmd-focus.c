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
 * Command: focus -- focus a window by various selectors.
t*
 * usage: focus [-npw] [-d up|down|left|right] [-o output]
 *              [-A app_id] [-T title] [-i id]
 */

#include <limits.h>
#include <string.h>

#include "cow/cmd.h"
#include "cow/output.h"
#include "cow/seat.h"
#include "cow/view.h"
#include "cow/wm.h"

static enum cmd_retval cmd_focus_exec(struct cmd *, struct cmd_q *);
static void	       do_next(void);
static void	       do_prev(void);
static void	       do_app_id(const char *, struct cmd_q *);
static void	       do_title(const char *, struct cmd_q *);
static void	       do_id(const char *, struct cmd_q *);
static void	       do_directional(int, struct cmd_q *);
static void	       do_screen(const char *, struct cmd_q *);
static int	       parse_direction(const char *);

struct cmd_entry cmd_focus_entry = { "focus", "t:npwd:o:A:T:i:", 0, 0,
	"focus [-npw] [-d dir] [-o output] [-A app_id] [-T title] [-i id]",
	cmd_focus_exec };

static enum cmd_retval
cmd_focus_exec(struct cmd *self, struct cmd_q *cmdq)
{
	struct args *args = self->args;

	const char *dir_val = args_get(args, 'd');
	const char *scr_val = args_get(args, 'o');
	const char *app_val = args_get(args, 'A');
	const char *ttl_val = args_get(args, 'T');
	const char *id_val  = args_get(args, 'i');
	bool	    do_warp = args_has(args, 'w');

	if (args->argc > 0) {
		cmdq_error(cmdq, "focus: unexpected argument: %s",
		    args->argv[0]);
		return (CMD_RETURN_ERROR);
	}

	if (args_has(args, 'n')) {
		do_next();
	} else if (args_has(args, 'p')) {
		do_prev();
	} else if (scr_val != NULL) {
		do_screen(scr_val, cmdq);
		return (CMD_RETURN_NORMAL);
	} else if (dir_val != NULL) {
		int dir = parse_direction(dir_val);
		if (dir < 0) {
			cmdq_error(cmdq, "focus: unknown direction: %s",
			    dir_val);
			return (CMD_RETURN_ERROR);
		}
		do_directional(dir, cmdq);
	} else if (app_val != NULL) {
		do_app_id(app_val, cmdq);
	} else if (ttl_val != NULL) {
		do_title(ttl_val, cmdq);
	} else if (id_val != NULL) {
		do_id(id_val, cmdq);
	}

	if (do_warp) {
		cow_view_warp_to_focused();
	}

	return (CMD_RETURN_NORMAL);
}

static void
defer_focus(struct cow_view *view)
{
	cow_wm.pending_focus	  = view;
	cow_wm.needs_manage_dirty = true;
}

/* focus -n */
static void
do_next(void)
{
	struct cow_view *focused = cow_wm.focused_view;

	if (wl_list_empty(&cow_wm.views))
		return;

	struct cow_view *next = NULL;
	if (focused == NULL) {
		wl_list_for_each(next, &cow_wm.views, wm_link)
		{
			if (cow_view_focusable(next))
				break;
		}
	} else {
		struct wl_list *start = focused->wm_link.next;
		struct wl_list *pos   = start;
		do {
			if (pos == &cow_wm.views) {
				pos = pos->next;
				continue;
			}
			next = wl_container_of(pos, next, wm_link);
			if (cow_view_focusable(next))
				break;
			next = NULL;
			pos  = pos->next;
		} while (pos != start);
	}

	if (next != NULL && next != focused)
		defer_focus(next);
}

/* focus -p */
static void
do_prev(void)
{
	struct cow_view *focused = cow_wm.focused_view;

	if (wl_list_empty(&cow_wm.views))
		return;

	struct cow_view *prev = NULL;
	if (focused == NULL) {
		struct cow_view *v;
		wl_list_for_each_reverse(v, &cow_wm.views, wm_link)
		{
			if (cow_view_focusable(v)) {
				prev = v;
				break;
			}
		}
	} else {
		struct wl_list *start = focused->wm_link.prev;
		struct wl_list *pos   = start;
		do {
			if (pos == &cow_wm.views) {
				pos = pos->prev;
				continue;
			}
			prev = wl_container_of(pos, prev, wm_link);
			if (cow_view_focusable(prev))
				break;
			prev = NULL;
			pos  = pos->prev;
		} while (pos != start);
	}

	if (prev != NULL && prev != focused)
		defer_focus(prev);
}

/* focus -A app_id */
static void
do_app_id(const char *pattern, struct cmd_q *cmdq)
{
	struct cow_view *v;
	wl_list_for_each(v, &cow_wm.views, wm_link)
	{
		if (cow_view_focusable(v) && v->app_id != NULL &&
		    strcmp(v->app_id, pattern) == 0) {
			defer_focus(v);
			return;
		}
	}
	cmdq_error(cmdq, "focus: no window with app_id: %s", pattern);
}

/* focus -T title */
static void
do_title(const char *pattern, struct cmd_q *cmdq)
{
	struct cow_view *v;

	wl_list_for_each(v, &cow_wm.views, wm_link)
	{
		if (cow_view_focusable(v) && v->title != NULL &&
		    strstr(v->title, pattern) != NULL) {
			defer_focus(v);
			return;
		}
	}
	cmdq_error(cmdq, "focus: no window with title matching: %s", pattern);
}

/* focus -i id */
static void
do_id(const char *id, struct cmd_q *cmdq)
{
	struct cow_view *v;

	wl_list_for_each(v, &cow_wm.views, wm_link)
	{
		if (cow_view_focusable(v) && v->identifier != NULL &&
		    strcmp(v->identifier, id) == 0) {
			defer_focus(v);
			return;
		}
	}
	cmdq_error(cmdq, "focus: no window with id: %s", id);
}

enum { DIR_UP = 0, DIR_DOWN = 1, DIR_LEFT = 2, DIR_RIGHT = 3 };

static int
parse_direction(const char *s)
{
	if (strcmp(s, "up") == 0)
		return DIR_UP;
	if (strcmp(s, "down") == 0)
		return DIR_DOWN;
	if (strcmp(s, "left") == 0)
		return DIR_LEFT;
	if (strcmp(s, "right") == 0)
		return DIR_RIGHT;
	return -1;
}

static void
do_directional(int dir, struct cmd_q *cmdq)
{
	struct cow_view *focused = cow_wm.focused_view;

	if (focused == NULL || focused->output == NULL) {
		cmdq_error(cmdq, "focus: no focused window");
		return;
	}

	int fx, fy;
	cow_view_centre(focused, &fx, &fy);

	struct cow_view *best	       = NULL;
	int		 best_dist     = INT_MAX;
	bool		 best_overlaps = false;

	struct cow_view *v;
	wl_list_for_each(v, &cow_wm.views, wm_link)
	{
		if (v == focused || !cow_view_focusable(v))
			continue;
		if (v->output != focused->output)
			continue;

		int vx, vy;
		cow_view_centre(v, &vx, &vy);

		int f_top = focused->y - focused->dec.title_thickness -
		    focused->dec.border_width;
		int f_bot = focused->y + focused->content_height +
		    focused->dec.border_width;
		int f_left  = focused->x - focused->dec.border_width;
		int f_right = focused->x + focused->content_width +
		    focused->dec.border_width;

		int v_top = v->y - v->dec.title_thickness - v->dec.border_width;
		int v_bot = v->y + v->content_height + v->dec.border_width;
		int v_left  = v->x - v->dec.border_width;
		int v_right = v->x + v->content_width + v->dec.border_width;

		int  primary_dist;
		bool in_direction, overlaps;

		switch (dir) {
		case DIR_UP:
			in_direction = (vy < fy);
			primary_dist = fy - vy;
			overlaps     = (v_right > f_left && v_left < f_right);
			break;
		case DIR_DOWN:
			in_direction = (vy > fy);
			primary_dist = vy - fy;
			overlaps     = (v_right > f_left && v_left < f_right);
			break;
		case DIR_LEFT:
			in_direction = (vx < fx);
			primary_dist = fx - vx;
			overlaps     = (v_bot > f_top && v_top < f_bot);
			break;
		case DIR_RIGHT:
			in_direction = (vx > fx);
			primary_dist = vx - fx;
			overlaps     = (v_bot > f_top && v_top < f_bot);
			break;
		default:
			in_direction = false;
			primary_dist = INT_MAX;
			overlaps     = false;
			break;
		}

		if (!in_direction)
			continue;
		if (overlaps && !best_overlaps) {
			best	      = v;
			best_dist     = primary_dist;
			best_overlaps = true;
		} else if (overlaps == best_overlaps &&
		    primary_dist < best_dist) {
			best	  = v;
			best_dist = primary_dist;
		}
	}

	if (best != NULL)
		defer_focus(best);
}

/* focus -o output (warp pointer to output) */
static void
do_screen(const char *name, struct cmd_q *cmdq)
{
	struct cow_output *src = cow_wm.current_output;

	if (src == NULL) {
		cmdq_error(cmdq, "focus: no current output");
		return;
	}

	struct cow_output *target = cow_output_find_by_name(name);
	if (target == NULL) {
		cmdq_error(cmdq, "focus: unknown output: %s", name);
		return;
	}
	if (target == src)
		return;

	int px = src->x + src->width / 2;
	int py = src->y + src->height / 2;
	cow_seat_get_pointer(&px, &py);

	double rx = 0.5, ry = 0.5;
	if (src->width > 0)
		rx = (double)(px - src->x) / src->width;
	if (src->height > 0)
		ry = (double)(py - src->y) / src->height;
	if (rx < 0.0)
		rx = 0.0;
	if (rx > 1.0)
		rx = 1.0;
	if (ry < 0.0)
		ry = 0.0;
	if (ry > 1.0)
		ry = 1.0;

	cow_wm.pending_warp	  = true;
	cow_wm.warp_x		  = target->x + (int)(rx * target->width);
	cow_wm.warp_y		  = target->y + (int)(ry * target->height);
	cow_wm.needs_manage_dirty = true;
}
