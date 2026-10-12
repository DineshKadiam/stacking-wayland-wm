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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cow/compat.h"
#include "cow/cmd.h"
#include "cow/config.h"
#include "cow/desk_ops.h"
#include "cow/frame.h"
#include "cow/keybinding.h"
#include "cow/menu_choice.h"
#include "cow/output.h"
#include "cow/placement.h"
#include "cow/pointer_op.h"
#include "cow/seat.h"
#include "cow/selection.h"
#include "cow/style.h"
#include "cow/titlebar.h"
#include "cow/view.h"
#include "cow/update.h"
#include "cow/util.h"
#include "cow/wm.h"

#define SELECTION_MOVE_MENU "\037cow-selection-move"

static enum cmd_retval cmd_move_exec(struct cmd *, struct cmd_q *);
static enum cmd_retval move_group(struct cow_view *, const char *,
    struct cmd_q *);
static enum cmd_retval choose_move_group(struct cow_view *, struct cmd_q *);
static void do_screen(struct cow_view *, const char *, struct cmd_q *);
static void do_interactive(struct cow_view *);
static void do_interactive_group(struct cow_view *, const char *);
static void do_step(struct cow_view *, const char *, struct cmd_q *);
static void do_snap(struct cow_view *, const char *, struct cmd_q *);
static void do_xy(struct cow_view *, const char *, const char *,
    struct cmd_q *);
static bool do_anchor(struct cow_view *, const char *, struct cmd_q *);
static void do_desk(struct cow_view *, uint8_t, struct cmd_q *);
static void do_transfer_to_output(struct cow_view *, struct cow_output *,
    const char *);
static void finish_move(struct cow_view *, bool);
static struct cow_output *output_neighbour_in_dir(struct cow_output *,
    const char *);
static void move_content_size(const struct cow_view *, int *, int *);
static void move_frame_geometry(const struct cow_view *, int *, int *, int *,
    int *);
static struct cow_box move_frame_box(const struct cow_view *);
static void sync_pending_move_position(struct cow_view *);

struct cmd_entry cmd_move_entry = {
	"window-move",
	"t:d:o:a:cCGnwk:x:y:P:",
	0,
	1,
	"[-C] [-w] [-t target] [-d dir [-n]] [-o output] [-a anchor] [-c] "
	"[-k 0-9] [-x X] [-y Y] [-P col[,row]] [-G [selection]]",
	cmd_move_exec,
	0,
};

static enum cmd_retval
cmd_move_exec(struct cmd *self, struct cmd_q *cmdq)
{
	struct args	*args = self->args;
	struct cow_view *view = cmd_ctx_target_window(cmdq);
	const char	*group_name = args->argc == 1 ? args->argv[0] : NULL;

	if (cow_container_is_attached_client(view)) {
		cmdq_error(cmdq, "move: window belongs to a module panel");
		return CMD_RETURN_ERROR;
	}

	if (args_has(args, 'G')) {
		if (args->argc > 1 || args_get(args, 'd') != NULL ||
		    args_get(args, 'o') != NULL || args_get(args, 'a') != NULL ||
		    args_has(args, 'c') || args_has(args, 'C') ||
		    args_has(args, 'n') || args_has(args, 'w') ||
		    args_get(args, 'k') != NULL || args_get(args, 'x') != NULL ||
		    args_get(args, 'y') != NULL || args_get(args, 'P') != NULL) {
			cmdq_error(cmdq, "window-move: -G cannot be combined with "
			    "another move operation");
			return CMD_RETURN_ERROR;
		}
		if (group_name != NULL && group_name[0] == '\0') {
			cmdq_error(cmdq,
			    "window-move: selection name cannot be empty");
			return CMD_RETURN_ERROR;
		}
		if (group_name == NULL)
			return choose_move_group(view, cmdq);
		return move_group(view, group_name, cmdq);
	}

	if (view == NULL || view->output == NULL) {
		cmdq_error(cmdq, "move: no target window");
		return (CMD_RETURN_ERROR);
	}
	if (view->closed || !view->has_dimensions)
		return (CMD_RETURN_NORMAL);
	if (cmdq == NULL || !cmdq->in_style)
		cow_style_cancel_pending_initial(view);

	const char *dir_val = args_get(args, 'd');
	const char *scr_val = args_get(args, 'o');
	const char *anchor = args_get(args, 'a');
	bool warp = args_has(args, 'w');
	if (anchor != NULL && (dir_val != NULL || scr_val != NULL ||
	    args_has(args, 'c') || args_has(args, 'C') ||
	    args_has(args, 'k') || args_has(args, 'x') ||
	    args_has(args, 'y') || args_has(args, 'P'))) {
		cmdq_error(cmdq, "window-move: -a cannot be combined with "
		    "another move operation");
		return (CMD_RETURN_ERROR);
	}
	if (args->argc != 0 && args_get(args, 'P') == NULL) {
		cmdq_error(cmdq, "usage: %s %s", self->entry->name,
		    self->entry->usage);
		return (CMD_RETURN_ERROR);
	}

	if (scr_val != NULL) {
		do_screen(view, scr_val, cmdq);
		/*
		 * Fall through so a same-command -k can land the view on
		 * a specific desk of the new output.  Other modifiers
		 * (-d, -x/-y, -c, -P) are not meaningful when combined
		 * with -o and are rejected below.
		 */
		if (dir_val != NULL || args_has(args, 'c') ||
		    args_has(args, 'x') || args_has(args, 'y') ||
		    args_get(args, 'P') != NULL) {
			cmdq_error(cmdq, "move: -o only combines with -k");
			return (CMD_RETURN_ERROR);
		}
		if (args_get(args, 'k') == NULL) {
			finish_move(view, warp);
			return (CMD_RETURN_NORMAL);
		}
	}

	if (dir_val != NULL) {
		if (args_has(args, 'n'))
			do_snap(view, dir_val, cmdq);
		else
			do_step(view, dir_val, cmdq);
		finish_move(view, warp);
		return (CMD_RETURN_NORMAL);
	}

	if (anchor != NULL) {
		if (!do_anchor(view, anchor, cmdq))
			return (CMD_RETURN_ERROR);
		finish_move(view, warp);
		return (CMD_RETURN_NORMAL);
	}

	if (args_has(args, 'c')) {
		(void)do_anchor(view, "centre", cmdq);
		finish_move(view, warp);
		return (CMD_RETURN_NORMAL);
	}

	char *desk_no;
	if ((desk_no = args_get(args, 'k')) != NULL) {
		struct cow_output *out = view ? view->output : NULL;
		int		   max = cow_desk_count(out) - 1;
		if (max < 0)
			max = 0;
		long long nr;
		if (!cmd_parse_int(cmdq, "move-window", "desk", desk_no, 0,
			max, &nr))
			return (CMD_RETURN_ERROR);
		do_desk(view, (uint8_t)nr, cmdq);
		finish_move(view, warp);
		return (CMD_RETURN_NORMAL);
	}

	char *xp    = NULL;
	char *yp    = NULL;
	bool  xflag = args_has(args, 'x');
	bool  yflag = args_has(args, 'y');

	if (xflag)
		xp = args_get(args, 'x');
	if (yflag)
		yp = args_get(args, 'y');

	if (xflag || yflag) {
		do_xy(view, xp, yp, cmdq);
		finish_move(view, warp);
		return (CMD_RETURN_NORMAL);
	}

	if (args_get(args, 'P') != NULL) {
		const char *page_s = args_get(args, 'P');
		int	    col = 0, row = 0;
		int	    cols = cow_config ? cow_config->page_cols :
						COW_CFG_DEFAULT_PAGE_COLS;
		int	    rows = cow_config ? cow_config->page_rows :
						COW_CFG_DEFAULT_PAGE_ROWS;
		long long   lc, lr;
		if (strchr(page_s, ',') != NULL) {
			if (args->argc != 0) {
				cmdq_error(cmdq,
				    "window-move: -P col,row takes no row argument");
				return (CMD_RETURN_ERROR);
			}
			if (!cmd_parse_page_spec(cmdq, "window-move", page_s,
				cow_view_page_col(view), cow_view_page_row(view),
				cols, rows, &lc, &lr))
				return (CMD_RETURN_ERROR);
			row = (int)lr;
		} else {
			if (!cmd_parse_page_axis(cmdq, "window-move", "col",
				page_s, cow_view_page_col(view), cols, &lc))
				return (CMD_RETURN_ERROR);
			if (args->argc >= 1) {
				if (!cmd_parse_page_axis(cmdq, "window-move", "row",
					args->argv[0], cow_view_page_row(view), rows,
					&lr))
					return (CMD_RETURN_ERROR);
				row = (int)lr;
			}
		}
		col = (int)lc;
		cow_view_move_to_page(view, col, row);
		finish_move(view, warp);
		return (CMD_RETURN_NORMAL);
	}

	/* Interactive move; -C arms container-formation on drop */
	bool container_drag = args_has(args, 'C');
	do_interactive(view);
	if (container_drag) {
		struct cow_seat *seat = cow_seat_primary();
		/*
		 * Only arm container drag if the move actually started: check
		 * op_in_progress rather than just seat != NULL so that IPC
		 * invocations (where do_interactive is a no-op) don't set stale
		 * state.
		 */
		if (seat != NULL && seat->op_in_progress) {
			seat->container_drag	= true;
			cow_wm.drag_drop_source = view;
		}
	}
	return (CMD_RETURN_NORMAL);
}

static enum cmd_retval
move_group(struct cow_view *view, const char *name, struct cmd_q *cmdq)
{
	struct cow_view **selected = NULL;
	size_t count = cow_selection_collect(name, &selected);

	if (!cow_selection_has(view, name))
		view = NULL;
	if (view == NULL || view->closed || !view->has_dimensions ||
	    view->output == NULL || cow_view_is_iconified(view) ||
	    cow_container_is_attached_client(view)) {
		view = NULL;
		for (size_t i = 0; i < count; i++) {
			if (selected[i] != NULL && !selected[i]->closed &&
			    selected[i]->has_dimensions &&
			    selected[i]->output != NULL &&
			    !cow_view_is_iconified(selected[i]) &&
			    !cow_container_is_attached_client(selected[i])) {
				view = selected[i];
				break;
			}
		}
	}
	xfree(selected);
	if (view == NULL) {
		cmdq_error(cmdq, "window-move: selection is empty");
		return CMD_RETURN_ERROR;
	}
	if (cmdq == NULL || !cmdq->in_style)
		cow_style_cancel_pending_initial(view);
	do_interactive_group(view, name);
	return CMD_RETURN_NORMAL;
}

static enum cmd_retval
choose_move_group(struct cow_view *view, struct cmd_q *cmdq)
{
	struct cow_selection_summary *summaries = NULL;
	size_t count = cow_selection_summaries(&summaries);

	if (count == 0) {
		cmdq_error(cmdq, "window-move: no non-empty selections");
		return CMD_RETURN_ERROR;
	}
	struct cow_menu_choice *choices = xcalloc(count, sizeof(*choices));
	for (size_t i = 0; i < count; i++) {
		char *label;
		xasprintf(&label, "%s (%zu window%s)", summaries[i].name,
		    summaries[i].count, summaries[i].count == 1 ? "" : "s");
		const char *argv[] = { "window-move", "-G", summaries[i].name };
		if (!cow_menu_choice_init(&choices[i], label, 3, argv)) {
			xfree(label);
			cow_menu_choices_free(choices, count);
			cow_selection_summaries_free(summaries, count);
			cmdq_error(cmdq,
			    "window-move: cannot build selection menu");
			return CMD_RETURN_ERROR;
		}
		xfree(label);
	}

	struct cow_output *output = view != NULL ? view->output : NULL;
	if (output == NULL)
		output = cmd_ctx_target_output(cmdq);
	if (output == NULL)
		output = cow_wm.current_output;
	enum cow_menu_choice_result result = cow_menu_choose(SELECTION_MOVE_MENU,
	    "Move Selection (type to search)", choices, count, output, view);
	enum cmd_retval retval = CMD_RETURN_NORMAL;
	if (result == COW_MENU_CHOICE_SINGLE)
		retval = move_group(view, summaries[0].name, cmdq);
	else if (result == COW_MENU_CHOICE_ERROR) {
		cmdq_error(cmdq, "window-move: cannot create selection menu");
		retval = CMD_RETURN_ERROR;
	}
	cow_menu_choices_free(choices, count);
	cow_selection_summaries_free(summaries, count);
	return (retval);
}

static void
unmax_if_needed(struct cow_view *view)
{
	cow_view_clear_expanded_state(view);
	if (!view->maximized)
		return;
	/*
	 * window-maximize -k deliberately allows the current geometry to
	 * represent a maximized window.  Keep that state, and its saved restore
	 * geometry, when a later command positions the window.
	 */
	if ((!view->maximized_h || view->maximized_h_keep_geometry) &&
	    (!view->maximized_v || view->maximized_v_keep_geometry))
		return;
	cow_view_set_maximized(view, false);
	view->icon_inverted_button = -1;
	view->dec.dirty		   = true;
}

static void
apply_move(struct cow_view *view)
{
	sync_pending_move_position(view);
	cow_view_update_output(view);
	view->placed = true;
	/* Keep virtual position in sync with global position */
	cow_view_sync_vxy_from_global(view);
	cow_view_sync_container_pos(view);
	cmd_view_dirty(view);
}

static void
finish_move(struct cow_view *view, bool warp)
{
	/*
	 * Notify status consumers after every non-interactive move.  A manage
	 * pass does not itself guarantee a snapshot, particularly when the
	 * window has moved away from the current page.
	 */
	cow_update_request(COW_UPDATE_STATUS);
	if (view->output == NULL || !cow_view_visible(view, view->output))
		return;
	if (view == cow_wm.focused_view && cow_view_focusable(view))
		cow_wm.pending_focus = view;
	if (warp)
		cow_view_warp_to(view);
}

static void
move_content_size(const struct cow_view *view, int *width, int *height)
{
	if (view->sd != COW_SHADE_NONE) {
		cow_view_effective_content_size(view, width, height);
		return;
	}
	*width = view->pending_resize ? view->pending_width :
	    view->content_width;
	*height = view->pending_resize ? view->pending_height :
	    view->content_height;
}

static void
move_frame_geometry(const struct cow_view *view, int *frame_w, int *frame_h,
    int *content_x, int *content_y)
{
	int content_w, content_h;
	move_content_size(view, &content_w, &content_h);
	cow_titlebar_frame_geometry(view, content_w, content_h, frame_w,
	    frame_h, content_x, content_y);
}

static struct cow_box
move_frame_box(const struct cow_view *view)
{
	int frame_w, frame_h, content_x, content_y;

	move_frame_geometry(view, &frame_w, &frame_h, &content_x,
	    &content_y);
	return (struct cow_box) {
		.x	= view->x - content_x,
		.y	= view->y - content_y,
		.width	= frame_w,
		.height = frame_h,
	};
}

static void
sync_pending_move_position(struct cow_view *view)
{
	if (!view->pending_resize)
		return;
	view->pending_x = view->x;
	view->pending_y = view->y;
}

/*
 * Reassign a view to a neighbouring output, placing it at the entry edge
 * of the new output's usable area in the given direction.  The perpendicular
 * coordinate is preserved but clamped so the window stays fully on-screen.
 */
static void
do_transfer_to_output(struct cow_view *view, struct cow_output *target,
    const char *dir)
{
	struct cow_box *ua = &target->usable_area;
	int frame_w, frame_h, content_x, content_y;
	move_frame_geometry(view, &frame_w, &frame_h, &content_x, &content_y);

	/* Place at entry edge; clamp perpendicular coord. */
	if (strcmp(dir, "right") == 0) {
		view->x	 = ua->x + content_x;
		int miny = ua->y + content_y;
		int maxy = ua->y + ua->height - frame_h + content_y;
		if (view->y < miny)
			view->y = miny;
		else if (view->y > maxy)
			view->y = maxy;
	} else if (strcmp(dir, "left") == 0) {
		view->x	 = ua->x + ua->width - frame_w + content_x;
		int miny = ua->y + content_y;
		int maxy = ua->y + ua->height - frame_h + content_y;
		if (view->y < miny)
			view->y = miny;
		else if (view->y > maxy)
			view->y = maxy;
	} else if (strcmp(dir, "down") == 0) {
		view->y	 = ua->y + content_y;
		int minx = ua->x + content_x;
		int maxx = ua->x + ua->width - frame_w + content_x;
		if (view->x < minx)
			view->x = minx;
		else if (view->x > maxx)
			view->x = maxx;
	} else { /* up */
		view->y	 = ua->y + ua->height - frame_h + content_y;
		int minx = ua->x + content_x;
		int maxx = ua->x + ua->width - frame_w + content_x;
		if (view->x < minx)
			view->x = minx;
		else if (view->x > maxx)
			view->x = maxx;
	}

	cow_view_reassign_output(view, target);
	cow_view_clear_orphan(view);
}

/*
 * Return the output that is geometrically adjacent to `cur' in `dir',
 * or NULL if none exists.  Adjacency requires at least 1px of overlap
 * in the perpendicular axis.
 */
static struct cow_output *
output_neighbour_in_dir(struct cow_output *cur, const char *dir)
{
	struct cow_output *best	     = NULL;
	int		   best_dist = INT_MAX;

	struct cow_output *cand;
	wl_list_for_each(cand, &cow_wm.outputs, wm_link)
	{
		if (cand == cur)
			continue;

		int dist, perp_lo, perp_hi, cur_lo, cur_hi;

		if (strcmp(dir, "right") == 0) {
			/* Candidate must be to the right */
			if (cand->x < cur->x + cur->width)
				continue;
			dist	= cand->x - (cur->x + cur->width);
			perp_lo = cand->y;
			perp_hi = cand->y + cand->height;
			cur_lo	= cur->y;
			cur_hi	= cur->y + cur->height;
		} else if (strcmp(dir, "left") == 0) {
			/* Candidate must be to the left */
			if (cand->x + cand->width > cur->x)
				continue;
			dist	= cur->x - (cand->x + cand->width);
			perp_lo = cand->y;
			perp_hi = cand->y + cand->height;
			cur_lo	= cur->y;
			cur_hi	= cur->y + cur->height;
		} else if (strcmp(dir, "down") == 0) {
			/* Candidate must be below */
			if (cand->y < cur->y + cur->height)
				continue;
			dist	= cand->y - (cur->y + cur->height);
			perp_lo = cand->x;
			perp_hi = cand->x + cand->width;
			cur_lo	= cur->x;
			cur_hi	= cur->x + cur->width;
		} else { /* up */
			/* Candidate must be above */
			if (cand->y + cand->height > cur->y)
				continue;
			dist	= cur->y - (cand->y + cand->height);
			perp_lo = cand->x;
			perp_hi = cand->x + cand->width;
			cur_lo	= cur->x;
			cur_hi	= cur->x + cur->width;
		}

		/* Require at least 1px perpendicular overlap */
		int overlap_lo = perp_lo > cur_lo ? perp_lo : cur_lo;
		int overlap_hi = perp_hi < cur_hi ? perp_hi : cur_hi;
		if (overlap_hi <= overlap_lo)
			continue;

		if (dist < best_dist) {
			best_dist = dist;
			best	  = cand;
		}
	}

	return best;
}

static void
do_screen(struct cow_view *view, const char *name, struct cmd_q *cmdq)
{
	struct cow_output *target = cow_output_find_by_spec(name);
	if (target == NULL) {
		cmdq_error(cmdq, "move: unknown output: %s", name);
		return;
	}
	if (target == view->output)
		return;

	struct cow_box *dst_ua = &target->usable_area;

	unmax_if_needed(view);

	if (view->placed) {
		/*
		 * Preserve the proportional position within the new
		 * output's usable area.  Clamp into bounds so the
		 * computed centre lands inside `target` and the
		 * subsequent stack update doesn't bounce the view onto
		 * a neighbouring output.
		 */
		struct cow_box *src_ua = &view->output->usable_area;
		double		rx = 0.0, ry = 0.0;
		if (src_ua->width > 0)
			rx = (double)(view->x - src_ua->x) / src_ua->width;
		if (src_ua->height > 0)
			ry = (double)(view->y - src_ua->y) / src_ua->height;
		if (rx < 0.0)
			rx = 0.0;
		else if (rx > 1.0)
			rx = 1.0;
		if (ry < 0.0)
			ry = 0.0;
		else if (ry > 1.0)
			ry = 1.0;
		view->x = dst_ua->x + (int)(rx * dst_ua->width);
		view->y = dst_ua->y + (int)(ry * dst_ua->height);
	} else {
		/*
		 * Window hasn't had its initial placement yet (e.g.
		 * window-move fired from an on-map rule before the
		 * render loop ran view_compute_initial_placement()).
		 * Drop it at the top-left of the target's usable area
		 * and mark it as placed below so the render loop does
		 * NOT subsequently snap the view onto the output under
		 * the pointer.
		 */
		int frame_w, frame_h, content_x, content_y;
		move_frame_geometry(view, &frame_w, &frame_h, &content_x,
		    &content_y);
		view->x = dst_ua->x + content_x;
		view->y = dst_ua->y + content_y;
	}

	cow_view_reassign_output(view, target);

	/*
	 * User-initiated relocation: forget any remembered "original"
	 * output so the new home is where the view returns to after a
	 * blackout.
	 */
	cow_view_clear_orphan(view);

	/*
	 * Mark placed so view_compute_initial_placement() (which would
	 * otherwise unconditionally reassign the view to the output
	 * under the pointer on the next render) leaves the explicit
	 * placement alone.
	 */
	view->placed = true;

	sync_pending_move_position(view);
	cow_view_sync_vxy_from_global(view);
	cow_view_sync_container_pos(view);
	cmd_view_dirty(view);
}

static void
do_interactive(struct cow_view *view)
{
	/*
	 * Interactive moves require an active manage sequence: River's
	 * river_seat_v1_op_start_pointer is only valid inside a manage
	 * sequence.  Calling it from IPC context (sequence == NONE) is a
	 * protocol violation that terminates the Wayland connection.
	 */
	if (cow_wm.sequence != COW_WM_SEQUENCE_MANAGE)
		return;
	/*
	 * Prefer the seat whose pointer initiated the current
	 * interaction (set by wm.c's decoration-click dispatch when
	 * firing a bind-decoration binding).  Falls back to the
	 * primary seat for callers without an active interaction --
	 * e.g. `window-move` fired from a keybinding.
	 */
	struct cow_seat *seat = cow_wm.current_interaction_seat;
	if (seat == NULL)
		seat = cow_seat_primary();
	if (seat != NULL) {
		/* Deferred unmax: cow_pointer_op_process unmaxes on the
		 * first non-zero drag delta.  This keeps the view's
		 * geometry intact through a press-without-motion so a
		 * 2-click bind-decoration action runs against the
		 * original state. */
		cow_pointer_op_start_move(seat, view);
	}
}

static void
do_interactive_group(struct cow_view *view, const char *selection)
{
	if (cow_wm.sequence != COW_WM_SEQUENCE_MANAGE)
		return;
	struct cow_seat *seat = cow_wm.current_interaction_seat;
	if (seat == NULL)
		seat = cow_seat_primary();
	if (seat != NULL)
		cow_pointer_op_start_group_move(seat, view, selection);
}

static void
do_step(struct cow_view *view, const char *dir, struct cmd_q *cmdq)
{
	int step = cow_config ? cow_config->step : COW_CFG_DEFAULT_STEP;
	struct cow_box *ua = &view->output->usable_area;
	int frame_w, frame_h, content_x, content_y;
	move_frame_geometry(view, &frame_w, &frame_h, &content_x, &content_y);

	unmax_if_needed(view);

	int at_right  = ua->x + ua->width;
	int at_left   = ua->x;
	int at_bottom = ua->y + ua->height;
	int at_top    = ua->y;
	struct cow_box fb = move_frame_box(view);

	bool at_edge = false;
	if (strcmp(dir, "right") == 0) {
		at_edge = (fb.x + fb.width >= at_right);
		if (!at_edge) {
			fb.x += step;
			if (fb.x + fb.width > at_right)
				fb.x = at_right - fb.width;
			view->x = fb.x + content_x;
		}
	} else if (strcmp(dir, "left") == 0) {
		at_edge = (fb.x <= at_left);
		if (!at_edge) {
			fb.x -= step;
			if (fb.x < at_left)
				fb.x = at_left;
			view->x = fb.x + content_x;
		}
	} else if (strcmp(dir, "down") == 0) {
		at_edge = (fb.y + fb.height >= at_bottom);
		if (!at_edge) {
			fb.y += step;
			if (fb.y + fb.height > at_bottom)
				fb.y = at_bottom - fb.height;
			view->y = fb.y + content_y;
		}
	} else if (strcmp(dir, "up") == 0) {
		at_edge = (fb.y <= at_top);
		if (!at_edge) {
			fb.y -= step;
			if (fb.y < at_top)
				fb.y = at_top;
			view->y = fb.y + content_y;
		}
	} else {
		cmdq_error(cmdq, "move: unknown direction: %s", dir);
		return;
	}

	/* Already at the edge: transfer to neighbour if one exists. */
	if (at_edge) {
		struct cow_output *nb = output_neighbour_in_dir(view->output,
		    dir);
		if (nb != NULL)
			do_transfer_to_output(view, nb, dir);
	}

	apply_move(view);
}

static void
do_snap(struct cow_view *view, const char *dir, struct cmd_q *cmdq)
{
	if (dir == NULL) {
		cmdq_error(cmdq, "move snap: -d direction required");
		return;
	}

	struct cow_box *ua	     = &view->output->usable_area;
	int frame_w, frame_h, content_x, content_y;
	move_frame_geometry(view, &frame_w, &frame_h, &content_x, &content_y);
	struct cow_box	fb	     = move_frame_box(view);
	int		frame_left   = fb.x;
	int		frame_top    = fb.y;
	int		frame_right  = fb.x + fb.width;
	int		frame_bottom = fb.y + fb.height;

	unmax_if_needed(view);

	/*
	 * If the frame is already at the output edge in this direction,
	 * transfer to the neighbouring output instead of snapping again.
	 */
	bool at_edge = false;
	if (strcmp(dir, "up") == 0)
		at_edge = (frame_top <= ua->y);
	else if (strcmp(dir, "down") == 0)
		at_edge = (frame_bottom >= ua->y + ua->height);
	else if (strcmp(dir, "left") == 0)
		at_edge = (frame_left <= ua->x);
	else if (strcmp(dir, "right") == 0)
		at_edge = (frame_right >= ua->x + ua->width);

	if (at_edge) {
		struct cow_output *nb = output_neighbour_in_dir(view->output,
		    dir);
		if (nb != NULL)
			do_transfer_to_output(view, nb, dir);
		apply_move(view);
		return;
	}

	uint32_t edge;
	int distance;
	if (strcmp(dir, "up") == 0) {
		edge = COW_EDGE_TOP;
		distance = frame_top - ua->y;
	} else if (strcmp(dir, "down") == 0) {
		edge = COW_EDGE_BOTTOM;
		distance = ua->y + ua->height - frame_bottom;
	} else if (strcmp(dir, "left") == 0) {
		edge = COW_EDGE_LEFT;
		distance = frame_left - ua->x;
	} else if (strcmp(dir, "right") == 0) {
		edge = COW_EDGE_RIGHT;
		distance = ua->x + ua->width - frame_right;
	} else {
		cmdq_error(cmdq, "snap: unknown direction: %s", dir);
		return;
	}
	distance = cow_frame_travel(view, &fb, edge, false, distance);
	cow_frame_extend(&fb, edge, false, distance);
	view->x = fb.x + content_x;
	view->y = fb.y + content_y;

	apply_move(view);
}

static void
do_xy(struct cow_view *view, const char *xs, const char *ys, struct cmd_q *cmdq)
{
	int  x = 0, y = 0;
	bool x_rel = false, y_rel = false;
	struct cmd_length xl = { 0 }, yl = { 0 };

	/*
	 * A leading '+' or '-' makes the value relative to the current
	 * position.  '-0' and '+0' are treated as absolute zero, since
	 * relative-by-zero is a no-op that users never actually want --
	 * in practice they have typed '-y -0' meaning '-y 0'.
	 */
	if (xs != NULL) {
		if (!cmd_parse_length(cmdq, "window-move", "x", xs, -32768,
			32767, &xl))
			return;
		if (xl.unit == CMD_LENGTH_VH) {
			cmdq_error(cmdq,
			    "window-move: x: vh is not valid for an X position");
			return;
		}
		if (!cmd_length_resolve(cmdq, "window-move", "x", &xl,
			view->output, &x))
			return;
		x_rel = (xs[0] == '+' || xs[0] == '-') && x != 0;
	}
	if (ys != NULL) {
		if (!cmd_parse_length(cmdq, "window-move", "y", ys, -32768,
			32767, &yl))
			return;
		if (yl.unit == CMD_LENGTH_VW) {
			cmdq_error(cmdq,
			    "window-move: y: vw is not valid for a Y position");
			return;
		}
		if (!cmd_length_resolve(cmdq, "window-move", "y", &yl,
			view->output, &y))
			return;
		y_rel = (ys[0] == '+' || ys[0] == '-') && y != 0;
	}

	unmax_if_needed(view);
	int frame_w, frame_h, content_x, content_y;
	move_frame_geometry(view, &frame_w, &frame_h, &content_x, &content_y);
	if (xs != NULL) {
		if (x_rel)
			view->x += x;
		else if (xl.unit == CMD_LENGTH_VW)
			view->x = view->output->usable_area.x + x + content_x;
		else
			view->x = x;
	}
	if (ys != NULL) {
		if (y_rel)
			view->y += y;
		else if (yl.unit == CMD_LENGTH_VH)
			view->y = view->output->usable_area.y + y + content_y;
		else
			view->y = y;
	}
	apply_move(view);
}

static bool
do_anchor(struct cow_view *view, const char *anchor, struct cmd_q *cmdq)
{
	enum cow_placement_anchor parsed;
	if (!cow_placement_anchor_parse(anchor, &parsed)) {
		cmdq_error(cmdq,
		    "window-move: invalid anchor '%s'", anchor);
		return (false);
	}

	int page_col = cow_view_page_col(view);
	int page_row = cow_view_page_row(view);
	unmax_if_needed(view);
	int frame_w, frame_h, content_x, content_y;
	int frame_x, frame_y;
	move_frame_geometry(view, &frame_w, &frame_h, &content_x, &content_y);
	const struct cow_box *ua = &view->output->usable_area;
	cow_placement_anchor(ua, frame_w, frame_h, parsed, &frame_x, &frame_y);
	view->x = frame_x + content_x;
	view->y = frame_y + content_y;
	apply_move(view);
	cow_view_move_to_page(view, page_col, page_row);
	return (true);
}

/* move desk N */
static void
do_desk(struct cow_view *view, uint8_t nr, struct cmd_q *cmdq)
{
	if (view->output == NULL) {
		cmdq_error(cmdq, "move desk: no output");
		return;
	}

	cow_view_assign_desk(view, nr);

	cow_update_request(COW_UPDATE_MANAGE);
}
