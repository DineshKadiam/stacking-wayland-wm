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
#include <stdlib.h>
#include <string.h>

#include "cow/image.h"
#include "cow/image_cache.h"

struct cow_image_cache_entry {
	char			     *path;
	int			      size;
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

	for (entry = cache->entries; entry != NULL; entry = entry->next) {
		if (entry->size == size && strcmp(entry->path, path) == 0)
			return entry;
	}

	entry = calloc(1, sizeof(*entry));
	if (entry == NULL)
		return NULL;
	entry->path = strdup(path);
	entry->pixels = calloc((size_t)size * size, sizeof(*entry->pixels));
	if (entry->path == NULL || entry->pixels == NULL) {
		free(entry->path);
		free(entry->pixels);
		free(entry);
		return NULL;
	}
	entry->size = size;
	pixbuf = (struct cow_pixbuf) {
		.pixels = entry->pixels,
		.stride_px = size,
		.width = size,
		.height = size,
	};
	if (!cow_image_render_rect(path, &pixbuf, 0, 0, size, size) &&
	    fallback != NULL && strcmp(path, fallback) != 0)
		(void)cow_image_render_rect(fallback, &pixbuf, 0, 0, size, size);
	entry->next = cache->entries;
	cache->entries = entry;
	return entry;
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
