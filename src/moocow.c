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
 * moocow - Command-line tool for sending commands to cow.
 *
 * Connects to the cow IPC socket and sends a command,
 * then prints the JSON response.
 *
 * Usage:
 *   moocow set border 10
 *   moocow view-close
 *   moocow get windows
 *   moocow bind L+q view-quit
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static const char *
get_socket_path(void)
{
	static char path[4096];
	const char *override = getenv("COW_IPC_SOCKET");
	if (override != NULL)
		return override;

	const char *runtime = getenv("XDG_RUNTIME_DIR");
	if (runtime == NULL) {
		fprintf(stderr, "moocow: XDG_RUNTIME_DIR not set\n");
		return NULL;
	}
	snprintf(path, sizeof(path), "%s/cow-cmd.sock", runtime);
	return path;
}

static int
connect_to_cow(void)
{
	const char *path = get_socket_path();
	if (path == NULL)
		return -1;

	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) {
		perror("moocow: socket");
		return -1;
	}

	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	if (strlen(path) >= sizeof(addr.sun_path)) {
		fprintf(stderr, "moocow: socket path too long: %s\n", path);
		close(fd);
		return -1;
	}
	strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		fprintf(stderr, "moocow: cannot connect to %s: %s\n", path,
		    strerror(errno));
		close(fd);
		return -1;
	}

	return fd;
}

int
main(int argc, char *argv[])
{
	if (argc < 2) {
		fprintf(stderr,
		    "Usage: moocow COMMAND [ARGS...]\n"
		    "\n"
		    "Examples:\n"
		    "  moocow set border 10\n"
		    "  moocow get windows\n"
		    "  moocow view-close\n"
		    "  moocow bind L+q view-quit\n"
		    "  moocow action terminal xterm\n"
		    "  moocow exec foot\n");
		return 1;
	}

	int fd = connect_to_cow();
	if (fd < 0)
		return 1;

	/* Join all arguments into a single command line */
	char cmd[4096];
	int  pos = 0;
	for (int i = 1; i < argc; i++) {
		if (i > 1)
			cmd[pos++] = ' ';
		int len = strlen(argv[i]);
		if (pos + len + 2 >= (int)sizeof(cmd)) {
			fprintf(stderr, "moocow: command too long\n");
			close(fd);
			return 1;
		}
		memcpy(cmd + pos, argv[i], len);
		pos += len;
	}
	cmd[pos++] = '\n';
	cmd[pos]   = '\0';

	/* Send command */
	if (write(fd, cmd, pos) < 0) {
		perror("moocow: write");
		close(fd);
		return 1;
	}

	/* Read response */
	char resp[65536];
	int  total = 0;
	while (total < (int)sizeof(resp) - 1) {
		int n = read(fd, resp + total, sizeof(resp) - 1 - total);
		if (n <= 0)
			break;
		total += n;
		/* Check if we got a complete line */
		if (memchr(resp, '\n', total) != NULL)
			break;
	}
	resp[total] = '\0';

	/* Print response */
	printf("%s", resp);
	if (total > 0 && resp[total - 1] != '\n')
		printf("\n");

	close(fd);

	/* Exit with error if response indicates failure */
	if (strstr(resp, "\"ok\":false") != NULL)
		return 1;

	return 0;
}
