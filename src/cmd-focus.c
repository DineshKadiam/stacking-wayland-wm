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
#include <stdlib.h>
#include <string.h>

#include "cow/cmd.h"
#include "cow/compat.h"
#include "cow/config.h"
#include "cow/memory.h"
#include "cow/output.h"
#include "cow/seat.h"
#include "cow/view.h"
#include "cow/wm.h"

static enum cmd_retval cmd_focus_exec(struct cmd *, struct cmd_q *);
static void	       do_next(int target_layer);
static void	       do_prev(int target_layer);
static void	       do_app_id(const char *, struct cmd_q *);
static void	       do_title(const char *, struct cmd_q *);
static void	       do_id(const char *, struct cmd_q *);
static void	       do_directional(enum cow_direction, int target_layer,
	       struct cmd_q *);
static void	       do_screen(const char *, struct cmd_q *);
static void	       do_layer_focus(int layer, struct cmd_q *);

/*
 * File-static layer filter state.  When >= 0, the predicate
 * focusable_layered() also requires effective_layer == target.
 * Set immediately before invoking cow_view_walk() and cleared on
 * function return; not multi-thread safe (the WM is single-threaded).
 */
static int g_layer_filter = -1;

static bool
focusable_layered(const struct cow_view *v)
{
	if (!cow_view_focusable(v))
		return false;
	if (g_layer_filter < 0)
		return true;
	return (int)cow_view_effective_layer(v) == g_layer_filter;
}

struct cmd_entry cmd_focus_entry = {
	"focus",
	"t:npwd:o:Ll:",
	0,
	0,
	"focus [-npwL] [-l N] [-d dir] [-o output] "
	"[-t %app|/title/|#id]",
	cmd_focus_exec,
	0,
};

/*
 * Resolve a window selector of the form:
 *   %app_id   exact match on app_id
 *   /title/   substring match on title (no trailing '/' required)
 *   #id       exact match on identifier
 *
 * Calls one of the do_app_id/do_title/do_id helpers and returns true
 * if a recognised prefix was found.  Emits an error and returns false
 * if the selector is malformed.
 */
static bool
focus_dispatch_selector(const char *spec, struct cmd_q *cmdq)
{
	if (spec == NULL || spec[0] == '\0') {
		cmdq_error(cmdq, "focus: -t: empty selector");
		return false;
	}
	switch (spec[0]) {
	case '%':
		do_app_id(spec + 1, cmdq);
		return true;
	case '/': {
		/* Strip an optional trailing '/' for symmetry. */
		size_t len = strlen(spec + 1);
		char  *pat = xstrdup(spec + 1);
		if (len > 0 && pat[len - 1] == '/')
			pat[len - 1] = '\0';
		do_title(pat, cmdq);
		xfree(pat);
		return true;
	}
	case '#':
		do_id(spec + 1, cmdq);
		return true;
	default:
		cmdq_error(cmdq,
		    "focus: -t: selector must start with %%, /, or #: %s",
		    spec);
		return false;
	}
}

static enum cmd_retval
cmd_focus_exec(struct cmd *self, struct cmd_q *cmdq)
{
	struct args *args = self->args;

	const char *dir_val    = args_get(args, 'd');
	const char *scr_val    = args_get(args, 'o');
	const char *t_val      = args_get(args, 't');
	const char *layer_val  = args_get(args, 'l');
	bool	    do_warp    = args_has(args, 'w');
	bool	    same_layer = args_has(args, 'L');

	if (args->argc > 0) {
		cmdq_error(cmdq, "focus: unexpected argument: %s",
		    args->argv[0]);
		return (CMD_RETURN_ERROR);
	}

	if (same_layer && layer_val != NULL) {
		cmdq_error(cmdq, "focus: -L and -l are mutually exclusive");
		return (CMD_RETURN_ERROR);
	}

	/*
	 * Resolve target_layer for the layer-aware modifiers:
	 *   -L      -> effective layer of the focused view (default layer
	 *              when there is none).
	 *   -l N    -> N (explicit).
	 *  neither  -> -1 = "any layer" (back-compat).
	 */
	int target_layer = -1;
	if (same_layer) {
		struct cow_view *f = cow_wm.focused_view;
		if (f != NULL && !f->closed)
			target_layer = (int)cow_view_effective_layer(f);
		else
			target_layer = (cow_config != NULL) ?
			    (int)cow_config->default_layer :
			    COW_CFG_DEFAULT_LAYER;
	} else if (layer_val != NULL) {
		const char *errstr;
		long long   v = strtonum(layer_val, 0, 255, &errstr);
		if (errstr != NULL) {
			cmdq_error(cmdq, "focus: -l: bad layer value: %s",
			    layer_val);
			return (CMD_RETURN_ERROR);
		}
		target_layer = (int)v;
	}

	if (args_has(args, 'n')) {
		do_next(target_layer);
	} else if (args_has(args, 'p')) {
		do_prev(target_layer);
	} else if (scr_val != NULL) {
		do_screen(scr_val, cmdq);
		return (CMD_RETURN_NORMAL);
	} else if (dir_val != NULL) {
		enum cow_direction dir = cow_parse_direction(dir_val);
		if (dir == COW_DIR_NONE) {
			cmdq_error(cmdq, "focus: unknown direction: %s",
			    dir_val);
			return (CMD_RETURN_ERROR);
		}
		do_directional(dir, target_layer, cmdq);
	} else if (t_val != NULL) {
		if (!focus_dispatch_selector(t_val, cmdq))
			return (CMD_RETURN_ERROR);
	} else if (layer_val != NULL) {
		/* `focus -l N` with no -n/-p: topmost (or cycle if
		 * focused view is already in N). */
		do_layer_focus(target_layer, cmdq);
	} else if (same_layer) {
		/* -L is a modifier; not a standalone verb. */
		cmdq_error(cmdq,
		    "focus: -L requires -n, -p, or -d <direction>");
		return (CMD_RETURN_ERROR);
	}

	if (do_warp) {
		cow_view_warp_to_focused();
	}

	return (CMD_RETURN_NORMAL);
}

static void
defer_focus(struct cow_view *view)
{
	cow_wm.pending_focus = view;
	cow_wm_request_manage_dirty();
}

/*
 * Find the first focusable view relative to 'focused' in the given
 * direction.  If focused is NULL, search from the start/end of the
 * list.  Returns NULL if none is found.
 *
 * target_layer >= 0 restricts the search to views whose effective
 * layer equals target_layer; -1 means "any layer" (back-compat).
 */
static struct cow_view *
find_focusable(struct cow_view *focused, bool forward, int target_layer)
{
	if (wl_list_empty(&cow_wm.views))
		return NULL;

	g_layer_filter = target_layer;

	struct cow_view *result = NULL;
	if (focused == NULL) {
		struct cow_view *v;
		if (forward) {
			wl_list_for_each(v, &cow_wm.views, wm_link)
			{
				if (focusable_layered(v)) {
					result = v;
					break;
				}
			}
		} else {
			wl_list_for_each_reverse(v, &cow_wm.views, wm_link)
			{
				if (focusable_layered(v)) {
					result = v;
					break;
				}
			}
		}
	} else {
		result = cow_view_walk(focused, forward, focusable_layered);
	}

	g_layer_filter = -1;
	return result;
}

/* focus -n [-L | -l N] */
static void
do_next(int target_layer)
{
	struct cow_view *focused = cow_wm.focused_view;
	struct cow_view *next	 = find_focusable(focused, true, target_layer);

	if (next != NULL && next != focused)
		defer_focus(next);
}

/* focus -p [-L | -l N] */
static void
do_prev(int target_layer)
{
	struct cow_view *focused = cow_wm.focused_view;
	struct cow_view *prev	 = find_focusable(focused, false, target_layer);

	if (prev != NULL && prev != focused)
		defer_focus(prev);
}

/*
 * focus -l N (no -n/-p): focus the most-recently-raised view in
 * layer N.  If the focused view is already in layer N, cycle to the
 * next layer-N view (in insertion order); when no other view exists
 * in N, no-op.
 */
static void
do_layer_focus(int layer, struct cmd_q *cmdq)
{
	(void)cmdq; /* no-op when layer empty; quiet rather than error */
	struct cow_view *focused = cow_wm.focused_view;

	if (focused != NULL && !focused->closed &&
	    (int)cow_view_effective_layer(focused) == layer) {
		/* Cycle: walk forward, stop at the next focusable in N. */
		struct cow_view *next = find_focusable(focused, true, layer);
		if (next != NULL && next != focused)
			defer_focus(next);
		return;
	}

	/* Otherwise pick the topmost (largest raise_seq) view in N. */
	struct cow_view *best = NULL;
	struct cow_view *v;
	wl_list_for_each(v, &cow_wm.views, wm_link)
	{
		if (!cow_view_focusable(v))
			continue;
		if ((int)cow_view_effective_layer(v) != layer)
			continue;
		if (best == NULL || v->raise_seq > best->raise_seq)
			best = v;
	}
	if (best != NULL)
		defer_focus(best);
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
	struct cow_view *v = cow_view_find_by_identifier(id);

	if (v != NULL) {
		defer_focus(v);
		return;
	}
	cmdq_error(cmdq, "focus: no window with id: %s", id);
}

static void
do_directional(enum cow_direction dir, int target_layer, struct cmd_q *cmdq)
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
		if (target_layer >= 0 &&
		    (int)cow_view_effective_layer(v) != target_layer)
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
		case COW_DIR_UP:
			in_direction = (vy < fy);
			primary_dist = fy - vy;
			overlaps     = (v_right > f_left && v_left < f_right);
			break;
		case COW_DIR_DOWN:
			in_direction = (vy > fy);
			primary_dist = vy - fy;
			overlaps     = (v_right > f_left && v_left < f_right);
			break;
		case COW_DIR_LEFT:
			in_direction = (vx < fx);
			primary_dist = fx - vx;
			overlaps     = (v_bot > f_top && v_top < f_bot);
			break;
		case COW_DIR_RIGHT:
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

	cow_wm_request_pointer_warp(target->x + (int)(rx * target->width),
	    target->y + (int)(ry * target->height));
	cow_wm_request_manage_dirty();
}
