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
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "cow/cmd_scan.h"

#ifndef COW_VERSION
#define COW_VERSION "unknown"
#endif

static const char *usage =
    "Usage: moocow [-h]\n"
    "       moocow -p [-P prompt] [-S prompt] [-C colour]\n"
    "       moocow COMMAND [ARGS...]\n"
    "\n"
    "Examples:\n"
    "  moocow -p\n"
    "  moocow -p -P 'cow> ' -S '...> ' -C green\n"
    "  moocow set border 10\n"
    "  moocow get windows\n"
    "  moocow view-close\n"
    "  moocow bind L+q view-quit\n"
    "  moocow action terminal xterm\n"
    "  moocow exec foot\n";

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

static bool
append_buf(char **buf, size_t *len, size_t *cap, const char *s, size_t slen)
{
	if (*len + slen + 1 > *cap) {
		size_t new_cap = *cap ? *cap : 128;
		while (*len + slen + 1 > new_cap)
			new_cap *= 2;
		char *new_buf = realloc(*buf, new_cap);
		if (new_buf == NULL) {
			perror("moocow: realloc");
			return false;
		}
		*buf = new_buf;
		*cap = new_cap;
	}

	memcpy(*buf + *len, s, slen);
	*len += slen;
	(*buf)[*len] = '\0';
	return true;
}

static char *
build_command(int argc, char *argv[])
{
	char  *cmd = NULL;
	size_t len = 0, cap = 0;

	for (int i = 0; i < argc; i++) {
		if (i > 0 && !append_buf(&cmd, &len, &cap, " ", 1))
			goto error;
		if (!append_buf(&cmd, &len, &cap, argv[i], strlen(argv[i])))
			goto error;
	}
	if (!append_buf(&cmd, &len, &cap, "\n", 1))
		goto error;
	return cmd;

error:
	free(cmd);
	return NULL;
}

static char *
collapse_continuations(const char *cmd)
{
	char  *out = NULL;
	size_t len = 0, cap = 0;

	for (size_t i = 0; cmd[i] != '\0'; i++) {
		if (cmd[i] == '\\' && cmd[i + 1] == '\n') {
			i++;
			continue;
		}
		if (!append_buf(&out, &len, &cap, &cmd[i], 1))
			goto error;
	}
	return out;

error:
	free(out);
	return NULL;
}

static bool
write_all(int fd, const char *buf, size_t len)
{
	while (len > 0) {
		ssize_t n = write(fd, buf, len);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			perror("moocow: write");
			return false;
		}
		if (n == 0)
			return false;
		buf += n;
		len -= n;
	}
	return true;
}

static int
read_response(int fd)
{
	char resp[65536];
	int  total = 0;

	while (total < (int)sizeof(resp) - 1) {
		int n = read(fd, resp + total, sizeof(resp) - 1 - total);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			perror("moocow: read");
			return 1;
		}
		if (n == 0)
			break;
		total += n;
		if (memchr(resp, '\n', total) != NULL)
			break;
	}
	resp[total] = '\0';

	printf("%s", resp);
	if (total > 0 && resp[total - 1] != '\n')
		printf("\n");

	return strstr(resp, "\"ok\":false") != NULL ? 1 : 0;
}

static int
send_command(const char *cmd)
{
	int fd = connect_to_cow();
	if (fd < 0)
		return 1;

	char *out = collapse_continuations(cmd);
	if (out == NULL) {
		close(fd);
		return 1;
	}

	int ret = 1;
	if (write_all(fd, out, strlen(out)))
		ret = read_response(fd);

	free(out);
	close(fd);
	return ret;
}

static const char *
prompt_colour_code(const char *colour)
{
	if (colour == NULL || strcasecmp(colour, "red") == 0)
		return "31";
	if (strcasecmp(colour, "black") == 0)
		return "30";
	if (strcasecmp(colour, "green") == 0)
		return "32";
	if (strcasecmp(colour, "yellow") == 0)
		return "33";
	if (strcasecmp(colour, "blue") == 0)
		return "34";
	if (strcasecmp(colour, "magenta") == 0)
		return "35";
	if (strcasecmp(colour, "cyan") == 0)
		return "36";
	if (strcasecmp(colour, "white") == 0)
		return "37";
	if (strcasecmp(colour, "none") == 0)
		return NULL;
	return "";
}

static bool
prompt_colour_valid(const char *colour)
{
	const char *code = prompt_colour_code(colour);

	return code == NULL || code[0] != '\0';
}

static void
print_prompt(const char *prompt, const char *colour)
{
	const char *code = prompt_colour_code(colour);

	if (code == NULL)
		fputs(prompt, stdout);
	else
		printf("\033[%sm%s\033[0m", code, prompt);
	fflush(stdout);
}

static bool
skip_command(const char *cmd)
{
	while (*cmd == ' ' || *cmd == '\t')
		cmd++;
	return (*cmd == '\0' || *cmd == '\n' || *cmd == '#');
}

static bool
command_complete(const char *cmd, size_t len)
{
	size_t end;
	return cow_cmd_find_terminator(cmd, len, &end) == COW_CMD_SCAN_COMPLETE;
}

static int
persistent_mode(const char *primary_prompt, const char *secondary_prompt,
    const char *prompt_colour)
{
	bool interactive = isatty(STDIN_FILENO) && isatty(STDOUT_FILENO);
	char *cmd = NULL, *line = NULL;
	size_t cmd_len = 0, cmd_cap = 0, line_cap = 0;
	int ret = 0;

	if (interactive)
		printf("*moocow (%s) [client %ld]\n\n", COW_VERSION,
		    (long)getpid());

	for (;;) {
		if (interactive)
			print_prompt(cmd_len == 0 ? primary_prompt :
			    secondary_prompt, prompt_colour);

		errno = 0;
		ssize_t line_len = getline(&line, &line_cap, stdin);
		if (line_len < 0) {
			if (errno != 0) {
				perror("moocow: getline");
				ret = 1;
			} else if (cmd_len > 0) {
				fprintf(stderr, "moocow: incomplete command\n");
				ret = 1;
			}
			break;
		}

		if (!append_buf(&cmd, &cmd_len, &cmd_cap, line, line_len)) {
			ret = 1;
			break;
		}
		if (!command_complete(cmd, cmd_len))
			continue;

		if (!skip_command(cmd) && send_command(cmd) != 0)
			ret = 1;
		cmd_len = 0;
		if (cmd != NULL)
			cmd[0] = '\0';
	}

	free(line);
	free(cmd);
	return ret;
}

int
main(int argc, char *argv[])
{
	bool persistent = false, prompt_option = false;
	const char *primary_prompt = ">>> ";
	const char *secondary_prompt = "... ";
	const char *prompt_colour = "red";
	int ch;

	while ((ch = getopt(argc, argv, "hP:S:C:p")) != -1) {
		switch (ch) {
		case 'p':
			persistent = true;
			break;
		case 'P':
			primary_prompt = optarg;
			prompt_option = true;
			break;
		case 'S':
			secondary_prompt = optarg;
			prompt_option = true;
			break;
		case 'C':
			prompt_colour = optarg;
			prompt_option = true;
			break;
		case 'h':
			fputs(usage, stdout);
			return 0;
		default:
			fputs(usage, stderr);
			return 1;
		}
	}

	argc -= optind;
	argv += optind;

	if (!prompt_colour_valid(prompt_colour)) {
		fprintf(stderr, "moocow: unknown prompt colour: %s\n",
		    prompt_colour);
		return 1;
	}

	if (persistent) {
		if (argc != 0) {
			fputs(usage, stderr);
			return 1;
		}
		return persistent_mode(primary_prompt, secondary_prompt,
		    prompt_colour);
	}

	if (prompt_option) {
		fputs(usage, stderr);
		return 1;
	}

	if (argc < 1) {
		fputs(usage, stderr);
		return 1;
	}

	char *cmd = build_command(argc, argv);
	if (cmd == NULL)
		return 1;

	int ret = send_command(cmd);
	free(cmd);
	return ret;
}
