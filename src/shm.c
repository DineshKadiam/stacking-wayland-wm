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
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "cow/compat.h"
#include "cow/memory.h"
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

static int
create_shm_file(size_t size)
{
	int fd = -1;

#if HAVE_MEMFD_CREATE
	fd = memfd_create("cow-shm", MFD_CLOEXEC);
#else
	/* POSIX fallback for systems without memfd_create. */
	char name[64];
	snprintf(name, sizeof(name), "/cow-shm-%d", getpid());
	fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
	if (fd >= 0)
		shm_unlink(name);
#endif

	if (fd < 0) {
		cow_log_error("failed to create shm file: %s", strerror(errno));
		return -1;
	}

	if (ftruncate(fd, size) < 0) {
		cow_log_error("ftruncate failed: %s", strerror(errno));
		close(fd);
		return -1;
	}

	return fd;
}

struct cow_shm_buffer *
cow_shm_buffer_create(int width, int height)
{
	if (cow_wm.shm == NULL)
		return NULL;
	if (width <= 0 || height <= 0)
		return NULL;

	int    stride = width * 4; /* ARGB8888 = 4 bytes per pixel */
	size_t size   = (size_t)stride * height;

	int fd = create_shm_file(size);
	if (fd < 0)
		return NULL;

	void *data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
	    0);
	if (data == MAP_FAILED) {
		cow_log_error("mmap failed: %s", strerror(errno));
		close(fd);
		return NULL;
	}

	struct wl_shm_pool *pool   = wl_shm_create_pool(cow_wm.shm, fd, size);
	struct wl_buffer   *buffer = wl_shm_pool_create_buffer(pool, 0, width,
	      height, stride, WL_SHM_FORMAT_ARGB8888);

	/* We can destroy the pool immediately -- the buffer keeps a reference
	 */
	wl_shm_pool_destroy(pool);

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
