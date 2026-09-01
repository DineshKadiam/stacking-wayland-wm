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
#include "cow/module.h"
#include "cow/util.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static bool
send_identity(int fd, const char *prog)
{
	if (prog == NULL || *prog == '\0' ||
	    strlen(prog) > COW_MODULE_CLIENT_NAME_MAX) {
		errno = EINVAL;
		return (false);
	}

	char hello[sizeof(COW_MODULE_HELLO) + COW_MODULE_CLIENT_NAME_MAX + 1];
	int len = snprintf(hello, sizeof(hello), "%s%s\n", COW_MODULE_HELLO,
	    prog);
	if (len < 0 || (size_t)len >= sizeof(hello)) {
		errno = EINVAL;
		return (false);
	}

	size_t offset = 0;
	while (offset < (size_t)len) {
		ssize_t n = write(fd, hello + offset, (size_t)len - offset);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return (false);
		}
		if (n == 0)
			return (false);
		offset += (size_t)n;
	}
	return (true);
}

bool
cow_module_cmd_path(char *buf, size_t bufsz, const char *prog)
{
	const char *override = getenv("COW_IPC_SOCKET");

	if (override != NULL) {
		if (snprintf(buf, bufsz, "%s", override) >= (int)bufsz) {
			fprintf(stderr, "%s: socket path too long: %s\n", prog,
			    override);
			return false;
		}
		return true;
	}
	return cow_runtime_socket_path(buf, bufsz, prog, "cmd");
}

bool
cow_module_status_path(char *buf, size_t bufsz, const char *prog)
{
	const char *override = getenv("COW_STATUS_SOCKET");

	if (override != NULL) {
		if (snprintf(buf, bufsz, "%s", override) >= (int)bufsz) {
			fprintf(stderr, "%s: socket path too long: %s\n", prog,
			    override);
			return false;
		}
		return true;
	}
	return cow_runtime_socket_path(buf, bufsz, prog, "status");
}

static int
connect_path(const char *prog, const char *path, bool quiet_connect)
{
	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) {
		fprintf(stderr, "%s: socket: %s\n", prog, strerror(errno));
		return -1;
	}

	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	if (strlen(path) >= sizeof(addr.sun_path)) {
		fprintf(stderr, "%s: socket path too long: %s\n", prog, path);
		close(fd);
		return -1;
	}
	strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		if (!quiet_connect) {
			fprintf(stderr, "%s: cannot connect to %s: %s\n", prog,
			    path, strerror(errno));
		}
		close(fd);
		return -1;
	}
	if (!send_identity(fd, prog)) {
		fprintf(stderr, "%s: cannot identify client: %s\n", prog,
		    strerror(errno));
		close(fd);
		return (-1);
	}
	return fd;
}

int
cow_module_connect_path(const char *prog, const char *path)
{
	return connect_path(prog, path, false);
}

int
cow_module_cmd_connect(const char *prog)
{
	char path[COW_MODULE_SOCKET_PATH_MAX];

	if (!cow_module_cmd_path(path, sizeof(path), prog))
		return -1;
	return cow_module_connect_path(prog, path);
}

int
cow_module_cmd_connect_quiet(const char *prog)
{
	char path[COW_MODULE_SOCKET_PATH_MAX];

	if (!cow_module_cmd_path(path, sizeof(path), prog))
		return -1;
	return connect_path(prog, path, true);
}

int
cow_module_status_connect_quiet(const char *prog)
{
	char path[COW_MODULE_SOCKET_PATH_MAX];

	if (!cow_module_status_path(path, sizeof(path), prog))
		return -1;
	return connect_path(prog, path, true);
}

int
cow_module_status_connect(const char *prog, int retries, unsigned int delay)
{
	char path[COW_MODULE_SOCKET_PATH_MAX];

	if (!cow_module_status_path(path, sizeof(path), prog))
		return -1;
	if (retries < 1)
		retries = 1;

	for (int i = 0; i < retries; i++) {
		int fd = connect_path(prog, path, true);
		if (fd >= 0)
			return fd;
		if (i + 1 < retries)
			sleep(delay);
	}

	fprintf(stderr, "%s: cannot connect to %s\n", prog, path);
	return -1;
}

bool
cow_module_cmd_send(int fd, const char *cmd, bool append_newline)
{
	size_t len = strlen(cmd);

	while (len > 0) {
		ssize_t n = write(fd, cmd, len);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return false;
		}
		if (n == 0)
			return false;
		cmd += n;
		len -= (size_t)n;
	}
	if (!append_newline)
		return true;
	for (;;) {
		ssize_t n = write(fd, "\n", 1);
		if (n == 1)
			return true;
		if (n < 0 && errno == EINTR)
			continue;
		return false;
	}
}

char *
cow_module_read_response(int fd, size_t initial_cap, bool stop_at_newline)
{
	size_t cap = initial_cap != 0 ? initial_cap : COW_MODULE_ACK_BUFFER_SIZE;
	size_t len = 0;
	char  *buf = malloc(cap);

	if (buf == NULL)
		return NULL;

	for (;;) {
		if (len + 1 >= cap) {
			size_t ncap = cap * 2;
			char  *nbuf = realloc(buf, ncap);
			if (nbuf == NULL) {
				free(buf);
				return NULL;
			}
			buf = nbuf;
			cap = ncap;
		}

		ssize_t n = read(fd, buf + len, cap - len - 1);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			free(buf);
			return NULL;
		}
		if (n == 0)
			break;
		len += (size_t)n;
		if (stop_at_newline && memchr(buf, '\n', len) != NULL)
			break;
	}
	buf[len] = '\0';
	return buf;
}

char *
cow_module_cmd_request(const char *prog, const char *cmd, bool append_newline)
{
	int fd = cow_module_cmd_connect(prog);

	if (fd < 0)
		return NULL;
	if (!cow_module_cmd_send(fd, cmd, append_newline)) {
		fprintf(stderr, "%s: write: %s\n", prog, strerror(errno));
		close(fd);
		return NULL;
	}

	char *resp = cow_module_read_response(fd, COW_MODULE_RESPONSE_INITIAL,
	    true);
	close(fd);
	return resp;
}

bool
cow_module_cmd_execute(const char *prog, const char *cmd, bool append_newline)
{
	char *response = cow_module_cmd_request(prog, cmd, append_newline);

	if (response == NULL)
		return false;
	free(response);
	return true;
}
