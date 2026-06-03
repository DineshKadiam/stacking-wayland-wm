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
#include "cow/output.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cow/colour.h"
#include "cow/compat.h"
#include "cow/config.h"
#include "cow/layer_shell.h"
#include "cow/memory.h"
#include "cow/shm.h"
#include "cow/util.h"
#include "cow/wm.h"
#include "cow/desk_ops.h"
#include "cow/pan_frames.h"
#include "cow/seat.h"
#include "cow/view.h"

#include "river-window-management-v1-client-protocol.h"

/*
 * Recompute the usable area from the output dimensions (or layer shell
 * non_exclusive_area), subtracting the configured reserved area.
 */
void
cow_output_apply_reserved_area(struct cow_output *output)
{
	if (cow_config == NULL)
		return;

	int rt = cow_config->reserved.top;
	int rb = cow_config->reserved.bottom;
	int rl = cow_config->reserved.left;
	int rr = cow_config->reserved.right;

	if (rt == 0 && rb == 0 && rl == 0 && rr == 0)
		return;

	output->usable_area.x += rl;
	output->usable_area.y += rt;
	output->usable_area.width -= rl + rr;
	output->usable_area.height -= rt + rb;

	if (output->usable_area.width < 100)
		output->usable_area.width = 100;
	if (output->usable_area.height < 100)
		output->usable_area.height = 100;
}

/*
 * river_output_v1 event handlers
 */

static void
output_handle_removed(void *data, __unused struct river_output_v1 *river_output)
{
	struct cow_output *output = data;

	fprintf(stderr, "cow: output removed\n");
	output->removed = true;
}

static void
output_handle_wl_output(void	    *data,
    __unused struct river_output_v1 *river_output, uint32_t name)
{
	struct cow_output *output = data;
	output->wl_output_name	  = name;

	/* Try to match a bound wl_output to get the human-readable name */
	struct cow_wl_output_entry *entry;
	wl_list_for_each(entry, &cow_wm.wl_outputs, link)
	{
		if (entry->global_name == name) {
			output->wl_output = entry->wl_output;
			if (entry->name != NULL && output->name == NULL)
				output->name = xstrdup(entry->name);
			break;
		}
	}

	/* wl_output is now known — try to create pan frames if dimensions
	 * are also already set and the config has finished loading. */
	if (!cow_wm.exec_pending && output->wl_output != NULL &&
	    output->width > 0 && output->height > 0)
		pan_frames_update_output(output);
}

static void
output_handle_position(void	    *data,
    __unused struct river_output_v1 *river_output, int32_t x, int32_t y)
{
	struct cow_output *output  = data;
	output->x		   = x;
	output->y		   = y;
	output->usable_area.x	   = x;
	output->usable_area.y	   = y;
	output->usable_area.width  = output->width;
	output->usable_area.height = output->height;
	cow_output_apply_reserved_area(output);

	/* A monitor moved -- the raster-scan order may have changed. */
	cow_outputs_renumber();
}

static void
output_handle_dimensions(void	    *data,
    __unused struct river_output_v1 *river_output, int32_t width,
    int32_t height)
{
	struct cow_output *output  = data;
	output->width		   = width;
	output->height		   = height;
	output->usable_area.x	   = output->x;
	output->usable_area.y	   = output->y;
	output->usable_area.width  = width;
	output->usable_area.height = height;

	/* At runtime (after startup) apply reserved immediately.
	 * During startup this is skipped — wm.c applies reserved once
	 * after the second roundtrip when all initial events have settled. */
	if (!cow_wm.exec_pending)
		cow_output_apply_reserved_area(output);

	/* If config has already been loaded, (re-)create pan frames now that
	 * we have valid dimensions.  During startup this is skipped and
	 * pan_frames_update_all() is called once at end of exec_pending. */
	if (!cow_wm.exec_pending)
		pan_frames_update_output(output);
}

static const struct river_output_v1_listener output_listener = {
	.removed    = output_handle_removed,
	.wl_output  = output_handle_wl_output,
	.position   = output_handle_position,
	.dimensions = output_handle_dimensions,
};

static void output_adopt_orphan_views(struct cow_output *);

struct cow_output *
cow_output_create(struct river_output_v1 *river_output)
{
	struct cow_output *output = xcalloc(1, sizeof(*output));

	output->river_output = river_output;
	output->removed	     = false;
	output->pan_edge     = -1;
	wl_list_init(&output->visible_views);

	cow_desk_set_init(&output->desk_set);
	cow_desk_set_populate_defaults(output);
	output->current_desk = cow_desk_first(output);

	river_output_v1_add_listener(river_output, &output_listener, output);

	wl_list_insert(&cow_wm.outputs, &output->wm_link);

	/* Set as current output if we don't have one yet */
	if (cow_wm.current_output == NULL) {
		cow_wm.current_output = output;
	}

	/* Register with layer shell for non_exclusive_area events */
	cow_layer_shell_init_output(output);

	/* If a previous output disappearance left views orphaned (no
	 * output to attach to), adopt them onto this new output.
	 */
	output_adopt_orphan_views(output);

	/* Assign a tentative ordinal.  The compositor will follow this
	 * with `position` and `dimensions` events; renumber again
	 * there so the ordering reflects the final geometry. */
	cow_outputs_renumber();

	return output;
}

/*
 * Find a surviving output to migrate orphaned views onto.  Returns the
 * first entry of cow_wm.outputs that isn't `dying`, or NULL if `dying`
 * is the only output left.  Called while `dying` is still on the
 * outputs list, so any other entry is a valid target.
 */
static struct cow_output *
output_find_migration_target(struct cow_output *dying)
{
	struct cow_output *o;
	wl_list_for_each(o, &cow_wm.outputs, wm_link)
	{
		if (o != dying)
			return (o);
	}
	return (NULL);
}

/*
 * Snapshot of the dying output's active desk and that desk's page
 * state.  Captured when the last surviving output goes away and
 * consumed by the first output to appear afterwards, so the user's
 * "where am I" returns with their windows.
 *
 * desk_nr == -1 means no snapshot pending.
 */
static struct {
	int desk_nr;
	int page_vx;
	int page_vy;
} pending_session = { .desk_nr = -1 };

/*
 * Move every view currently on `dying` onto `target` (when there is
 * one).
 *
 * Desk identity is preserved when possible:
 *   - with a target: if the source view was on desk N and `target`
 *     also has a desk N, the view lands on target's desk N rather
 *     than on target's current_desk.
 *   - without a target: the source desk number is remembered on the
 *     view (view->orphan_desk_nr) so that when a new output appears
 *     and adopts the view (output_adopt_orphan_views) the same desk
 *     number is restored.
 *
 * Views are NOT closed on the no-target path: the session may be
 * spending a brief period with every monitor disabled (e.g. a laptop
 * whose internal panel is forced off and whose only external display
 * was just powered off).  When a monitor returns the views resume
 * normally.
 */
static void
output_migrate_views(struct cow_output *dying, struct cow_output *target)
{
	bool	preserve = cow_config != NULL &&
		     cow_config->preserve_desk_on_output_loss;

	/*
	 * Last output going away: snapshot its active desk + page so
	 * the next output to appear can restore the session.
	 */
	if (target == NULL && dying != NULL &&
	    dying->current_desk != NULL) {
		pending_session.desk_nr = (int)dying->current_desk->nr;
		pending_session.page_vx = dying->current_desk->page_vx;
		pending_session.page_vy = dying->current_desk->page_vy;
	}

	struct cow_view *view;
	wl_list_for_each(view, &cow_wm.views, wm_link)
	{
		if (view->output != dying)
			continue;
		if (target != NULL) {
			uint8_t old_nr = view->desk != NULL ?
			    view->desk->nr : 0;
			bool had_desk = view->desk != NULL;
			cow_view_reassign_output(view, target);
			if (preserve && had_desk)
				cow_view_assign_desk(view, old_nr);
		} else {
			view->orphan_desk_nr = (view->desk != NULL && preserve)
			    ? (int8_t)view->desk->nr : -1;
			wl_list_remove(&view->desk_views);
			wl_list_init(&view->desk_views);
			wl_list_remove(&view->output_views);
			wl_list_init(&view->output_views);
			view->output = NULL;
			view->desk   = NULL;
		}
	}
}

/* Adopt views that were left orphaned by a previous output's destruction. */
static void
output_adopt_orphan_views(struct cow_output *adopter)
{
	if (adopter == NULL || adopter->current_desk == NULL)
		return;

	/*
	 * Restore the snapshot taken when the last output disappeared,
	 * if any: switch the adopter to the same desk number and put
	 * that desk on the same page.  One-shot: consumed by the first
	 * adopter after a blackout.
	 */
	if (pending_session.desk_nr >= 0) {
		struct cow_desk *d = cow_desk_by_nr(adopter,
		    (uint8_t)pending_session.desk_nr);
		if (d != NULL) {
			adopter->current_desk = d;
			d->page_vx	      = pending_session.page_vx;
			d->page_vy	      = pending_session.page_vy;
		}
		pending_session.desk_nr = -1;
	}

	struct cow_view *view;
	wl_list_for_each(view, &cow_wm.views, wm_link)
	{
		if (view->output != NULL || view->closed)
			continue;
		cow_view_reassign_output(view, adopter);
		if (view->orphan_desk_nr >= 0) {
			cow_view_assign_desk(view,
			    (uint8_t)view->orphan_desk_nr);
			view->orphan_desk_nr = -1;
		}
	}
}

void
cow_output_destroy(struct cow_output *output)
{
	/*
	 * Detach every view from this output before tearing it down.
	 * cow_view_reassign_output() rewrites the inline desk_views /
	 * output_views links, so it must run while the old output's
	 * desk_set is still intact (cow_desk_set_fini below frees the
	 * desks themselves).
	 */
	output_migrate_views(output, output_find_migration_target(output));

	pan_frames_destroy_output(output);
	cow_layer_shell_fini_output(output);
	wl_list_remove(&output->wm_link);

	if (cow_wm.current_output == output) {
		if (!wl_list_empty(&cow_wm.outputs)) {
			cow_wm.current_output =
			    wl_container_of(cow_wm.outputs.next,
				cow_wm.current_output, wm_link);
		} else {
			cow_wm.current_output = NULL;
		}
	}

	/* Background shell surface resources */
	if (output->bg_buffer != NULL)
		cow_shm_buffer_destroy(output->bg_buffer);
	if (output->bg_node != NULL)
		river_node_v1_destroy(output->bg_node);
	if (output->bg_shell_surface != NULL)
		river_shell_surface_v1_destroy(output->bg_shell_surface);
	if (output->bg_surface != NULL)
		wl_surface_destroy(output->bg_surface);

	cow_desk_set_fini(&output->desk_set);
	xfree(output->name);
	river_output_v1_destroy(output->river_output);
	xfree(output);

	/* Numbering shifts when an output disappears. */
	cow_outputs_renumber();
}

void
cow_output_prepare_background(struct cow_output *output)
{
	if (cow_config == NULL || !cow_config->has_output_bg_colour)
		return;
	if (output->width <= 0 || output->height <= 0)
		return;

	/* Create wl_surface + shell_surface during manage sequence */
	if (output->bg_surface == NULL) {
		output->bg_surface = wl_compositor_create_surface(
		    cow_wm.compositor);
		if (output->bg_surface == NULL)
			return;
	}

	if (output->bg_shell_surface == NULL) {
		output->bg_shell_surface =
		    river_window_manager_v1_get_shell_surface(cow_wm.manager,
			output->bg_surface);
		if (output->bg_shell_surface == NULL)
			return;
	}

	if (output->bg_node == NULL) {
		output->bg_node = river_shell_surface_v1_get_node(
		    output->bg_shell_surface);
	}
}

void
cow_output_render_background(struct cow_output *output)
{
	/* Use per-output colour if set, otherwise fall back to global */
	bool has_colour = output->has_bg_colour ||
	    (cow_config != NULL && cow_config->has_output_bg_colour);
	if (!has_colour)
		return;
	if (output->bg_node == NULL || output->bg_surface == NULL ||
	    output->bg_shell_surface == NULL)
		return;
	if (output->width <= 0 || output->height <= 0)
		return;

	/* Only re-render if dirty or dimensions changed */
	bool needs_render = output->bg_buffer == NULL || output->bg_dirty ||
	    output->bg_buffer->width != output->width ||
	    output->bg_buffer->height != output->height;

	if (needs_render) {
		/* Free old buffer if wrong size or busy */
		if (output->bg_buffer != NULL &&
		    (output->bg_buffer->width != output->width ||
			output->bg_buffer->height != output->height ||
			output->bg_buffer->busy)) {
			cow_shm_buffer_destroy(output->bg_buffer);
			output->bg_buffer = NULL;
		}

		if (output->bg_buffer == NULL) {
			output->bg_buffer = cow_shm_buffer_create(output->width,
			    output->height);
			if (output->bg_buffer == NULL)
				return;
		}

		/* Fill with background colour (per-output overrides global) */
		const float *bg	       = output->has_bg_colour ?
			   output->bg_colour :
			   cow_config->output_bg_colour;
		uint32_t     px	       = cow_colour_to_pixel(bg);
		int	     stride_px = output->bg_buffer->stride / 4;
		uint32_t    *data      = output->bg_buffer->data;
		for (int y = 0; y < output->height; y++) {
			for (int x = 0; x < output->width; x++) {
				data[y * stride_px + x] = px;
			}
		}
		output->bg_dirty = false;
	}

	if (output->bg_buffer == NULL)
		return;

	/* Position at output origin, place at bottom */
	river_node_v1_set_position(output->bg_node, output->x, output->y);
	river_node_v1_place_bottom(output->bg_node);

	/* Commit the surface */
	wl_surface_attach(output->bg_surface, output->bg_buffer->buffer, 0, 0);
	wl_surface_damage_buffer(output->bg_surface, 0, 0, output->width,
	    output->height);
	river_shell_surface_v1_sync_next_commit(output->bg_shell_surface);
	wl_surface_commit(output->bg_surface);
	output->bg_buffer->busy = true;
}

struct cow_output *
cow_output_at(int x, int y)
{
	struct cow_output *output;
	wl_list_for_each(output, &cow_wm.outputs, wm_link)
	{
		if (x >= output->x && x < output->x + output->width &&
		    y >= output->y && y < output->y + output->height) {
			return output;
		}
	}
	return cow_wm.current_output;
}

struct cow_output *
cow_output_find(struct river_output_v1 *river_output)
{
	struct cow_output *output;
	wl_list_for_each(output, &cow_wm.outputs, wm_link)
	{
		if (output->river_output == river_output)
			return output;
	}
	return NULL;
}

struct cow_output *
cow_output_find_by_name(const char *name)
{
	if (name == NULL)
		return NULL;

	struct cow_output *output;
	wl_list_for_each(output, &cow_wm.outputs, wm_link)
	{
		if (output->name != NULL && strcmp(output->name, name) == 0)
			return output;
	}
	return NULL;
}

/*
 * Stable raster-scan compare for cow_outputs_renumber's qsort:
 *   primary key   = y  (smaller wins -> higher up the screen)
 *   secondary key = x  (smaller wins -> further left)
 *   tertiary key  = original list position (stable for identical
 *                   coordinates, e.g. two outputs mirrored at (X, Y))
 */
struct renumber_item {
	struct cow_output *output;
	int		   seq;
};

static int
renumber_cmp(const void *a, const void *b)
{
	const struct renumber_item *ra = a;
	const struct renumber_item *rb = b;
	if (ra->output->y != rb->output->y)
		return (ra->output->y < rb->output->y) ? -1 : 1;
	if (ra->output->x != rb->output->x)
		return (ra->output->x < rb->output->x) ? -1 : 1;
	return ra->seq - rb->seq;
}

void
cow_outputs_renumber(void)
{
	int		   n = 0;
	struct cow_output *o;
	wl_list_for_each(o, &cow_wm.outputs, wm_link) n++;
	if (n == 0)
		return;

	struct renumber_item *items = xcalloc((size_t)n, sizeof(*items));
	int		      i	    = 0;
	wl_list_for_each(o, &cow_wm.outputs, wm_link)
	{
		items[i].output = o;
		items[i].seq	= i;
		i++;
	}
	qsort(items, (size_t)n, sizeof(*items), renumber_cmp);
	for (i = 0; i < n; i++)
		items[i].output->number = i + 1;
	xfree(items);
}

int
cow_outputs_max_bottom(void)
{
	int		   max_bot = 0;
	struct cow_output *output;
	wl_list_for_each(output, &cow_wm.outputs, wm_link)
	{
		int bot = output->y + output->height;
		if (bot > max_bot)
			max_bot = bot;
	}
	return max_bot;
}

void
cow_output_update_current(void)
{
	/* TODO -- make this a setting?  To either say the current output is
	 * based on focused window, or pointer?
	 *
	 * For now, assume pointer, not focused window.
	 */

	if (!cow_wm.running)
		return;

	if (wl_list_empty(&cow_wm.seats))
		return;

	struct cow_seat *ps;
	wl_list_for_each(ps, &cow_wm.seats, wm_link)
	{
		if (ps->has_pointer_position) {
			struct cow_output *this = cow_output_at(ps->pointer_x,
			    ps->pointer_y);
			if (this != NULL && this != cow_wm.current_output)
				cow_wm.current_output = this;
		}
	}
}
