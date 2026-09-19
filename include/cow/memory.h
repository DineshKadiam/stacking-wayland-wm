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
#if !defined(COW_MEMORY_H)
#define COW_MEMORY_H

#include <stdlib.h>
#include <sys/types.h>

/*
 * All allocation wrappers abort on failure.
 * Prototypes are also in cmd.h for the command subsystem.
 */

int   xasprintf(char **, const char *, ...);
void *xmalloc(size_t);
void *xcalloc(size_t, size_t);
char *xstrdup(const char *);
char *xstrdup0(const char *);
void  xstrset(char **, const char *);
void *xrealloc(void *, size_t, size_t);
void  xfree(void *);

void log_fatal(const char *, ...) __attribute__((noreturn));

#endif
