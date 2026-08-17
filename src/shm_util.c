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
#include "cow/shm_util.h"

#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>

#include "cow/compat.h"

bool
cow_shm_argb8888_size(int width, int height, int *stride, size_t *size)
{
	if (width <= 0 || height <= 0 || stride == NULL || size == NULL ||
	    width > INT_MAX / 4)
		return false;

	int row_bytes = width * 4;
	if ((size_t)height > SIZE_MAX / (size_t)row_bytes)
		return false;
	*stride = row_bytes;
	*size = (size_t)row_bytes * (size_t)height;
	return true;
}

int
cow_shm_file_create(const char *name, size_t size)
{
	if (name == NULL || *name == '\0' || size == 0 ||
	    (size_t)(off_t)size != size)
		return -1;

#if HAVE_MEMFD_CREATE
	int fd = memfd_create(name, MFD_CLOEXEC);
#else
	char shm_name[128];
	if (snprintf(shm_name, sizeof(shm_name), "/%s-%d", name,
	    getpid()) >= (int)sizeof(shm_name))
		return -1;
	int fd = shm_open(shm_name, O_RDWR | O_CREAT | O_EXCL, 0600);
	if (fd >= 0)
		shm_unlink(shm_name);
#endif
	if (fd < 0)
		return -1;
	if (ftruncate(fd, (off_t)size) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}
