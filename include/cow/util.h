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
#if !defined(COW_UTIL_H)
#define COW_UTIL_H

#include <stdbool.h>
#include <stddef.h>

/* Positive alignment; callers must check addition overflow first. */
#define ALIGN_UP(value, alignment) \
	(((value) + (alignment) - 1) / (alignment) * (alignment))

struct cow_string_choice {
	const char *name;
	int         value;
};

/* Look up an exact string in a NULL-terminated choice table. */
bool cow_string_choice_parse(const struct cow_string_choice *, const char *,
    int *);

/*
 * Small project-wide utilities used by multiple modules.
 */

/*
 * Monotonic clock in milliseconds.  Used by code that compares timestamps
 * across manage cycles (dwell timers, animation deadlines, etc.).
 */
long cow_mono_ms(void);

/*
 * Build the default per-display socket path inside $XDG_RUNTIME_DIR.
 * If WAYLAND_DISPLAY is set, its basename is folded into the socket
 * name so independent CoW instances in the same runtime directory do
 * not steal each other's IPC/status sockets.
 */
bool cow_runtime_socket_path(char *, size_t, const char *, const char *);

/*
 * Resolve a path against an optional base directory.  Leading ~/,
 * $HOME/, and ${HOME}/ forms are expanded.  The returned string is owned by
 * the caller; NULL indicates that HOME was required but unavailable, or an
 * allocation failure.
 */
char *cow_path_resolve(const char *, const char *);

/* Return an allocated directory component for a path. */
char *cow_path_dirname(const char *);

/* Quote and join argv so cmd_string_parse() recovers the same arguments. */
char *cow_command_from_argv(size_t, const char *const *);

/* Escape text for use in XML/Pango markup. The caller owns the result. */
char *cow_markup_escape(const char *);

/*
 * Create, bind, and listen on an AF_UNIX SOCK_STREAM socket inside
 * $XDG_RUNTIME_DIR.
 *
 * On success returns a non-blocking listening fd and writes the path
 * actually used into 'path_out' (NUL-terminated).  Returns -1 on any
 * failure; an error string is printed to stderr in that case.
 *
 *   name     socket role used in the default filename (e.g. "cmd")
 *   path_out caller-supplied buffer to receive the full path on success
 *   path_cap size of path_out in bytes; must be >= sizeof(sockaddr_un.sun_path)
 *   backlog  listen() backlog (e.g. 16)
 */
int cow_unix_listen(const char *, char *, size_t, int);

/*
 * Convenience macro: if 'p' is non-NULL, call 'fn(p)' and set p = NULL.
 * Used to keep teardown sequences symmetric and idempotent.
 */
#define COW_DESTROY_AND_NULL(p, fn) \
	do {                        \
		if ((p) != NULL) {  \
			fn((p));    \
			(p) = NULL; \
		}                   \
	} while (0)

#endif /* COW_UTIL_H */
