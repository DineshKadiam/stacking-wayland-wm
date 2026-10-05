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

#include "cow/border.h"
#include "cow/container.h"
#include "cow/decoration.h"
#include "cow/memory.h"
#include "cow/shm.h"
#include "cow/titlebar.h"
#include "cow/view.h"
#include "cow/wm.h"
#include "river-window-management-v1-client-protocol.h"
#include "viewporter-client-protocol.h"

/* Immutable buffers can be attached to several surfaces while busy. */
struct border_tiles {
	struct border_tiles	*next;
	struct cow_border_colour colours;
	int			 border, corner, style, refs;
	struct cow_shm_buffer	*buffer;
};

struct border_piece {
	struct wl_surface	   *surface;
	struct river_decoration_v1 *decoration;
	struct wp_viewport	   *viewport;
	int			    x, y;
	bool attached;
};

struct cow_decoration_tiles {
	struct border_tiles *shared;
	struct border_piece  pieces[8];
	struct wp_viewport  *title;
};

static struct border_tiles *tile_cache;

static void
tiles_release(struct border_tiles *tiles)
{
	if (tiles == NULL || --tiles->refs != 0)
		return;
	struct border_tiles **link = &tile_cache;
	while (*link != tiles)
		link = &(*link)->next;
	*link = tiles->next;
	cow_shm_buffer_destroy(tiles->buffer);
	xfree(tiles);
}

void
cow_decoration_tiles_fini(struct cow_view_decoration *dec)
{
	struct cow_decoration_tiles *a = dec->tiles;
	if (a == NULL)
		return;
	for (size_t i = 0; i < 8; i++) {
		struct border_piece *p = &a->pieces[i];
		if (p->viewport != NULL)
			wp_viewport_destroy(p->viewport);
		if (p->decoration != NULL)
			river_decoration_v1_destroy(p->decoration);
		if (p->surface != NULL)
			wl_surface_destroy(p->surface);
	}
	if (a->title != NULL)
		wp_viewport_destroy(a->title);
	tiles_release(a->shared);
	xfree(a);
	dec->tiles = NULL;
}

bool
cow_decoration_tiles_prepare(struct cow_view *v, bool active, int w, int h)
{
	struct cow_titlebar_squeeze squeeze;
	cow_titlebar_effective_squeeze(v, &squeeze);
	int bw = v->dec.border_width;
	int cl = cow_decoration_corner_length(v);
	/*
	 * Keep both pixels of the handle relief, plus one guard pixel.
	 * Linear filtering of a stretched one-pixel edge can sample either
	 * neighbour; those neighbours must also contain the plain edge.
	 */
	int k = (cl > bw ? cl : bw) + 3;
	if (cow_wm.viewporter == NULL || bw <= 0 || k > 1024 || w <= 2 * k ||
	    h <= 2 * k || v->dec.title_thickness <= 0 ||
	    v->cfg.titlebar.position != COW_TITLEBAR_TOP ||
	    squeeze.mode != COW_SQUEEZE_NONE || cow_view_container(v) != NULL ||
	    v->sd != COW_SHADE_NONE || cow_view_is_drop_target(v) ||
	    v->resize_indicator_edges != 0)
		goto fallback;

	const struct cow_border_colour *bc = active ? &v->cfg.border_active :
						      &v->cfg.border_inactive;
	/* Canonicalise the solid-colour key; never retain config pointers. */
	struct cow_border_colour key;
	memset(&key, 0, sizeof(key));
#define SOLID_EDGE(suffix, colour)                                    \
	do {                                                          \
		if (bc->gradient##suffix.mode != COW_GRADIENT_NONE || \
		    bc->image##suffix != NULL)                        \
			goto fallback;                                \
		memcpy(key.colour, bc->colour, sizeof(key.colour));   \
	} while (0)
	if (bc->gradient.mode != COW_GRADIENT_NONE || bc->image != NULL)
		goto fallback;
	SOLID_EDGE(_n, n);
	SOLID_EDGE(_s, s);
	SOLID_EDGE(_e, e);
	SOLID_EDGE(_w, w);
	SOLID_EDGE(_nw, nw);
	SOLID_EDGE(_ne, ne);
	SOLID_EDGE(_sw, sw);
	SOLID_EDGE(_se, se);
#undef SOLID_EDGE
	key.has_corners = bc->has_corners;
	key.has_nw	= bc->has_nw;
	key.has_ne	= bc->has_ne;
	key.has_sw	= bc->has_sw;
	key.has_se	= bc->has_se;
	struct border_tiles *shared;
	for (shared = tile_cache; shared != NULL; shared = shared->next)
		if (shared->border == bw && shared->corner == cl &&
		    shared->style == v->cfg.border_style &&
		    memcmp(&shared->colours, &key, sizeof(key)) == 0)
			break;
	if (shared == NULL) {
		shared	       = xcalloc(1, sizeof(*shared));
		shared->buffer = cow_shm_buffer_create(2 * k + 1, 2 * k + 1);
		if (shared->buffer == NULL) {
			xfree(shared);
			goto fallback;
		}
		memcpy(&shared->colours, &key, sizeof(key));
		shared->border = bw;
		shared->corner = cl;
		shared->style  = v->cfg.border_style;
		cow_border_style_render(shared->buffer->data,
		    shared->buffer->stride / 4, 2 * k + 1, 2 * k + 1, &key, bw,
		    cl, shared->style);
		shared->next = tile_cache;
		tile_cache = shared;
	}
	if (v->dec.tiles == NULL)
		v->dec.tiles = xcalloc(1, sizeof(*v->dec.tiles));
	struct cow_decoration_tiles *a = v->dec.tiles;
	if (a->shared != shared) {
		shared->refs++;
		tiles_release(a->shared);
		a->shared = shared;
		for (size_t i = 0; i < 8; i++)
			a->pieces[i].attached = false;
	}
	for (size_t i = 0; i < 8; i++) {
		struct border_piece *p = &a->pieces[i];
		if (p->surface != NULL)
			continue;
		p->surface = wl_compositor_create_surface(cow_wm.compositor);
		if (p->surface == NULL)
			goto fallback;
		p->decoration = river_window_v1_get_decoration_below(
		    v->river_window, p->surface);
		p->viewport = wp_viewporter_get_viewport(cow_wm.viewporter,
		    p->surface);
		if (p->decoration == NULL || p->viewport == NULL)
			goto fallback;
	}
	if (v->decoration_surface == NULL)
		v->decoration_surface = wl_compositor_create_surface(
		    cow_wm.compositor);
	if (v->decoration_surface == NULL)
		goto fallback;
	if (a->title == NULL)
		a->title = wp_viewporter_get_viewport(cow_wm.viewporter,
		    v->decoration_surface);
	if (a->title == NULL)
		goto fallback;
	return true;

fallback:
	cow_decoration_tiles_fini(&v->dec);
	return false;
}

static void
piece_rectangles(int k, int bw, int w, int h, struct cow_box source[8],
    struct cow_box destination[8])
{
	/* Source tiles and destination frame rectangles: corners, then edges.
	 */
	const struct cow_box src[] = {
		{0, 0, k, k}, {k + 1, 0, k, k},
		{0, k + 1, k, k}, {k + 1, k + 1, k, k},
		{k, 0, 1, bw}, {k, 2 * k + 1 - bw, 1, bw},
		{0, k, bw, 1}, {2 * k + 1 - bw, k, bw, 1},
	};
	const struct cow_box dst[] = {
		{0, 0, k, k}, {w - k, 0, k, k},
		{0, h - k, k, k}, {w - k, h - k, k, k},
		{k, 0, w - 2 * k, bw}, {k, h - bw, w - 2 * k, bw},
		{0, k, bw, h - 2 * k}, {w - bw, k, bw, h - 2 * k},
	};
	memcpy(source, src, sizeof(src));
	memcpy(destination, dst, sizeof(dst));
}

void
cow_decoration_tiles_commit(struct cow_view *v, int cx, int cy)
{
	struct cow_decoration_tiles *a	    = v->dec.tiles;
	struct cow_shm_buffer	    *buffer = a->shared->buffer;
	int			     bw	    = v->dec.border_width;
	int	       w = v->dec.frame_width, h = v->dec.frame_height;
	struct cow_box src[8], dst[8];
	piece_rectangles(buffer->width / 2, bw, w, h, src, dst);
	for (size_t i = 0; i < 8; i++) {
		struct border_piece *p = &a->pieces[i];
		p->x = dst[i].x;
		p->y = dst[i].y;
		wp_viewport_set_source(p->viewport, wl_fixed_from_int(src[i].x),
		    wl_fixed_from_int(src[i].y),
		    wl_fixed_from_int(src[i].width),
		    wl_fixed_from_int(src[i].height));
		wp_viewport_set_destination(p->viewport, dst[i].width,
		    dst[i].height);
		river_decoration_v1_set_offset(p->decoration, p->x - cx,
		    p->y - cy);
		/* Transparent corner interiors must not intercept client input.
		 */
		struct wl_region *r = wl_compositor_create_region(
		    cow_wm.compositor);
		if (r != NULL) {
			wl_region_add(r, -p->x, -p->y, w, bw);
			wl_region_add(r, -p->x, h - bw - p->y, w, bw);
			wl_region_add(r, -p->x, -p->y, bw, h);
			wl_region_add(r, w - bw - p->x, -p->y, bw, h);
			wl_surface_set_input_region(p->surface, r);
			wl_region_destroy(r);
		}
		/* Geometry changes do not change the immutable tile pixels. */
		if (!p->attached) {
			wl_surface_attach(p->surface, buffer->buffer, 0, 0);
			wl_surface_damage_buffer(p->surface, 0, 0, buffer->width,
			    buffer->height);
			p->attached = true;
		}
		river_decoration_v1_sync_next_commit(p->decoration);
		wl_surface_commit(p->surface);
	}
	wp_viewport_set_source(a->title, wl_fixed_from_int(bw),
	    wl_fixed_from_int(bw), wl_fixed_from_int(w - 2 * bw),
	    wl_fixed_from_int(v->dec.title_thickness));
	wp_viewport_set_destination(a->title, w - 2 * bw,
	    v->dec.title_thickness);
	river_decoration_v1_set_offset(v->decoration, bw - cx, bw - cy);
	wl_surface_set_input_region(v->decoration_surface, NULL);
}

void
cow_decoration_tiles_hide(struct cow_view *v)
{
	if (v->dec.tiles == NULL)
		return;
	for (size_t i = 0; i < 8; i++) {
		struct border_piece *p = &v->dec.tiles->pieces[i];
		if (p->surface == NULL)
			continue;
		wl_surface_attach(p->surface, NULL, 0, 0);
		p->attached = false;
		river_decoration_v1_sync_next_commit(p->decoration);
		wl_surface_commit(p->surface);
	}
}

bool
cow_decoration_surface_point(struct cow_view *v, struct wl_surface *surface,
    int *x, int *y)
{
	if (surface == NULL)
		return false;
	if (v->decoration_surface == surface) {
		if (v->dec.tiles != NULL) {
			*x += v->dec.border_width;
			*y += v->dec.border_width;
		}
		return true;
	}
	if (v->dec.tiles != NULL)
		for (size_t i = 0; i < 8; i++) {
			struct border_piece *p = &v->dec.tiles->pieces[i];
			if (p->surface == surface) {
				*x += p->x;
				*y += p->y;
				return true;
			}
		}
	return false;
}
