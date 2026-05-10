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
#include "cow/desk_ops.h"

#include <stdbool.h>

#include "cow/config.h"
#include "cow/output.h"
#include "cow/pan_frames.h"
#include "cow/rule.h"
#include "cow/seat.h"
#include "cow/status.h"
#include "cow/util.h"
#include "cow/view.h"
#include "cow/wm.h"

#include "river-window-management-v1-client-protocol.h"

/*
 * A view is visible on the given output's current desk if:
 * - It is on the current desk, OR
 * - It has the STICKY flag set
 * AND it is not iconified.
 */
/*
 * Predicate: is 'view' (assumed to be on the current desk of
 * 'output') visible on the current page?  Returns true when the page
 * grid is 1x1 (single page) or the view is page-sticky.  Otherwise
 * applies one of two rules:
 *
 *   edge_scroll = true  : page-strict.  A view lives on the page
 *                         containing its top-left corner.
 *   edge_scroll = false : FVWM-style overlap.  A view is visible if
 *                         its virtual frame rectangle intersects the
 *                         current viewport.
 */
static bool
view_visible_in_page(const struct cow_view *view,
    const struct cow_output		   *output)
{
	if (view->flags & COW_VIEW_STICKY_PAGE_FLAG)
		return true;
	if (cow_config == NULL ||
	    (cow_config->page_cols <= 1 && cow_config->page_rows <= 1))
		return true;

	struct cow_desk *desk = output->current_desk;
	int		 w    = output->usable_area.width;
	int		 h    = output->usable_area.height;
	int		 vpx  = desk->page_vx;
	int		 vpy  = desk->page_vy;

	if (cow_config->edge_scroll) {
		return !(view->vx < vpx || view->vx >= vpx + w ||
		    view->vy < vpy || view->vy >= vpy + h);
	}
	int vw = view->content_width + 2 * view->dec.border_width;
	int vh = view->content_height + 2 * view->dec.border_width +
	    view->dec.title_thickness;
	return !(view->vx + vw <= vpx || view->vx >= vpx + w ||
	    view->vy + vh <= vpy || view->vy >= vpy + h);
}

bool
cow_view_visible(const struct cow_view *view, const struct cow_output *output)
{
	if (view->closed)
		return false;
	if (view->flags & COW_VIEW_ICONIFIED_FLAG)
		return false;
	if (view->output != output)
		return false;
	/* Non-active container members are always hidden */
	if (view->container_member != NULL &&
	    view->container_member->container->active != view->container_member)
		return false;
	if (view->desk == NULL)
		return true;
	if (view->desk == output->current_desk)
		return view_visible_in_page(view, output);
	if (view->flags & COW_VIEW_STICKY_DESK_FLAG)
		return true;
	/* Visible if on a collected desk */
	if (output->collected_desks & (1u << view->desk->nr))
		return true;
	return false;
}

/*
 * Switch a single output to a new desk (internal helper).
 */
static void
switch_output_desk(struct cow_output *output, uint8_t desk_nr)
{
	struct cow_desk *target = &output->desks[desk_nr];

	if (target == output->current_desk)
		return;

	/* Restore geometry for any collected windows before clearing */
	if (output->collected_desks != 0) {
		struct cow_view *cv;
		wl_list_for_each(cv, &cow_wm.views, wm_link)
		{
			if (cv->output != output || cv->closed)
				continue;
			if (!cv->collected_geo_saved)
				continue;
			cv->x = cv->collected_saved_x;
			cv->y = cv->collected_saved_y;
			cow_view_set_pending_geometry(cv, cv->collected_saved_x,
			    cv->collected_saved_y, cv->collected_saved_width,
			    cv->collected_saved_height);
			cv->collected_geo_saved = false;
		}
	}

	output->current_desk	= target;
	output->collected_desks = 0;

	/* Mark views on this output dirty so the new desk's geometry is
	 * applied on the next render. */
	struct cow_view *view;
	wl_list_for_each(view, &cow_wm.views, wm_link)
	{
		if (view->output == output && !view->closed)
			view->dec.dirty = true;
	}

	/* Clear focused view if it is no longer visible on this output. */
	if (cow_wm.focused_view != NULL &&
	    !cow_view_visible(cow_wm.focused_view, output)) {
		cow_wm.focused_view = NULL;
		struct cow_seat *seat;
		wl_list_for_each(seat, &cow_wm.seats, wm_link)
		{
			if (seat->focused_view != NULL &&
			    !cow_view_visible(seat->focused_view, output))
				seat->focused_view = NULL;
		}
	}

	cow_rule_fire(COW_RULE_ON_MAP, NULL, output->current_desk, output);
	cow_rule_fire(COW_RULE_ON_NEW_DESK, NULL, output->current_desk, output);
}

void
cow_desk_switch(struct cow_output *output, uint8_t desk_nr)
{
	if (desk_nr >= COW_NR_OF_DESKS)
		return;

	int mode = cow_config ? cow_config->desktop_config : COW_DESKTOP_GLOBAL;

	switch (mode) {
	case COW_DESKTOP_GLOBAL:
		/* All outputs switch to the same desk */
		{
			struct cow_output *out;
			wl_list_for_each(out, &cow_wm.outputs, wm_link)
			{
				switch_output_desk(out, desk_nr);
			}
		}
		break;

	case COW_DESKTOP_PER_OUTPUT:
		/* Only this output switches */
		switch_output_desk(output, desk_nr);
		break;

	case COW_DESKTOP_SHARED:
		/* Each output views a different desk number.  If another
		 * output is already showing the target desk, swap with it. */
		{
			struct cow_output *other;
			wl_list_for_each(other, &cow_wm.outputs, wm_link)
			{
				if (other == output)
					continue;
				if (other->current_desk->nr == desk_nr) {
					/* Swap: other gets our current desk */
					uint8_t our_nr =
					    output->current_desk->nr;
					switch_output_desk(other, our_nr);
					break;
				}
			}
			switch_output_desk(output, desk_nr);
		}
		break;

	default:
		/* unreachable: mode is constrained to the three cases */
		break;
	}

	cow_status_emit();
	cow_wm_request_manage_dirty();
}

void
cow_view_assign_desk(struct cow_view *view, uint8_t desk_nr)
{
	if (desk_nr >= COW_NR_OF_DESKS || view == NULL || view->closed)
		return;

	struct cow_output *output = view->output;
	if (output == NULL)
		return;

	struct cow_desk *target = &output->desks[desk_nr];

	if (view->desk == target)
		return;

	/* Remove from old desk's view list */
	wl_list_remove(&view->desk_views);

	/* Add to new desk */
	view->desk = target;
	wl_list_insert(&target->views, &view->desk_views);

	view->dec.dirty = true;

	/* Visibility recomputed in render callback */
	cow_wm_request_manage_dirty();
}

/* -----------------------------------------------------------------------
 * Page navigation
 * ----------------------------------------------------------------------- */

int
cow_page_col(const struct cow_output *output)
{
	if (output == NULL || cow_config == NULL || cow_config->page_cols <= 1)
		return 0;
	int w = output->usable_area.width;
	if (w <= 0)
		return 0;
	return output->current_desk->page_vx / w;
}

int
cow_page_row(const struct cow_output *output)
{
	if (output == NULL || cow_config == NULL || cow_config->page_rows <= 1)
		return 0;
	int h = output->usable_area.height;
	if (h <= 0)
		return 0;
	return output->current_desk->page_vy / h;
}

/* Switch a single output to the given page (internal helper). */
static void
switch_output_page(struct cow_output *output, int new_vx, int new_vy)
{
	struct cow_desk *desk = output->current_desk;
	if (desk->page_vx == new_vx && desk->page_vy == new_vy)
		return;

	if (cow_config != NULL && cow_config->page_scroll_animate) {
		desk->scroll_from_vx  = desk->page_vx;
		desk->scroll_from_vy  = desk->page_vy;
		desk->scroll_to_vx    = new_vx;
		desk->scroll_to_vy    = new_vy;
		desk->scroll_start_ms = cow_mono_ms();
		desk->scrolling	      = true;
	} else {
		desk->page_vx	= new_vx;
		desk->page_vy	= new_vy;
		desk->scrolling = false;
	}

	int		 w = output->usable_area.width;
	int		 h = output->usable_area.height;
	struct cow_view *view;
	wl_list_for_each(view, &cow_wm.views, wm_link)
	{
		if (view->closed || view->output != output)
			continue;
		if (view->desk != desk &&
		    !(view->flags & COW_VIEW_STICKY_PAGE_FLAG))
			continue;
		/*
		 * Page-sticky windows must follow the viewport: update their
		 * virtual position to keep the same within-page offset on the
		 * new page.  Without this the render formula
		 * (output->x + vx - new_page_vx) lands them on a different
		 * physical output whenever vx was anchored to a different page.
		 */
		if ((view->flags & COW_VIEW_STICKY_PAGE_FLAG) && w > 0 &&
		    h > 0) {
			int within_x = view->vx % w;
			int within_y = view->vy % h;
			view->vx     = new_vx + within_x;
			view->vy     = new_vy + within_y;
		}
		view->dec.dirty = true;
	}

	cow_rule_fire(COW_RULE_ON_NEW_PAGE, NULL, desk, output);
}

void
cow_page_switch(struct cow_output *output, int col, int row)
{
	if (output == NULL || cow_config == NULL)
		return;

	int cols = cow_config->page_cols;
	int rows = cow_config->page_rows;
	int w	 = output->usable_area.width;
	int h	 = output->usable_area.height;

	if (w <= 0 || h <= 0)
		return;

	/* Clamp to valid page range */
	if (col < 0)
		col = 0;
	if (col >= cols)
		col = cols - 1;
	if (row < 0)
		row = 0;
	if (row >= rows)
		row = rows - 1;

	int new_vx = col * w;
	int new_vy = row * h;

	/*
	 * Respect desktop-configuration:
	 * - global:     all outputs switch to the same page together
	 * - per-output: only the given output switches
	 * - shared:     only the given output switches (each output
	 *               already has an independent current desk)
	 */
	switch (cow_config->desktop_config) {
	case COW_DESKTOP_GLOBAL: {
		struct cow_output *o;
		wl_list_for_each(o, &cow_wm.outputs, wm_link)
		{
			int ow = o->usable_area.width;
			int oh = o->usable_area.height;
			if (ow <= 0 || oh <= 0)
				continue;
			/* Scale page coordinates to this output's size */
			switch_output_page(o, col * ow, row * oh);
		}
		break;
	}
	default:
		/* per-output or shared: only switch the requested output */
		switch_output_page(output, new_vx, new_vy);
		break;
	}

	cow_status_emit();
	cow_wm_request_manage_dirty();
	/* Scrollable edges change after a page switch */
	pan_frames_update_all();
}

void
cow_view_move_to_page(struct cow_view *view, int col, int row)
{
	if (view == NULL || view->output == NULL || cow_config == NULL)
		return;

	int w = view->output->usable_area.width;
	int h = view->output->usable_area.height;
	if (w <= 0 || h <= 0)
		return;

	int cols = cow_config->page_cols;
	int rows = cow_config->page_rows;
	if (col < 0)
		col = 0;
	if (col >= cols)
		col = cols - 1;
	if (row < 0)
		row = 0;
	if (row >= rows)
		row = rows - 1;

	/* Compute within-page position (offset from current page origin) */
	int cur_page_vx = (view->vx / w) * w;
	int cur_page_vy = (view->vy / h) * h;
	int within_x	= view->vx - cur_page_vx;
	int within_y	= view->vy - cur_page_vy;

	/* Place on the new page preserving within-page position */
	view->vx = col * w + within_x;
	view->vy = row * h + within_y;

	view->dec.dirty = true;
	cow_wm_request_manage_dirty();
}

void
cow_page_clamp_views(struct cow_output *output)
{
	if (output == NULL || cow_config == NULL)
		return;

	int w	 = output->usable_area.width;
	int h	 = output->usable_area.height;
	int cols = cow_config->page_cols;
	int rows = cow_config->page_rows;
	if (w <= 0 || h <= 0)
		return;

	int vx_max = (cols - 1) * w;
	int vy_max = (rows - 1) * h;

	struct cow_desk *desk;
	for (int i = 0; i < COW_NR_OF_DESKS; i++) {
		desk = &output->desks[i];
		if (desk->page_vx > vx_max)
			desk->page_vx = vx_max;
		if (desk->page_vy > vy_max)
			desk->page_vy = vy_max;
	}

	struct cow_view *view;
	wl_list_for_each(view, &cow_wm.views, wm_link)
	{
		if (view->closed || view->output != output)
			continue;

		/*
		 * Clamp to nearest valid page.  Truncating signed division
		 * rounds toward zero, not toward minus-infinity, so negative
		 * coordinates would be treated as page 0; use floor-div here.
		 * The modulo is normalised into [0, dim) for the same reason.
		 */
		int page_col = (view->vx >= 0) ? view->vx / w :
						 -((-view->vx + w - 1) / w);
		int page_row = (view->vy >= 0) ? view->vy / h :
						 -((-view->vy + h - 1) / h);
		if (page_col < 0) {
			int within = ((view->vx % w) + w) % w;
			view->vx   = within;
		} else if (page_col >= cols) {
			int within = ((view->vx % w) + w) % w;
			view->vx   = (cols - 1) * w + within;
		}
		if (page_row < 0) {
			int within = ((view->vy % h) + h) % h;
			view->vy   = within;
		} else if (page_row >= rows) {
			int within = ((view->vy % h) + h) % h;
			view->vy   = (rows - 1) * h + within;
		}
		view->dec.dirty = true;
	}
	cow_wm_request_manage_dirty();
}

/*
 * Advance the smooth page scroll animation for one output.
 * Call this from the manage cycle.  Returns true while animation is
 * in progress (so the caller should set needs_manage_dirty again).
 */
bool
cow_page_scroll_tick(struct cow_output *output)
{
	if (output == NULL || !cow_config || !cow_config->page_scroll_animate)
		return false;

	struct cow_desk *desk = output->current_desk;
	if (!desk->scrolling)
		return false;

	long now_ms  = cow_mono_ms();
	long elapsed = now_ms - desk->scroll_start_ms;
	int  dur = cow_config->page_scroll_ms > 0 ? cow_config->page_scroll_ms :
						    200;

	if (elapsed >= dur) {
		/* Animation complete: snap to target */
		desk->page_vx	= desk->scroll_to_vx;
		desk->page_vy	= desk->scroll_to_vy;
		desk->scrolling = false;
		cow_wm_request_manage_dirty();
		/* Update pan frames now that the final page position is
		 * committed — scrollable edges may have changed. */
		pan_frames_update_all();
		return false;
	}

	/* Linear interpolation */
	float t	      = (float)elapsed / (float)dur;
	desk->page_vx = desk->scroll_from_vx +
	    (int)(t * (desk->scroll_to_vx - desk->scroll_from_vx));
	desk->page_vy = desk->scroll_from_vy +
	    (int)(t * (desk->scroll_to_vy - desk->scroll_from_vy));

	cow_wm_request_manage_dirty();
	return true;
}
