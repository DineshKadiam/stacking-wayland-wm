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

#include "cow/util.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

bool
cow_string_choice_parse(const struct cow_string_choice *choices,
    const char *value, int *result)
{
	if (choices == NULL || value == NULL || result == NULL)
		return (false);
	for (; choices->name != NULL; choices++) {
		if (strcmp(value, choices->name) == 0) {
			*result = choices->value;
			return (true);
		}
	}
	return (false);
}

static bool
command_arg_needs_quotes(const char *s)
{
	if (s == NULL || *s == '\0')
		return (true);
	for (; *s != '\0'; s++) {
		unsigned char ch = (unsigned char)*s;
		if (isspace(ch) || ch == '"' || ch == '\'' || ch == '\\' ||
		    ch == '#')
			return (true);
	}
	return (false);
}

static size_t
command_arg_quoted_len(const char *s)
{
	if (!command_arg_needs_quotes(s))
		return (strlen(s));

	size_t len = 2;
	for (; *s != '\0'; s++) {
		switch (*s) {
		case '\\':
		case '"':
		case '\n':
		case '\r':
		case '\t':
			len += 2;
			break;
		default:
			len++;
			break;
		}
	}
	return (len);
}

static char *
command_arg_quote(char *dst, const char *s)
{
	if (!command_arg_needs_quotes(s)) {
		size_t len = strlen(s);
		memcpy(dst, s, len);
		return (dst + len);
	}

	*dst++ = '"';
	for (; *s != '\0'; s++) {
		switch (*s) {
		case '\\':
			*dst++ = '\\';
			*dst++ = '\\';
			break;
		case '"':
			*dst++ = '\\';
			*dst++ = '"';
			break;
		case '\n':
			*dst++ = '\\';
			*dst++ = 'n';
			break;
		case '\r':
			*dst++ = '\\';
			*dst++ = 'r';
			break;
		case '\t':
			*dst++ = '\\';
			*dst++ = 't';
			break;
		default:
			*dst++ = *s;
			break;
		}
	}
	*dst++ = '"';
	return (dst);
}

char *
cow_command_from_argv(size_t argc, const char *const *argv)
{
	size_t len = 0;

	for (size_t i = 0; i < argc; i++)
		len += command_arg_quoted_len(argv[i]) + 1;
	if (len == 0)
		return (strdup(""));

	char *buf = malloc(len);
	if (buf == NULL)
		return (NULL);
	char *p = buf;
	for (size_t i = 0; i < argc; i++) {
		if (i != 0)
			*p++ = ' ';
		p = command_arg_quote(p, argv[i]);
	}
	*p = '\0';
	return (buf);
}

char *
cow_markup_escape(const char *text)
{
	if (text == NULL)
		return (strdup(""));

	size_t len = 0;
	for (const char *p = text; *p != '\0'; p++) {
		switch (*p) {
		case '&': len += 5; break;
		case '<':
		case '>': len += 4; break;
		case '\'':
		case '"': len += 6; break;
		default: len++; break;
		}
	}

	char *escaped = malloc(len + 1);
	if (escaped == NULL)
		return (NULL);
	char *dst = escaped;
	for (const char *p = text; *p != '\0'; p++) {
		const char *entity = NULL;
		switch (*p) {
		case '&': entity = "&amp;"; break;
		case '<': entity = "&lt;"; break;
		case '>': entity = "&gt;"; break;
		case '\'': entity = "&apos;"; break;
		case '"': entity = "&quot;"; break;
		default: *dst++ = *p; break;
		}
		if (entity != NULL) {
			size_t entity_len = strlen(entity);
			memcpy(dst, entity, entity_len);
			dst += entity_len;
		}
	}
	*dst = '\0';
	return (escaped);
}

char *
cow_path_resolve(const char *base_dir, const char *path)
{
	if (path == NULL)
		return (NULL);
	if (path[0] == '/')
		return (strdup(path));

	const char *suffix = NULL;
	if (path[0] == '~' && (path[1] == '\0' || path[1] == '/'))
		suffix = path + 1;
	else if (strncmp(path, "$HOME", 5) == 0 &&
	    (path[5] == '\0' || path[5] == '/'))
		suffix = path + 5;
	else if (strncmp(path, "${HOME}", 7) == 0 &&
	    (path[7] == '\0' || path[7] == '/'))
		suffix = path + 7;
	if (suffix != NULL) {
		const char *home = getenv("HOME");
		if (home == NULL || home[0] == '\0')
			return (NULL);
		char *resolved;
		if (asprintf(&resolved, "%s%s", home, suffix) < 0)
			return (NULL);
		return (resolved);
	}

	if (base_dir == NULL || base_dir[0] == '\0')
		return (strdup(path));
	char *resolved;
	if (asprintf(&resolved, "%s/%s", base_dir, path) < 0)
		return (NULL);
	return (resolved);
}

char *
cow_path_dirname(const char *path)
{
	if (path == NULL)
		return (NULL);
	const char *slash = strrchr(path, '/');
	if (slash == NULL)
		return (strdup("."));
	if (slash == path)
		return (strdup("/"));
	size_t len = (size_t)(slash - path);
	char *dir = malloc(len + 1);
	if (dir == NULL)
		return (NULL);
	memcpy(dir, path, len);
	dir[len] = '\0';
	return (dir);
}

long
cow_mono_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static const char *
display_basename(void)
{
	const char *display = getenv("WAYLAND_DISPLAY");
	if (display == NULL || *display == '\0')
		return (NULL);

	const char *slash = strrchr(display, '/');
	if (slash != NULL && slash[1] != '\0')
		display = slash + 1;
	return (*display != '\0' ? display : NULL);
}

static void
sanitize_display(char *dst, size_t dstsz, const char *src)
{
	size_t n = 0;
	for (; *src != '\0' && n + 1 < dstsz; src++) {
		unsigned char ch = (unsigned char)*src;
		dst[n++] = (isalnum(ch) || ch == '.' || ch == '_' ||
		    ch == '-') ? (char)ch : '_';
	}
	dst[n] = '\0';
	if (dst[0] == '\0')
		(void)snprintf(dst, dstsz, "default");
}

bool
cow_runtime_socket_path(char *buf, size_t bufsz, const char *prog,
    const char *kind)
{
	const char *runtime = getenv("XDG_RUNTIME_DIR");

	if (runtime == NULL) {
		fprintf(stderr, "%s: %s: XDG_RUNTIME_DIR not set\n", prog,
		    kind);
		return (false);
	}

	int n;
	const char *display = display_basename();
	if (display == NULL) {
		n = snprintf(buf, bufsz, "%s/cow-%s.sock", runtime, kind);
	} else {
		char safe[64];
		sanitize_display(safe, sizeof(safe), display);
		n = snprintf(buf, bufsz, "%s/cow-%s-%s.sock", runtime, safe,
		    kind);
	}
	if (n < 0 || (size_t)n >= bufsz) {
		fprintf(stderr, "%s: %s: socket path too long\n", prog, kind);
		return (false);
	}
	return (true);
}

int
cow_unix_listen(const char *name, char *path_out, size_t path_cap, int backlog)
{
	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	if (!cow_runtime_socket_path(addr.sun_path, sizeof(addr.sun_path),
		"cow", name))
		return -1;
	if (path_out != NULL && path_cap > 0) {
		size_t n = strlen(addr.sun_path);
		if ((size_t)n >= path_cap) {
			fprintf(stderr,
			    "cow: %s: caller path buffer too small\n", name);
			return -1;
		}
		memcpy(path_out, addr.sun_path, (size_t)n + 1);
	}

	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (fd < 0) {
		fprintf(stderr, "cow: %s: socket: %s\n", name, strerror(errno));
		return -1;
	}

	/* Stale socket from a previous run.  Best-effort unlink. */
	(void)unlink(addr.sun_path);

	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		fprintf(stderr, "cow: %s: bind %s: %s\n", name, addr.sun_path,
		    strerror(errno));
		close(fd);
		return -1;
	}
	if (listen(fd, backlog) < 0) {
		fprintf(stderr, "cow: %s: listen: %s\n", name, strerror(errno));
		(void)unlink(addr.sun_path);
		close(fd);
		return -1;
	}

	return fd;
}
