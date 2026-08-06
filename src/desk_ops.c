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
#include "cow/titlebar.h"
#include "cow/util.h"
#include "cow/view.h"
#include "cow/wm.h"

#include "river-window-management-v1-client-protocol.h"

/*
 * Predicate: is 'view' (assumed to be on the current desk of
 * 'output') visible on the current page?
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
	int		 w    = cow_page_width(output);
	int		 h    = cow_page_height(output);
	int		 vpx  = desk->page_vx;
	int		 vpy  = desk->page_vy;

	if (w <= 0 || h <= 0)
		return true;

	bool origin_visible = !(view->vx < vpx || view->vx >= vpx + w ||
	    view->vy < vpy || view->vy >= vpy + h);

	int frame_w, frame_h, content_x, content_y;
	cow_titlebar_frame_geometry(view, view->content_width,
	    view->content_height, &frame_w, &frame_h, &content_x, &content_y);
	int fx = view->vx - content_x;
	int fy = view->vy - content_y;

	bool page_sized = frame_w >= w && frame_h >= h &&
	    fx % w == 0 && fy % h == 0;

	if (view->fullscreen || view->maximized || page_sized)
		return origin_visible;

	/* Sub-page scrolls use overlap visibility even with edge_scroll. */
	bool aligned = (vpx % w == 0 && vpy % h == 0);

	if (cow_config->edge_scroll && aligned)
		return origin_visible;
	return !(fx + frame_w <= vpx || fx >= vpx + w ||
	    fy + frame_h <= vpy || fy >= vpy + h);
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

/* Switch one output to a new desk. */
static void
switch_output_desk(struct cow_output *output, uint8_t desk_nr)
{
	struct cow_desk *target = cow_desk_by_nr(output, desk_nr);
	if (target == NULL)
		return;

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

	/* Recompute geometry on next render. */
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

	cow_rule_fire(COW_RULE_ON_NEW_DESK, NULL, output->current_desk, output);
	cow_rule_fire(COW_RULE_ON_DESK, NULL, output->current_desk, output);
}

void
cow_desk_switch(struct cow_output *output, uint8_t desk_nr)
{
	if (output != NULL && cow_desk_by_nr(output, desk_nr) == NULL)
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
		/* Swap if another output already shows the target desk. */
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
	if (view == NULL || view->closed)
		return;

	struct cow_output *output = view->output;
	if (output == NULL)
		return;

	struct cow_desk *target = cow_desk_by_nr(output, desk_nr);
	if (target == NULL)
		return;

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

int
cow_page_col(const struct cow_output *output)
{
	if (output == NULL || cow_config == NULL || cow_config->page_cols <= 1)
		return 0;
	int w = cow_page_width(output);
	if (w <= 0)
		return 0;
	return output->current_desk->page_vx / w;
}

int
cow_page_row(const struct cow_output *output)
{
	if (output == NULL || cow_config == NULL || cow_config->page_rows <= 1)
		return 0;
	int h = cow_page_height(output);
	if (h <= 0)
		return 0;
	return output->current_desk->page_vy / h;
}

int
cow_page_width(const struct cow_output *output)
{
	return output != NULL ? output->width : 0;
}

int
cow_page_height(const struct cow_output *output)
{
	return output != NULL ? output->height : 0;
}

/* Switch a single output to the given page (internal helper). */
static void
switch_output_page(struct cow_output *output, int new_vx, int new_vy)
{
	struct cow_desk *desk = output->current_desk;
	if (desk->page_vx == new_vx && desk->page_vy == new_vy)
		return;

	int old_vx = desk->page_vx;
	int old_vy = desk->page_vy;

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

	struct cow_view *view;
	wl_list_for_each(view, &cow_wm.views, wm_link)
	{
		if (view->closed || view->output != output)
			continue;

		/* Only the scrolled desk's views need page adjustment. */
		if (view->desk != desk)
			continue;

		/* Page-sticky views track the viewport delta. */
		if (view->flags & COW_VIEW_STICKY_PAGE_FLAG) {
			view->vx += new_vx - old_vx;
			view->vy += new_vy - old_vy;
		}
		view->dec.dirty = true;
	}

	cow_rule_fire(COW_RULE_ON_NEW_PAGE, NULL, desk, output);
	cow_rule_fire(COW_RULE_ON_DESK, NULL, desk, output);
}

void
cow_page_switch(struct cow_output *output, int col, int row)
{
	if (output == NULL || cow_config == NULL)
		return;

	int cols = cow_config->page_cols;
	int rows = cow_config->page_rows;
	int w	 = cow_page_width(output);
	int h	 = cow_page_height(output);

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

	switch (cow_config->desktop_config) {
	case COW_DESKTOP_GLOBAL: {
		struct cow_output *o;
		wl_list_for_each(o, &cow_wm.outputs, wm_link)
		{
			int ow = cow_page_width(o);
			int oh = cow_page_height(o);
			if (ow <= 0 || oh <= 0)
				continue;
			/* Scale page coordinates to this output's size */
			switch_output_page(o, col * ow, row * oh);
		}
		break;
	}
	default:
		switch_output_page(output, new_vx, new_vy);
		break;
	}

	cow_status_emit();
	cow_wm_request_manage_dirty();
	/* Scrollable edges change after a page switch */
	pan_frames_update_all();
}

void
cow_page_scroll_to(struct cow_output *output, int new_vx, int new_vy, bool wrap)
{
	if (output == NULL || cow_config == NULL)
		return;

	int cols = cow_config->page_cols;
	int rows = cow_config->page_rows;
	int w	 = cow_page_width(output);
	int h	 = cow_page_height(output);
	if (w <= 0 || h <= 0)
		return;

	int desk_w = cols * w;
	int desk_h = rows * h;
	if (desk_w <= 0 || desk_h <= 0)
		return;

	if (wrap) {
		/* Mod into [0, desk_extent).  C99 % can be negative for
		 * negative inputs, so normalise. */
		new_vx %= desk_w;
		if (new_vx < 0)
			new_vx += desk_w;
		new_vy %= desk_h;
		if (new_vy < 0)
			new_vy += desk_h;
	} else {
		/* Clamp to the range cow_page_switch reaches. */
		int vx_max = (cols - 1) * w;
		int vy_max = (rows - 1) * h;
		if (new_vx < 0)
			new_vx = 0;
		if (new_vx > vx_max)
			new_vx = vx_max;
		if (new_vy < 0)
			new_vy = 0;
		if (new_vy > vy_max)
			new_vy = vy_max;
	}

	/* Sub-page scroll is intentionally instant. */
	bool saved_animate = false;
	if (cow_config != NULL) {
		saved_animate = cow_config->page_scroll_animate;
		cow_config->page_scroll_animate = false;
	}
	switch_output_page(output, new_vx, new_vy);
	if (cow_config != NULL)
		cow_config->page_scroll_animate = saved_animate;

	cow_status_emit();
	cow_wm_request_manage_dirty();
	pan_frames_update_all();
}

void
cow_view_move_to_page(struct cow_view *view, int col, int row)
{
	if (view == NULL || view->output == NULL || cow_config == NULL)
		return;

	int w = cow_page_width(view->output);
	int h = cow_page_height(view->output);
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

	/* Preserve within-page position. */
	int cur_page_vx = (view->vx / w) * w;
	int cur_page_vy = (view->vy / h) * h;
	int within_x	= view->vx - cur_page_vx;
	int within_y	= view->vy - cur_page_vy;

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

	int w	 = cow_page_width(output);
	int h	 = cow_page_height(output);
	int cols = cow_config->page_cols;
	int rows = cow_config->page_rows;
	if (w <= 0 || h <= 0)
		return;

	int vx_max = (cols - 1) * w;
	int vy_max = (rows - 1) * h;

	struct cow_desk *desk;
	wl_list_for_each(desk, &output->desk_set.desks, set_link)
	{
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

		/* Floor-div keeps negative virtual coordinates on their page. */
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

/* Advance one output's smooth page-scroll animation. */
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
