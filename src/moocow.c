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
#include <unistd.h>

#include <cJSON.h>

#include "cow/cmd_scan.h"
#include "cow/module.h"

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

struct moocow_state {
	const char *primary_prompt;
	const char *secondary_prompt;
	const char *prompt_colour;
	bool	    pretty_json;
};

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

static int
print_response(char *resp, bool pretty_json)
{
	size_t len = strlen(resp);

	if (pretty_json) {
		cJSON *json = cJSON_Parse(resp);
		if (json != NULL) {
			char *pretty = cJSON_Print(json);
			if (pretty != NULL) {
				printf("%s\n", pretty);
				cJSON_free(pretty);
			} else {
				printf("%s", resp);
				if (len > 0 && resp[len - 1] != '\n')
					printf("\n");
			}
			cJSON_Delete(json);
		} else {
			printf("%s", resp);
			if (len > 0 && resp[len - 1] != '\n')
				printf("\n");
		}
	} else {
		printf("%s", resp);
		if (len > 0 && resp[len - 1] != '\n')
			printf("\n");
	}

	return strstr(resp, "\"ok\":false") != NULL ? 1 : 0;
}

static int
send_command(const char *cmd, bool pretty_json)
{
	int fd = cow_module_cmd_connect("moocow");
	if (fd < 0)
		return 1;

	char *out = collapse_continuations(cmd);
	if (out == NULL) {
		close(fd);
		return 1;
	}

	int ret = 1;
	if (cow_module_cmd_send(fd, out, false)) {
		char *resp = cow_module_read_response(fd,
		    COW_MODULE_RESPONSE_INITIAL, true);
		if (resp != NULL) {
			ret = print_response(resp, pretty_json);
			free(resp);
		}
	} else
		perror("moocow: write");

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

static char *
copy_command(const char *cmd)
{
	size_t len = strlen(cmd);
	char  *copy = malloc(len + 1);

	if (copy == NULL) {
		perror("moocow: malloc");
		return NULL;
	}
	memcpy(copy, cmd, len + 1);
	return copy;
}

static bool
parse_bool(const char *s, bool *out)
{
	if (strcasecmp(s, "true") == 0 || strcasecmp(s, "on") == 0 ||
	    strcmp(s, "1") == 0) {
		*out = true;
		return true;
	}
	if (strcasecmp(s, "false") == 0 || strcasecmp(s, "off") == 0 ||
	    strcmp(s, "0") == 0) {
		*out = false;
		return true;
	}
	return false;
}

static void
show_moocow_state(const struct moocow_state *state)
{
	printf("pretty-print-json %s\n", state->pretty_json ? "true" : "false");
}

static bool
handle_moocow_command(struct moocow_state *state, const char *cmd,
    bool *failed)
{
	while (*cmd == ' ' || *cmd == '\t')
		cmd++;
	if (*cmd != '.')
		return false;

	char *copy = copy_command(cmd);
	if (copy == NULL) {
		*failed = true;
		return true;
	}

	char *name = strtok(copy, " \t\r\n");
	if (name == NULL) {
		free(copy);
		return true;
	}

	if (strcmp(name, ".moocow-show") == 0) {
		if (strtok(NULL, " \t\r\n") != NULL) {
			fprintf(stderr, "moocow: usage: .moocow-show\n");
			*failed = true;
		} else
			show_moocow_state(state);
		free(copy);
		return true;
	}

	if (strcmp(name, ".moocow-set") == 0) {
		char *key = strtok(NULL, " \t\r\n");
		char *val = strtok(NULL, " \t\r\n");
		if (key == NULL || val == NULL ||
		    strtok(NULL, " \t\r\n") != NULL) {
			fprintf(stderr,
			    "moocow: usage: .moocow-set key value\n");
			*failed = true;
		} else if (strcmp(key, "pretty-print-json") == 0) {
			bool b;
			if (!parse_bool(val, &b)) {
				fprintf(stderr,
				    "moocow: bad boolean value: %s\n", val);
				*failed = true;
			} else
				state->pretty_json = b;
		} else {
			fprintf(stderr, "moocow: unknown setting: %s\n", key);
			*failed = true;
		}
		free(copy);
		return true;
	}

	fprintf(stderr, "moocow: unknown command: %s\n", name);
	*failed = true;
	free(copy);
	return true;
}

static int
persistent_mode(struct moocow_state *state)
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
			print_prompt(cmd_len == 0 ? state->primary_prompt :
			    state->secondary_prompt, state->prompt_colour);

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

		bool failed = false;
		if (!skip_command(cmd) &&
		    !handle_moocow_command(state, cmd, &failed) &&
		    send_command(cmd, state->pretty_json) != 0)
			failed = true;
		if (failed)
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
	struct moocow_state state = {
		.primary_prompt		= ">>> ",
		.secondary_prompt	= "... ",
		.prompt_colour		= "red",
		.pretty_json		= true,
	};
	int ch;

	while ((ch = getopt(argc, argv, "hP:S:C:p")) != -1) {
		switch (ch) {
		case 'p':
			persistent = true;
			break;
		case 'P':
			state.primary_prompt = optarg;
			prompt_option = true;
			break;
		case 'S':
			state.secondary_prompt = optarg;
			prompt_option = true;
			break;
		case 'C':
			state.prompt_colour = optarg;
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

	if (!prompt_colour_valid(state.prompt_colour)) {
		fprintf(stderr, "moocow: unknown prompt colour: %s\n",
		    state.prompt_colour);
		return 1;
	}

	if (persistent) {
		if (argc != 0) {
			fputs(usage, stderr);
			return 1;
		}
		return persistent_mode(&state);
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

	int ret = send_command(cmd, state.pretty_json);
	free(cmd);
	return ret;
}
