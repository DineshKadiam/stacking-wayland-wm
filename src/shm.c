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
#include "cow/shm.h"
#include "cow/log.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "cow/compat.h"
#include "cow/memory.h"
#include "cow/shm_util.h"
#include "cow/wm.h"

static void
buffer_handle_release(void *data, __unused struct wl_buffer *buffer)
{
	struct cow_shm_buffer *buf = data;
	buf->busy		   = false;
}

static const struct wl_buffer_listener buffer_listener = {
	.release = buffer_handle_release,
};

struct cow_shm_buffer *
cow_shm_buffer_create(int width, int height)
{
	if (cow_wm.shm == NULL)
		return NULL;
	int stride;
	size_t size;
	if (!cow_shm_argb8888_size(width, height, &stride, &size) ||
	    size > INT_MAX)
		return NULL;

	int fd = cow_shm_file_create("cow-shm", size);
	if (fd < 0) {
		cow_log_error("failed to create shm file: %s", strerror(errno));
		return NULL;
	}

	void *data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
	    0);
	if (data == MAP_FAILED) {
		cow_log_error("mmap failed: %s", strerror(errno));
		close(fd);
		return NULL;
	}

	struct wl_shm_pool *pool = wl_shm_create_pool(cow_wm.shm, fd, size);
	if (pool == NULL) {
		munmap(data, size);
		close(fd);
		return NULL;
	}
	struct wl_buffer   *buffer = wl_shm_pool_create_buffer(pool, 0, width,
	      height, stride, WL_SHM_FORMAT_ARGB8888);

	/* We can destroy the pool immediately -- the buffer keeps a reference
	 */
	wl_shm_pool_destroy(pool);
	if (buffer == NULL) {
		munmap(data, size);
		close(fd);
		return NULL;
	}

	struct cow_shm_buffer *buf = xcalloc(1, sizeof(*buf));

	buf->buffer = buffer;
	buf->pool   = NULL; /* already destroyed */
	buf->data   = data;
	buf->width  = width;
	buf->height = height;
	buf->stride = stride;
	buf->fd	    = fd;
	buf->size   = size;
	buf->busy   = false;

	wl_buffer_add_listener(buffer, &buffer_listener, buf);

	return buf;
}

void
cow_shm_buffer_destroy(struct cow_shm_buffer *buf)
{
	if (buf == NULL)
		return;

	if (buf->buffer != NULL)
		wl_buffer_destroy(buf->buffer);
	if (buf->data != NULL)
		munmap(buf->data, buf->size);
	if (buf->fd >= 0)
		close(buf->fd);
	xfree(buf);
}
