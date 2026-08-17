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
#include "cow/module_shm.h"

#include <limits.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "cow/compat.h"
#include "cow/shm_util.h"

static void
buffer_release(void *data, __unused struct wl_buffer *buffer)
{
	struct cow_module_shm_buffer *buf = data;

	buf->busy = false;
}

static const struct wl_buffer_listener buffer_listener = {
	.release = buffer_release,
};

bool
cow_module_shm_buffer_create(struct cow_module_shm_buffer *buf,
    struct wl_shm *shm, const char *name, int width, int height)
{
	if (buf == NULL || shm == NULL || name == NULL)
		return false;

	int stride;
	size_t size;
	if (!cow_shm_argb8888_size(width, height, &stride, &size) ||
	    size > INT_MAX)
		return false;

	int fd = cow_shm_file_create(name, size);
	if (fd < 0)
		return false;

	void *data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
	    0);
	if (data == MAP_FAILED) {
		close(fd);
		return false;
	}

	struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, (int)size);
	if (pool == NULL) {
		munmap(data, size);
		close(fd);
		return false;
	}
	struct wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0, width,
	    height, stride, WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);
	if (buffer == NULL) {
		munmap(data, size);
		return false;
	}

	*buf = (struct cow_module_shm_buffer) {
		.buffer = buffer,
		.data = data,
		.size = size,
		.width = width,
		.height = height,
		.stride = stride,
	};
	wl_buffer_add_listener(buffer, &buffer_listener, buf);
	return true;
}

void
cow_module_shm_buffer_destroy(struct cow_module_shm_buffer *buf)
{
	if (buf == NULL)
		return;
	if (buf->buffer != NULL)
		wl_buffer_destroy(buf->buffer);
	if (buf->data != NULL && buf->data != MAP_FAILED)
		munmap(buf->data, buf->size);
	memset(buf, 0, sizeof(*buf));
}

struct cow_module_shm_buffer *
cow_module_shm_buffer_next(struct cow_module_shm_buffer *buffers,
    size_t count, struct wl_shm *shm, const char *name, int width, int height)
{
	for (size_t i = 0; i < count; i++) {
		struct cow_module_shm_buffer *buf = &buffers[i];

		if (buf->busy)
			continue;
		if (buf->buffer != NULL && (buf->width != width ||
		    buf->height != height))
			cow_module_shm_buffer_destroy(buf);
		if (buf->buffer == NULL && !cow_module_shm_buffer_create(buf,
		    shm, name, width, height))
			return NULL;
		return buf;
	}
	return NULL;
}

void
cow_module_shm_buffers_destroy(struct cow_module_shm_buffer *buffers,
    size_t count)
{
	for (size_t i = 0; i < count; i++)
		cow_module_shm_buffer_destroy(&buffers[i]);
}
