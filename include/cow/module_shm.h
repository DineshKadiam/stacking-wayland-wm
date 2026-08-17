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
#if !defined(COW_MODULE_SHM_H)
#define COW_MODULE_SHM_H

#include <stdbool.h>
#include <stddef.h>

#include <wayland-client.h>

struct cow_module_shm_buffer {
	struct wl_buffer *buffer;
	void             *data;
	size_t            size;
	int               width;
	int               height;
	int               stride;
	bool              busy;
};

bool cow_module_shm_buffer_create(struct cow_module_shm_buffer *,
    struct wl_shm *, const char *, int, int);
void cow_module_shm_buffer_destroy(struct cow_module_shm_buffer *);
struct cow_module_shm_buffer *cow_module_shm_buffer_next(
    struct cow_module_shm_buffer *, size_t, struct wl_shm *, const char *,
    int, int);
void cow_module_shm_buffers_destroy(struct cow_module_shm_buffer *, size_t);

#endif /* COW_MODULE_SHM_H */
