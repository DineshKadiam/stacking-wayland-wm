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

#include <stdint.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "cow/image.h"
#include "cow/image_cache.h"

struct cow_image_cache_entry {
	char			     *path;
	int			      size;
	int                           width, height;
	uint32_t		     *pixels;
	struct cow_image_cache_entry *next;
};

void
cow_image_cache_clear(struct cow_image_cache *cache)
{
	while (cache->entries != NULL) {
		struct cow_image_cache_entry *entry = cache->entries;

		cache->entries = entry->next;
		free(entry->path);
		free(entry->pixels);
		free(entry);
	}
}

static struct cow_image_cache_entry *
cache_get(struct cow_image_cache *cache, const char *path,
    const char *fallback, int size)
{
	struct cow_image_cache_entry *entry;
	struct cow_pixbuf	      pixbuf;
	int width = size, height = size;

	for (entry = cache->entries; entry != NULL; entry = entry->next) {
		if (entry->size == size && strcmp(entry->path, path) == 0)
			return entry;
	}
	/* A zero size requests the native tile rather than an icon thumbnail.
	 * Bound retained wallpaper memory; a single large image can exceed the
	 * budget, but is evicted before another image is loaded. */
	if (size == 0) {
		if (!cow_image_dimensions(path, &width, &height) || width <= 0 ||
		    height <= 0 || width > INT_MAX / 4 ||
		    (size_t)width > SIZE_MAX / sizeof(uint32_t) / height)
			return NULL;
		size_t bytes = 0;
		for (entry = cache->entries; entry != NULL; entry = entry->next)
			bytes += (size_t)entry->width * entry->height * sizeof(uint32_t);
		if (bytes >= 64 * 1024 * 1024)
			cow_image_cache_clear(cache);
	}

	entry = calloc(1, sizeof(*entry));
	if (entry == NULL)
		return NULL;
	entry->path = strdup(path);
	entry->pixels = calloc((size_t)width * height, sizeof(*entry->pixels));
	if (entry->path == NULL || entry->pixels == NULL) {
		free(entry->path);
		free(entry->pixels);
		free(entry);
		return NULL;
	}
	entry->size = size;
	entry->width = width;
	entry->height = height;
	pixbuf = (struct cow_pixbuf) {
		.pixels = entry->pixels,
		.stride_px = width,
		.width = width,
		.height = height,
	};
	if (!cow_image_render_rect(path, &pixbuf, 0, 0, width, height) &&
	    fallback != NULL && strcmp(path, fallback) != 0)
		(void)cow_image_render_rect(fallback, &pixbuf, 0, 0, size, size);
	entry->next = cache->entries;
	cache->entries = entry;
	return entry;
}

/* Scale the tiled page as a whole, not each tile to the page's size. */
void
cow_image_cache_tile(struct cow_image_cache *cache, const char *path,
    cairo_t *cr, double x, double y, double width, double height,
    double scale_x, double scale_y)
{
	if (path == NULL || strcmp(path, "none") == 0 ||
	    width <= 0 || height <= 0 || scale_x <= 0 || scale_y <= 0)
		return;
	struct cow_image_cache_entry *entry = cache_get(cache, path, NULL, 0);
	if (entry == NULL)
		return;
	cairo_surface_t *surface = cairo_image_surface_create_for_data(
	    (unsigned char *)entry->pixels, CAIRO_FORMAT_ARGB32,
	    entry->width, entry->height, entry->width * 4);
	cairo_save(cr);
	cairo_rectangle(cr, x, y, width, height);
	cairo_clip(cr);
	cairo_translate(cr, x, y);
	cairo_scale(cr, scale_x, scale_y);
	cairo_set_source_surface(cr, surface, 0, 0);
	cairo_pattern_set_extend(cairo_get_source(cr), CAIRO_EXTEND_REPEAT);
	cairo_paint(cr);
	cairo_restore(cr);
	cairo_surface_destroy(surface);
}

bool
cow_image_cache_blit(struct cow_image_cache *cache, const char *path,
    const char *fallback, struct cow_pixbuf *dst, int x, int y, int size)
{
	struct cow_image_cache_entry *entry;

	if (cache == NULL || path == NULL || path[0] == '\0' || dst == NULL ||
	    size < 1)
		return false;
	entry = cache_get(cache, path, fallback, size);
	if (entry == NULL)
		return false;

	for (int sy = 0; sy < size; sy++) {
		int dy = y + sy;

		if (dy < 0 || dy >= dst->height)
			continue;
		for (int sx = 0; sx < size; sx++) {
			int	  dx = x + sx;
			uint32_t  src, alpha, *target, old, inverse, a, r, g, b;

			if (dx < 0 || dx >= dst->width)
				continue;
			src = entry->pixels[sy * size + sx];
			alpha = src >> 24;
			if (alpha == 0)
				continue;
			target = &dst->pixels[dy * dst->stride_px + dx];
			old = *target;
			inverse = 255 - alpha;
			a = alpha + ((old >> 24) & 0xff) * inverse / 255;
			r = ((src >> 16) & 0xff) +
			    ((old >> 16) & 0xff) * inverse / 255;
			g = ((src >> 8) & 0xff) +
			    ((old >> 8) & 0xff) * inverse / 255;
			b = (src & 0xff) + (old & 0xff) * inverse / 255;
			*target = (a << 24) | (r << 16) | (g << 8) | b;
		}
	}
	return true;
}
