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
#if !defined(COW_SHM_H)
#define COW_SHM_H

#include <stdbool.h>
#include <stdint.h>
#include <wayland-client.h>

/*
 * A shared-memory buffer suitable for use as a wl_surface attachment.
 *
 * The buffer owns a wl_shm_pool (mmap'd file) and a wl_buffer.
 * The data pointer can be written to directly. After writing, call
 * wl_surface_attach + wl_surface_damage_buffer + wl_surface_commit.
 *
 * The buffer tracks whether it is currently "busy" (the compositor
 * still holds a reference from the last commit). If busy, a new
 * buffer must be allocated instead of reusing this one.
 */
struct cow_shm_buffer {
	struct wl_buffer   *buffer;
	struct wl_shm_pool *pool;
	uint32_t	   *data;
	int		    width, height;
	int		    stride; /* in bytes */
	int		    fd;
	size_t		    size;
	bool		    busy; /* true while compositor holds a reference */
};

/*
 * Allocate an ARGB8888 shm buffer of the given dimensions.
 * Returns NULL on failure.
 */
struct cow_shm_buffer *cow_shm_buffer_create(int, int);

/*
 * Free the shm buffer and all associated resources.
 */
void cow_shm_buffer_destroy(struct cow_shm_buffer *);

#endif
