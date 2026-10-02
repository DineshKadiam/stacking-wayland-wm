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
#include <string.h>
#include <unistd.h>

#include "cow/view.h"
#include "cow/config.h"
#include "cow/compat.h"
#include "cow/draw.h"
#include "cow/desk_ops.h"
#include "cow/font.h"
#include "cow/memory.h"
#include "cow/log.h"
#include "cow/output.h"
#include "cow/shm.h"
#include "cow/titlebar.h"
#include "cow/util.h"
#include "cow/update.h"
#include "cow/wm.h"
#include "river-window-management-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

/* Native toplevels share the ordinary WM input and decoration paths.
 * The outer frame is independent of its application's pane members.
 */
struct cow_container_window {
	struct cow_container	  *container;
	struct cow_container_pane *pane;
	struct cow_view		  *view;
	struct wl_surface	  *surface;
	struct xdg_surface	  *xdg;
	struct xdg_toplevel	  *toplevel;
	struct cow_shm_buffer	  *buffer;
	char			  *app_id, *title, *font;
	int			   width, height;
	int			   requested_width, requested_height;
	bool			   configured, dirty, placed, close, closing;
	bool			   highlighted;
};

static struct xdg_wm_base *window_base;
static void		   window_draw(struct cow_container_window *);

static void
window_ping(__unused void *data, struct xdg_wm_base *base, uint32_t serial)
{
	xdg_wm_base_pong(base, serial);
}

void
cow_container_window_bind(struct wl_registry *registry, uint32_t name)
{
	static const struct xdg_wm_base_listener listener = {
		.ping = window_ping
	};
	window_base = wl_registry_bind(registry, name, &xdg_wm_base_interface,
	    1);
	xdg_wm_base_add_listener(window_base, &listener, NULL);
}

void
cow_container_window_fini(void)
{
	cow_container_restore_clear();
	if (window_base)
		xdg_wm_base_destroy(window_base);
	window_base = NULL;
}

struct cow_container *
cow_container_window_owner(const struct cow_view *view)
{
	return view ? view->container_owner : NULL;
}

void
cow_container_set_frame(struct cow_container *c, struct cow_view *view)
{
	if (c->frame_view)
		c->frame_view->container_owner = NULL;
	c->frame_view = view;
	if (view)
		view->container_owner = c;
}

bool
cow_container_window_view(const struct cow_view *view)
{
	return view->container_placeholder ||
	    cow_container_window_owner(view) != NULL;
}

struct cow_container *
cow_container_group(const struct cow_view *view)
{
	struct cow_container *c = cow_view_container(view);
	return c ? c : cow_container_window_owner(view);
}

struct cow_container *
cow_container_top_group(const struct cow_view *view)
{
	struct cow_container *c = cow_container_group(view);
	while (c && c->frame_view && c->frame_view->container_member)
		c = cow_view_container(c->frame_view);
	return c;
}

struct cow_view *
cow_container_next_view(const struct cow_container *c,
    struct cow_view				   *previous)
{
	if (c == NULL)
		return NULL;
	struct wl_list *link = previous ? previous->wm_link.next :
					  cow_wm.views.next;
	for (; link != &cow_wm.views; link = link->next) {
		struct cow_view *view = wl_container_of(link, view, wm_link);
		if (cow_container_top_group(view) == c)
			return view;
	}
	return NULL;
}

static void
window_identify(struct cow_container_window *w, struct cow_view *view)
{
	if (w && view->app_id && strcmp(w->app_id, view->app_id) == 0) {
		w->view			    = view;
		view->container_placeholder = w->pane != NULL;
		if (!w->pane)
			cow_container_set_frame(w->container, view);
	}
}

void
cow_container_window_identify(struct cow_view *view)
{
	struct cow_container *c;
	wl_list_for_each(c, &cow_wm.containers, wm_link)
	{
		window_identify(c->frame_window, view);
		struct cow_container_pane *pane = NULL;
		while ((pane = cow_container_pane_next(c, pane)) != NULL)
			window_identify(pane->placeholder, view);
	}
}

void
cow_container_window_forget(struct cow_view *view)
{
	struct cow_container *c, *next;
	wl_list_for_each_safe(c, next, &cow_wm.containers, wm_link)
	{
		if (c->attached && c->frame_view == view) {
			struct cow_container_member *m;
			wl_list_for_each(m, &c->members, link)
			{
				if (m->pane->close_with_host &&
				    !m->view->closed)
					m->view->pending_close = true;
			}
			cow_container_destroy(c);
			cow_update_request(
			    COW_UPDATE_MANAGE | COW_UPDATE_STATUS);
			continue;
		}
		if (c->split_target == view)
			c->split_target = NULL;
		if (c->split_source == view)
			c->split_source = NULL;
		if (c->frame_view == view) {
			cow_container_set_frame(c, NULL);
			if (c->frame_window)
				c->frame_window->view = NULL;
		}
		struct cow_container_pane *pane = NULL;
		while ((pane = cow_container_pane_next(c, pane)) != NULL) {
			if (pane->placeholder &&
			    pane->placeholder->view == view)
				pane->placeholder->view = NULL;
		}
	}
}

static void
window_destroy(struct cow_container_window *w)
{
	if (!w)
		return;
	if (w->pane && w->view && w->view->container_member)
		cow_container_forget_view(w->view);
	if (w->toplevel)
		xdg_toplevel_destroy(w->toplevel);
	if (w->xdg)
		xdg_surface_destroy(w->xdg);
	if (w->surface)
		wl_surface_destroy(w->surface);
	cow_shm_buffer_destroy(w->buffer);
	xfree(w->app_id);
	xfree(w->title);
	xfree(w->font);
	xfree(w);
}

void
cow_container_window_destroy(struct cow_container *c)
{
	struct cow_container_window *w = c->frame_window;
	cow_container_set_frame(c, NULL);
	c->frame_window		       = NULL;
	window_destroy(w);
}

static void
window_configure(void *data, struct xdg_surface *surface, uint32_t serial)
{
	struct cow_container_window *w = data;
	xdg_surface_ack_configure(surface, serial);
	w->configured = true;
	w->dirty      = true;
	window_draw(w);
}

static void
window_size(void *data, __unused struct xdg_toplevel *toplevel, int32_t width,
    int32_t height, __unused struct wl_array *states)
{
	struct cow_container_window *w = data;
	if (width > 0)
		w->width = width;
	if (height > 0)
		w->height = height;
}

static void
window_close(void *data, __unused struct xdg_toplevel *toplevel)
{
	struct cow_container_window *w = data;
	w->close		       = true;
	w->closing		       = false;
	cow_update_request(COW_UPDATE_MANAGE);
}

static struct cow_container_window *
window_create(struct cow_container *c, struct cow_container_pane *pane)
{
	static const struct xdg_surface_listener surface_listener = {
		.configure = window_configure
	};
	static const struct xdg_toplevel_listener toplevel_listener = {
		.configure = window_size,
		.close	   = window_close
	};
	struct cow_container_window *w = xcalloc(1, sizeof(*w));
	w->container		       = c;
	w->pane			       = pane;
	w->width		       = 640;
	w->height		       = 480;
	w->title = xstrdup(pane ? (pane->name ? pane->name : "Empty pane") :
				  (c->name ? c->name : "Container"));
	xasprintf(&w->app_id, "cow-%s:%ld:%d:%d", pane ? "pane" : "container",
	    (long)getpid(), c->id, pane ? pane->id : 0);
	w->surface = wl_compositor_create_surface(cow_wm.compositor);
	if (!w->surface)
		goto fail;
	w->xdg = xdg_wm_base_get_xdg_surface(window_base, w->surface);
	if (!w->xdg)
		goto fail;
	xdg_surface_add_listener(w->xdg, &surface_listener, w);
	w->toplevel = xdg_surface_get_toplevel(w->xdg);
	if (!w->toplevel)
		goto fail;
	xdg_toplevel_add_listener(w->toplevel, &toplevel_listener, w);
	xdg_toplevel_set_app_id(w->toplevel, w->app_id);
	xdg_toplevel_set_title(w->toplevel, w->title);
	wl_surface_commit(w->surface);
	return w;

fail:
	cow_log_error("cannot create container window");
	window_destroy(w);
	return NULL;
}

static void
window_title(struct cow_container_window *w)
{
	const char *title = w->pane ?
	    (w->pane->name ? w->pane->name : "Empty pane") :
	    (w->container->name ? w->container->name : "Container");
	if (strcmp(title, w->title) != 0) {
		xstrset(&w->title, title);
		xdg_toplevel_set_title(w->toplevel, title);
	}
}

void
cow_container_window_geometry(struct cow_container *c)
{
	struct cow_view *v = c->frame_view;
	if (!v || !c->framed || c->syncing_frame || v->sd != COW_SHADE_NONE ||
	    v->fullscreen)
		return;
	int x	   = v->pending_resize ? v->pending_x : v->x;
	int y	   = v->pending_resize ? v->pending_y : v->y;
	int width  = v->pending_resize ? v->pending_width : v->content_width;
	int height = v->pending_resize ? v->pending_height : v->content_height;
	if (x != c->frame.x || y != c->frame.y || width != c->frame.width ||
	    height != c->frame.height)
		cow_view_set_pending_geometry(v, c->frame.x, c->frame.y,
		    c->frame.width, c->frame.height);
}

static bool
window_manage(struct cow_container *c)
{
	if (!c->frame_window)
		c->frame_window = window_create(c, NULL);
	struct cow_container_window *w = c->frame_window;
	if (!w)
		return false;
	if (w->close) {
		if (cow_container_window_count(c) == 0) {
			cow_container_destroy(c);
			cow_update_request(COW_UPDATE_STATUS);
			return false;
		}
		if (!w->closing) {
			struct cow_container_member *m;
			wl_list_for_each(m, &c->members, link)
			{
				if (!m->view->container_placeholder)
					m->view->pending_close = true;
			}
			w->closing = true;
		}
	}
	window_title(w);
	struct cow_view *view = c->frame_view;
	if (!view || !view->has_dimensions || view->closed)
		return false;
	if (c->split_axis != COW_CONTAINER_LEAF) {
		if (!cow_container_complete_tab_split(c)) {
			cow_container_destroy(c);
			return false;
		}
		c->split_axis = COW_CONTAINER_LEAF;
	}
	if (!w->placed) {
		w->placed = true;
		if (c->frame.width > 0 && c->frame.height > 0) {
			struct cow_desk *saved_desk = c->desk;
			view->flags		    = c->flags;
			struct cow_output *output;
			wl_list_for_each(output, &cow_wm.outputs, wm_link)
			{
				if (output != c->output || output->removed)
					continue;
				cow_view_reassign_output(view, output);
				struct cow_desk *desk;
				wl_list_for_each(desk, &output->desk_set.desks,
				    set_link)
				{
					if (desk == saved_desk) {
						cow_view_assign_desk(view,
						    desk->nr);
						break;
					}
				}
				break;
			}
			if (!c->framed) {
				int fw, fh, cx, cy;
				cow_titlebar_frame_geometry(view, 0, 0, &fw,
				    &fh, &cx, &cy);
				c->frame.x += cx;
				c->frame.y += cy;
				c->frame.width	= MAX(1, c->frame.width - fw);
				c->frame.height = MAX(1, c->frame.height - fh);
			}
			cow_view_set_pending_geometry(view, c->frame.x,
			    c->frame.y, c->frame.width, c->frame.height);
			if (c->active) {
				view->layer	= c->active->view->layer;
				view->raise_seq = c->active->view->raise_seq;
			}
		}
	}
	c->framed = true;
	c->output = view->output;
	c->desk	  = view->desk;
	c->flags  = view->flags;
	struct cow_container_member *m;
	wl_list_for_each(m, &c->members, link)
	{
		m->view->layer = view->layer;
	}
	if (view->sd != COW_SHADE_NONE || view->fullscreen)
		return true;
	/* Keep the requested layout while River waits for our configure ack.
	 */
	if (view->pending_resize) {
		w->requested_width  = view->pending_width;
		w->requested_height = view->pending_height;
	} else if (w->requested_width) {
		if (view->content_width != w->requested_width ||
		    view->content_height != w->requested_height)
			return true;
		w->requested_width = w->requested_height = 0;
	}
	struct cow_box frame = { view->pending_resize ? view->pending_x :
							view->x,
		view->pending_resize ? view->pending_y : view->y,
		view->pending_resize ? view->pending_width :
				       view->content_width,
		view->pending_resize ? view->pending_height :
				       view->content_height };
	int min_width, min_height;
	if (cow_container_layout_minimum(&c->root->layout, 0, &min_width,
		&min_height)) {
		view->min_width	 = min_width;
		view->min_height = min_height;
		if (frame.width < min_width || frame.height < min_height) {
			frame.width  = MAX(frame.width, min_width);
			frame.height = MAX(frame.height, min_height);
			cow_view_set_pending_geometry(view, frame.x, frame.y,
			    frame.width, frame.height);
		}
	}
	if (memcmp(&frame, &c->frame, sizeof(frame)) != 0 ||
	    memcmp(&frame, &c->root->layout.box, sizeof(frame)) != 0) {
		c->syncing_frame = true;
		bool arranged	 = cow_container_arrange(c, frame);
		c->syncing_frame = false;
		if (!arranged)
			cow_container_window_geometry(c);
	}
	struct cow_container_frame_state *saved = &c->frame_state;
	if (saved->pending && !view->pending_resize &&
	    !view->deferred_position && view->content_width == c->frame.width &&
	    view->content_height == c->frame.height) {
		saved->pending = false;
		view->flags    = saved->flags;
		cow_container_sync_flags(view,
		    COW_VIEW_STICKY_FLAG | COW_VIEW_ICONIFIED_FLAG);
		cow_view_set_layer(view, saved->layer);
		cow_view_set_maximized(view,
		    saved->maximized_h || saved->maximized_v);
		view->maximized_h = saved->maximized_h;
		view->maximized_v = saved->maximized_v;
		view->maximized	  = saved->maximized_h || saved->maximized_v;
		view->maximized_h_ignore_reserved = saved->ignore_h;
		view->maximized_v_ignore_reserved = saved->ignore_v;
		view->maximized_h_keep_geometry	  = saved->keep_h;
		view->maximized_v_keep_geometry	  = saved->keep_v;
		view->saved_x			  = saved->restore.x;
		view->saved_y			  = saved->restore.y;
		view->saved_width		  = saved->restore.width;
		view->saved_height		  = saved->restore.height;
		cow_view_set_pending_maximized_hint(view, view->maximized);
		if (saved->shade != COW_SHADE_NONE)
			cow_view_shade(view, saved->shade);
		view->dec.dirty = true;
		cow_update_request(COW_UPDATE_MANAGE | COW_UPDATE_STATUS);
	}
	return true;
}

void
cow_container_manage_windows(void)
{
	if (!window_base)
		return;
	struct cow_container *c, *next;
	wl_list_for_each_safe(c, next, &cow_wm.containers, wm_link)
	{
		if (c->attached) {
			cow_container_attachment_sync(c);
			continue;
		}
		if (!c->persistent && !c->framed && !c->frame_window &&
		    c->root->layout.axis == COW_CONTAINER_LEAF)
			continue;
		if (!window_manage(c))
			continue;
		struct cow_container_pane *pane = NULL;
		while ((pane = cow_container_pane_next(c, pane)) != NULL) {
			if (!c->persistent ||
			    cow_container_pane_window_count(pane))
				continue;
			if (!pane->placeholder)
				pane->placeholder = window_create(c, pane);
			struct cow_container_window *w = pane->placeholder;
			if (!w)
				continue;
			if (w->close) {
				cow_container_remove_empty_pane(c, pane);
				break;
			}
			window_title(w);
			if (w->view && w->view->has_dimensions &&
			    !w->view->closed && !w->view->container_member)
				cow_container_add_placeholder(c, pane, w->view);
		}
	}
	cow_container_restore_pending();
}

static bool
empty_box(struct cow_container *c, struct cow_container_pane *pane,
    struct cow_box *box)
{
	if (!c->persistent || cow_container_pane_window_count(pane) ||
	    !c->output || !c->desk || c->shaded)
		return false;
	struct cow_view *owner = c->frame_view ? c->frame_view :
	    c->active			       ? c->active->view :
						 NULL;
	if (owner) {
		if (owner->closed || owner->fullscreen ||
		    owner->sd != COW_SHADE_NONE ||
		    !cow_view_visible(owner, owner->output))
			return false;
	} else if (c->desk != c->output->current_desk &&
	    !(c->flags & COW_VIEW_STICKY_DESK_FLAG) &&
	    !(c->desk->nr < 32 &&
		(c->output->collected_desks & (1u << c->desk->nr))))
		return false;
	*box		      = pane->layout.box;
	struct cow_box output = { c->output->x, c->output->y, c->output->width,
		c->output->height };
	cow_box_intersection(box, &output, box);
	return box->width > 0 && box->height > 0;
}

static bool
view_above(struct cow_view *a, struct cow_view *b)
{
	return a &&
	    (!b || cow_view_effective_layer(a) > cow_view_effective_layer(b) ||
		(cow_view_effective_layer(a) == cow_view_effective_layer(b) &&
		    a->raise_seq > b->raise_seq));
}

static struct cow_view *
drop_target_at(int x, int y, struct cow_view *source, bool internal)
{
	if (!source || source->container_placeholder || source->parent)
		return NULL;
	struct cow_container *group = cow_container_top_group(source);
	struct cow_view	     *view, *best = NULL;
	wl_list_for_each(view, &cow_wm.views, wm_link)
	{
		if (view->closed || view == source ||
		    (!internal && view->container_placeholder) ||
		    view->parent || view->fullscreen || !view->output ||
		    !cow_view_visible(view, view->output))
			continue;
		if (internal) {
			if (!cow_view_container(source) ||
			    cow_view_container(view) !=
				cow_view_container(source) ||
			    cow_view_pane(view) == cow_view_pane(source))
				continue;
		} else if ((group && cow_container_top_group(view) == group) ||
		    !cow_container_can_tab(cow_view_container(view), source)) {
			continue;
		}
		/* The outer decorations join whole layouts as tabs. Inside the
		 * frame, the panes own drops, including their split-edge zones.
		 */
		struct cow_box content = { view->x, view->y,
			view->content_width, view->content_height };
		if (cow_container_window_owner(view) &&
		    cow_box_contains_point(&content, x, y))
			continue;
		if (cow_view_contains_point(view, x - view->x, y - view->y) &&
		    view_above(view, best))
			best = view;
	}
	return best;
}

struct cow_view *
cow_container_drop_target_at(int x, int y, struct cow_view *source)
{
	return drop_target_at(x, y, source, false);
}

struct cow_view *
cow_container_pane_drop_target_at(int x, int y, struct cow_view *source)
{
	return drop_target_at(x, y, source, true);
}

struct cow_container *
cow_container_empty_at(int x, int y, struct cow_view *source,
    struct cow_container_pane **result)
{
	struct cow_container *best = NULL, *c;
	*result			   = NULL;
	wl_list_for_each(c, &cow_wm.containers, wm_link)
	{
		if (source && c->frame_view == source)
			continue;
		struct cow_container_pane *pane = NULL;
		while ((pane = cow_container_pane_next(c, pane)) != NULL) {
			struct cow_box box;
			if (!empty_box(c, pane, &box) ||
			    !cow_box_contains_point(&box, x, y))
				continue;
			struct cow_view *owner = c->frame_view ? c->frame_view :
			    c->active ? c->active->view :
					NULL;
			bool		 obscured = false;
			struct cow_view *v;
			wl_list_for_each(v, &cow_wm.views, wm_link)
			{
				if (v == source ||
				    cow_container_group(v) == c || v->closed ||
				    !v->output ||
				    !cow_view_visible(v, v->output))
					continue;
				if (cow_view_contains_point(v, x - v->x,
					y - v->y) &&
				    (owner == NULL || view_above(v, owner)))
					obscured = true;
			}
			struct cow_view *previous = best ?
			    (best->frame_view	 ? best->frame_view :
				    best->active ? best->active->view :
						   NULL) :
			    NULL;
			if (!obscured &&
			    (!best || view_above(owner, previous) ||
				(!owner && !previous && c->id > best->id))) {
				best	= c;
				*result = pane;
			}
		}
	}
	return best;
}

void
cow_container_placeholder_destroy(struct cow_container_pane *pane)
{
	struct cow_container_window *w = pane->placeholder;
	pane->placeholder = NULL;
	window_destroy(w);
}

static void
window_draw(struct cow_container_window *w)
{
	if (!w || !w->configured)
		return;
	const char *font = w->view ? w->view->cfg.titlebar.font_name :
				     cow_config->titlebar.font_name;
	if (!font)
		font = cow_config->font_name;
	if (!font)
		font = "sans-serif 10";
	bool highlighted = w->pane && w->pane == cow_wm.drag_drop_pane;
	if (!w->dirty && w->highlighted == highlighted && w->font &&
	    strcmp(font, w->font) == 0)
		return;
	struct cow_shm_buffer *buffer = w->buffer;
	if (!buffer || buffer->busy || buffer->width != w->width ||
	    buffer->height != w->height)
		buffer = cow_shm_buffer_create(w->width, w->height);
	if (!buffer)
		return;
	struct cow_pixbuf pixels = { buffer->data, buffer->stride / 4, w->width,
		w->height };
	const float	  bg[4]	 = { 0.82f, 0.82f, 0.82f, 1.0f };
	const float	  fg[4]	 = { 0.2f, 0.2f, 0.2f, 1.0f };
	const float	 *colour = highlighted ?
		 COW_COLOUR_DROP_ORANGE :
		 bg;
	cow_draw_fill_rect(&pixels, 0, 0, w->width, w->height,
	    cow_colour_to_pixel(colour));
	if (w->pane) {
		struct cow_font face;
		cow_font_init(&face, font);
		cow_font_render_label(&pixels, 0, 0, w->width, w->height, &face,
		    "Drop a window here", colour, fg, 1, PANGO_ALIGN_CENTER,
		    NULL);
		cow_font_fini(&face);
	}
	if (buffer != w->buffer)
		cow_shm_buffer_destroy(w->buffer);
	w->buffer    = buffer;
	w->dirty     = false;
	w->highlighted = highlighted;
	xstrset(&w->font, font);
	buffer->busy = true;
	wl_surface_attach(w->surface, buffer->buffer, 0, 0);
	wl_surface_damage_buffer(w->surface, 0, 0, w->width, w->height);
	wl_surface_commit(w->surface);
}

void
cow_container_render_windows(void)
{
	struct cow_container *c;
	wl_list_for_each(c, &cow_wm.containers, wm_link)
	{
		window_draw(c->frame_window);
		struct cow_container_pane *pane = NULL;
		while ((pane = cow_container_pane_next(c, pane)) != NULL)
			window_draw(pane->placeholder);
	}
}
