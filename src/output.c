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
#include "cow/draw.h"
#include "cow/image.h"
#include "cow/layer_shell.h"
#include "cow/memory.h"
#include "cow/shm.h"
#include "cow/util.h"
#include "cow/wm.h"
#include "cow/desk_ops.h"
#include "cow/pan_frames.h"
#include "cow/pick.h"
#include "cow/pointer_input.h"
#include "cow/seat.h"
#include "cow/view.h"

#include "river-window-management-v1-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

#define OUTPUT_PAN_SUPPRESS_MS 1000

static void
output_background_destroy_resources(struct cow_output *output)
{
	if (output == NULL)
		return;
	if (output->bg_buffer != NULL) {
		cow_shm_buffer_destroy(output->bg_buffer);
		output->bg_buffer = NULL;
	}
	if (output->bg_layer_surface != NULL) {
		zwlr_layer_surface_v1_destroy(output->bg_layer_surface);
		output->bg_layer_surface = NULL;
	}
	if (output->bg_surface != NULL) {
		wl_surface_destroy(output->bg_surface);
		output->bg_surface = NULL;
	}
	output->bg_configured = false;
	output->bg_dirty      = true;
}

static void
output_apply_reserved_area(struct cow_box *area)
{
	if (cow_config == NULL)
		return;

	int rt = cow_config->reserved.top;
	int rb = cow_config->reserved.bottom;
	int rl = cow_config->reserved.left;
	int rr = cow_config->reserved.right;

	if (rt == 0 && rb == 0 && rl == 0 && rr == 0)
		return;

	area->x += rl;
	area->y += rt;
	area->width -= rl + rr;
	area->height -= rt + rb;

	if (area->width < 100)
		area->width = 100;
	if (area->height < 100)
		area->height = 100;
}

bool
cow_output_recompute_usable_area(struct cow_output *output)
{
	if (output == NULL)
		return (false);

	struct cow_box old = output->usable_area;

	if (output->has_layer_usable_area) {
		output->usable_area = output->layer_usable_area;
	} else {
		output->usable_area.x	    = output->x;
		output->usable_area.y	    = output->y;
		output->usable_area.width  = output->width;
		output->usable_area.height = output->height;
	}

	output_apply_reserved_area(&output->usable_area);

	bool changed = (old.x != output->usable_area.x ||
	    old.y != output->usable_area.y ||
	    old.width != output->usable_area.width ||
	    old.height != output->usable_area.height);
	if (!changed)
		return (false);

	struct cow_view *view;
	wl_list_for_each(view, &cow_wm.views, wm_link)
	{
		if (view->output == output)
			cow_view_reflow_maximized(view);
	}

	return (true);
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
			if (entry->name != NULL && output->name == NULL) {
				output->name = xstrdup(entry->name);
				cow_output_name_known(output);
			}
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
	if (!output->has_layer_usable_area)
		(void)cow_output_recompute_usable_area(output);

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
	if (output->has_layer_usable_area) {
		output->layer_usable_area.x	   = output->x;
		output->layer_usable_area.y	   = output->y;
		output->layer_usable_area.width  = width;
		output->layer_usable_area.height = height;
	}

	/* Startup applies reserved area after initial output events settle. */
	if (!cow_wm.exec_pending)
		(void)cow_output_recompute_usable_area(output);
	output_background_destroy_resources(output);

	/* Startup creates pan frames once at the end of exec_pending. */
	if (!cow_wm.exec_pending)
		pan_frames_update_output(output);
}

static const struct river_output_v1_listener output_listener = {
	.removed    = output_handle_removed,
	.wl_output  = output_handle_wl_output,
	.position   = output_handle_position,
	.dimensions = output_handle_dimensions,
};

static void
background_layer_configure(void *data, struct zwlr_layer_surface_v1 *lsurf,
    uint32_t serial, __unused uint32_t width, __unused uint32_t height)
{
	struct cow_output *output = data;

	zwlr_layer_surface_v1_ack_configure(lsurf, serial);
	if (output == NULL)
		return;

	output->bg_configured = true;
	output->bg_dirty      = true;
	cow_wm.needs_manage_dirty = true;
}

static void
background_layer_closed(void *data,
    __unused struct zwlr_layer_surface_v1 *lsurf)
{
	struct cow_output *output = data;

	if (output != NULL)
		output->bg_configured = false;
}

static const struct zwlr_layer_surface_v1_listener background_layer_listener = {
	.configure = background_layer_configure,
	.closed	   = background_layer_closed,
};

static void output_adopt_orphan_views(struct cow_output *);
static void output_adopt_unnamed_orphans(struct cow_output *);
static void output_apply_configured_background(struct cow_output *);

static bool
output_has_background(const struct cow_output *output)
{
	if (output == NULL)
		return false;
	if (output->has_bg_colour ||
	    (cow_config != NULL && cow_config->has_output_bg_colour))
		return true;
	if (output->has_bg_image)
		return output->bg_image != NULL;
	if (cow_config == NULL)
		return false;
	return cow_config->has_output_bg_image &&
	    cow_config->output_bg_image != NULL;
}

struct cow_output *
cow_output_create(struct river_output_v1 *river_output)
{
	struct cow_output *output = xcalloc(1, sizeof(*output));

	cow_pan_suppress_for(OUTPUT_PAN_SUPPRESS_MS);

	output->river_output = river_output;
	output->removed	     = false;
	output->pan_edge     = -1;
	wl_list_init(&output->visible_views);

	cow_desk_set_init(&output->desk_set);
	cow_desk_set_populate_defaults(output);
	output->current_desk = cow_desk_first(output);

	river_output_v1_add_listener(river_output, &output_listener, output);

	wl_list_insert(&cow_wm.outputs, &output->wm_link);

	/* Set as current output if we don't have one yet. */
	if (cow_wm.current_output == NULL)
		cow_output_set_current(output);

	/* Register with layer shell for non_exclusive_area events */
	cow_layer_shell_init_output(output);

	/* Adopt unkeyed orphans before the output name is known. */
	output_adopt_unnamed_orphans(output);

	/* Tentative ordinal until position/dimensions arrive. */
	cow_outputs_renumber();

	/* Debounce bursty topology changes. */
	cow_wm.outputs_changed_deadline_ms = cow_mono_ms() +
	    COW_OUTPUTS_CHANGED_DEBOUNCE_MS;

	return output;
}

/* Find a surviving output while `dying` is still linked. */
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

/* Last-output session snapshot; desk_nr == -1 means none. */
static struct {
	int desk_nr;
	int page_vx;
	int page_vy;
} pending_session = { .desk_nr = -1 };

struct output_session {
	struct wl_list link;
	char	      *name;
	int	       desk_nr;
	int	       page_vx;
	int	       page_vy;
};

static struct wl_list output_sessions;
static bool	      output_sessions_init;

static void
output_sessions_ensure_init(void)
{
	if (output_sessions_init)
		return;
	wl_list_init(&output_sessions);
	output_sessions_init = true;
}

static struct output_session *
output_session_find(const char *name)
{
	struct output_session *session;

	output_sessions_ensure_init();
	wl_list_for_each(session, &output_sessions, link)
	{
		if (strcmp(session->name, name) == 0)
			return (session);
	}
	return (NULL);
}

static void
output_session_save(struct cow_output *output)
{
	if (output == NULL || output->name == NULL ||
	    output->current_desk == NULL)
		return;

	struct output_session *session = output_session_find(output->name);
	if (session == NULL) {
		session	      = xcalloc(1, sizeof(*session));
		session->name = xstrdup(output->name);
		wl_list_insert(&output_sessions, &session->link);
	}

	session->desk_nr = (int)output->current_desk->nr;
	session->page_vx = output->current_desk->page_vx;
	session->page_vy = output->current_desk->page_vy;
}

static void
output_session_restore(struct cow_output *output)
{
	if (output == NULL || output->name == NULL)
		return;

	struct output_session *session = output_session_find(output->name);
	if (session == NULL)
		return;

	struct cow_desk *desk = cow_desk_by_nr(output,
	    (uint8_t)session->desk_nr);
	if (desk != NULL) {
		output->current_desk = desk;
		desk->page_vx	     = session->page_vx;
		desk->page_vy	     = session->page_vy;
	}

	wl_list_remove(&session->link);
	xfree(session->name);
	xfree(session);
}

/* Migrate or orphan views from a disappearing output. */
static void
output_migrate_views(struct cow_output *dying, struct cow_output *target)
{
	bool	preserve = cow_config != NULL &&
		     cow_config->preserve_desk_on_output_loss;

	/* Last output going away: remember desk/page. */
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
		/* Preserve the original source output across cascaded removal. */
		if (view->orphan_output_name == NULL && dying != NULL &&
		    dying->name != NULL)
			view->orphan_output_name = xstrdup(dying->name);
		/* Preserve source desk/page across cascaded removal. */
		if (view->orphan_desk_nr < 0 && preserve &&
		    view->desk != NULL) {
			view->orphan_desk_nr = (int8_t)view->desk->nr;
			view->orphan_page_vx = view->desk->page_vx;
			view->orphan_page_vy = view->desk->page_vy;
		}

		if (target != NULL) {
			uint8_t old_nr = view->desk != NULL ?
			    view->desk->nr : 0;
			bool had_desk = view->desk != NULL;
			cow_view_reassign_output(view, target);
			if (preserve && had_desk)
				cow_view_assign_desk(view, old_nr);
		} else {
			wl_list_remove(&view->desk_views);
			wl_list_init(&view->desk_views);
			wl_list_remove(&view->output_views);
			wl_list_init(&view->output_views);
			view->output = NULL;
			view->desk   = NULL;
		}
	}
}

/* Adopt one orphan view. */
static void
adopt_one(struct cow_view *view, struct cow_output *adopter)
{
	cow_view_reassign_output(view, adopter);
	if (view->orphan_desk_nr >= 0) {
		cow_view_assign_desk(view, (uint8_t)view->orphan_desk_nr);
		if (view->desk != NULL) {
			view->desk->page_vx = view->orphan_page_vx;
			view->desk->page_vy = view->orphan_page_vy;
		}
	}
	cow_view_clear_orphan(view);
}

/* One-shot restore after every output disappeared. */
static void
restore_pending_session(struct cow_output *adopter)
{
	if (pending_session.desk_nr < 0)
		return;
	struct cow_desk *d = cow_desk_by_nr(adopter,
	    (uint8_t)pending_session.desk_nr);
	if (d != NULL) {
		adopter->current_desk = d;
		d->page_vx	      = pending_session.page_vx;
		d->page_vy	      = pending_session.page_vy;
	}
	pending_session.desk_nr = -1;
}

/* Adopt orphans with no remembered output name. */
static void
output_adopt_unnamed_orphans(struct cow_output *adopter)
{
	if (adopter == NULL || adopter->current_desk == NULL)
		return;
	restore_pending_session(adopter);

	struct cow_view *view;
	wl_list_for_each(view, &cow_wm.views, wm_link)
	{
		if (view->output != NULL || view->closed)
			continue;
		if (view->orphan_output_name != NULL)
			continue;
		adopt_one(view, adopter);
	}
}

/* Adopt orphans matching this output name. */
static void
output_adopt_orphan_views(struct cow_output *adopter)
{
	if (adopter == NULL || adopter->current_desk == NULL ||
	    adopter->name == NULL)
		return;
	restore_pending_session(adopter);

	struct cow_view *view;
	wl_list_for_each(view, &cow_wm.views, wm_link)
	{
		if (view->output != NULL || view->closed)
			continue;
		if (view->orphan_output_name == NULL)
			continue;
		if (strcmp(view->orphan_output_name, adopter->name) != 0)
			continue;
		adopt_one(view, adopter);
	}
}

void
cow_output_name_known(struct cow_output *output)
{
	output_apply_configured_background(output);
	output_session_restore(output);
	output_adopt_orphan_views(output);
}

static void
output_apply_configured_background(struct cow_output *output)
{
	if (output == NULL)
		return;

	output->has_bg_colour = false;
	if (output->name != NULL && cow_config_get_output_background(cow_config,
		output->name, output->bg_colour)) {
		output->has_bg_colour = true;
	} else {
		char number[32];
		snprintf(number, sizeof(number), "@%d", output->number);
		if (cow_config_get_output_background(cow_config, number,
			output->bg_colour))
			output->has_bg_colour = true;
	}

	const char *image = NULL;
	output->has_bg_image = false;
	if ((output->name != NULL && cow_config_get_output_background_image(
		 cow_config, output->name, &image))) {
		output->has_bg_image = true;
	} else {
		char number[32];
		snprintf(number, sizeof(number), "@%d", output->number);
		if (cow_config_get_output_background_image(cow_config, number,
			&image))
			output->has_bg_image = true;
	}
	xfree(output->bg_image);
	output->bg_image = output->has_bg_image && image != NULL ?
	    xstrdup(image) : NULL;

	if (!output_has_background(output)) {
		output_background_destroy_resources(output);
		return;
	}
	output->bg_dirty = true;
}

void
cow_output_destroy(struct cow_output *output)
{
	/* Cancel any in-flight pick session: its per-output overlay is
	 * about to be torn down anyway. */
	cow_pick_cancel_active();
	cow_pan_suppress_for(OUTPUT_PAN_SUPPRESS_MS);
	output_session_save(output);

	/* Detach views before freeing this output's desk set. */
	output_migrate_views(output, output_find_migration_target(output));

	cow_pointer_input_forget_output(output);
	pan_frames_destroy_output(output);
	cow_layer_shell_fini_output(output);
	wl_list_remove(&output->wm_link);

	if (cow_wm.current_output == output) {
		struct cow_output *next = NULL;
		if (!wl_list_empty(&cow_wm.outputs))
			next = wl_container_of(cow_wm.outputs.next, next,
			    wm_link);
		cow_output_set_current(next);
	}

	output_background_destroy_resources(output);

	cow_desk_set_fini(&output->desk_set);
	xfree(output->name);
	xfree(output->bg_image);
	river_output_v1_destroy(output->river_output);
	xfree(output);

	/* Numbering shifts when an output disappears. */
	cow_outputs_renumber();

	cow_wm.outputs_changed_deadline_ms = cow_mono_ms() +
	    COW_OUTPUTS_CHANGED_DEBOUNCE_MS;
}

void
cow_output_prepare_background(struct cow_output *output)
{
	if (!output_has_background(output)) {
		output_background_destroy_resources(output);
		return;
	}
	if (cow_wm.compositor == NULL || cow_wm.wl_layer_shell == NULL)
		return;
	if (output->width <= 0 || output->height <= 0)
		return;
	if (output->wl_output == NULL)
		return;

	/* Create wl_surface + layer-shell surface during manage sequence. */
	if (output->bg_surface == NULL) {
		output->bg_surface = wl_compositor_create_surface(
		    cow_wm.compositor);
		if (output->bg_surface == NULL)
			return;
	}

	if (output->bg_layer_surface == NULL) {
		output->bg_layer_surface =
		    zwlr_layer_shell_v1_get_layer_surface(
			cow_wm.wl_layer_shell, output->bg_surface,
			output->wl_output, ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND,
			"cow-background");
		if (output->bg_layer_surface == NULL)
			return;

		zwlr_layer_surface_v1_set_size(output->bg_layer_surface,
		    output->width, output->height);
		zwlr_layer_surface_v1_set_anchor(output->bg_layer_surface,
		    ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
			ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
			ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
			ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
		zwlr_layer_surface_v1_set_exclusive_zone(
		    output->bg_layer_surface, -1);
		zwlr_layer_surface_v1_set_keyboard_interactivity(
		    output->bg_layer_surface,
		    ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
		zwlr_layer_surface_v1_add_listener(output->bg_layer_surface,
		    &background_layer_listener, output);

		/* Initial commit requests the configure event.  Do not attach
		 * a buffer until the compositor has configured the layer
		 * surface. */
		wl_surface_commit(output->bg_surface);
	}
}

void
cow_output_render_background(struct cow_output *output)
{
	if (!output_has_background(output))
		return;
	if (output->bg_surface == NULL || output->bg_layer_surface == NULL ||
	    !output->bg_configured)
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

		const float *bg = NULL;
		if (output->has_bg_colour)
			bg = output->bg_colour;
		else if (cow_config != NULL && cow_config->has_output_bg_colour)
			bg = cow_config->output_bg_colour;

		const char *image = NULL;
		if (output->has_bg_image)
			image = output->bg_image;
		else if (cow_config != NULL && cow_config->has_output_bg_image)
			image = cow_config->output_bg_image;

		uint32_t px = bg != NULL ? cow_colour_to_pixel(bg) : 0;
		struct cow_pixbuf p = {
			.pixels = output->bg_buffer->data,
			.stride_px = output->bg_buffer->stride / 4,
			.width = output->width,
			.height = output->height,
		};
		cow_draw_fill_rect(&p, 0, 0, output->width, output->height,
		    px);
		if (image != NULL)
			cow_image_render_tiled(image, &p);
		output->bg_dirty = false;
	}

	if (output->bg_buffer == NULL)
		return;

	/* Commit the background layer surface. */
	wl_surface_attach(output->bg_surface, output->bg_buffer->buffer, 0, 0);
	wl_surface_damage_buffer(output->bg_surface, 0, 0, output->width,
	    output->height);
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

struct cow_output *
cow_output_find_by_spec(const char *spec)
{
	if (spec == NULL)
		return NULL;

	if (spec[0] == '@' && spec[1] >= '0' && spec[1] <= '9') {
		const char *errstr;
		long long   nr = strtonum(spec + 1, 1, INT_MAX, &errstr);
		if (errstr == NULL) {
			struct cow_output *output;
			wl_list_for_each(output, &cow_wm.outputs, wm_link)
			{
				if ((long long)output->number == nr)
					return output;
			}
		}
		return NULL;
	}

	return cow_output_find_by_name(spec);
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
	for (i = 0; i < n; i++) {
		items[i].output->number = i + 1;
		output_apply_configured_background(items[i].output);
	}
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
cow_output_set_current(struct cow_output *output)
{
	cow_wm.current_output = output;
	cow_layer_shell_set_default_output(output);
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
				cow_output_set_current(this);
		}
	}
}
