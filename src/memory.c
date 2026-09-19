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
/*
 * Memory wrappers and utility functions.
 *
 * The x*() family is derived from lswm/tmux.
 */

#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cow/compat.h"
#include "cow/memory.h"

void
xfree(void *ptr)
{
	free(ptr);
}

/* ================================================================
 * x*() wrappers -- fatal on allocation failure.
 * ================================================================ */

void
log_fatal(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	fprintf(stderr, "cow: fatal: ");
	vfprintf(stderr, fmt, ap);
	fprintf(stderr, "\n");
	va_end(ap);
	abort();
}

int
xasprintf(char **out, const char *fmt, ...)
{
	va_list ap;
	int	i;

	va_start(ap, fmt);
	i = vasprintf(out, fmt, ap);
	va_end(ap);

	if (i == -1)
		log_fatal("xasprintf: %s", strerror(errno));

	return (i);
}

void *
xmalloc(size_t s)
{
	void *mem;

	if (s == 0)
		log_fatal("xmalloc: zero size");
	if ((mem = malloc(s)) == NULL)
		log_fatal("xmalloc: %s", strerror(errno));
	return (mem);
}

void *
xcalloc(size_t nmemb, size_t size)
{
	void *ptr;

	if (size == 0 || nmemb == 0)
		log_fatal("xcalloc: zero size");
	if (SIZE_MAX / nmemb < size)
		log_fatal("xcalloc: overflow");
	if ((ptr = calloc(nmemb, size)) == NULL)
		log_fatal("xcalloc: %s", strerror(errno));
	return (ptr);
}

char *
xstrdup(const char *s)
{
	char  *ptr;
	size_t len;

	len = strlen(s) + 1;
	ptr = xmalloc(len);
	strlcpy(ptr, s, len);
	return (ptr);
}

char *
xstrdup0(const char *s)
{
	return s != NULL ? xstrdup(s) : NULL;
}

void
xstrset(char **dst, const char *src)
{
	char *copy = xstrdup0(src);

	xfree(*dst);
	*dst = copy;
}

void *
xrealloc(void *oldptr, size_t nmemb, size_t size)
{
	void *newptr;

	if (size == 0 || nmemb == 0)
		log_fatal("xrealloc: zero size");
	if (SIZE_MAX / nmemb < size)
		log_fatal("xrealloc: overflow");
	if ((newptr = realloc(oldptr, nmemb * size)) == NULL)
		log_fatal("xrealloc: %s", strerror(errno));
	return (newptr);
}
