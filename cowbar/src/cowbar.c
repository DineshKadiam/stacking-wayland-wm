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
 * cowbar -- status relay for the cow window manager.
 *
 * Connects to cow's status socket and relays JSON state snapshots
 * to stdout, one line per update.  Optionally filters by output
 * name and/or extracts a specific field.
 *
 * Usage:
 *   cowbar                     # relay full snapshots
 *   cowbar -o DP-1             # filter for output DP-1
 *   cowbar -f desks -o DP-1    # extract desks field for DP-1
 *   cowbar -f focused          # extract focused window info

 *
 * Output is one JSON line per state change on stdout.
 * No styling or markup -- consumers handle presentation.
 */

#include <cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static void
usage(void)
{
	fprintf(stderr,
	    "usage: cowbar [-o output] [-f field]\n"
	    "\n"
	    "  -o output   filter for named output\n"
	    "  -f field    extract field: desks, focused, outputs\n"
	    "\n"
	    "With no flags, relays full state snapshots.\n");
}

/*
 * Find the output object matching the given name.
 * Returns NULL if not found or name is NULL (returns first).
 */
static cJSON *
find_output(cJSON *state, const char *name)
{
	cJSON *outputs = cJSON_GetObjectItem(state, "outputs");
	if (!cJSON_IsArray(outputs))
		return NULL;

	cJSON *out;
	cJSON_ArrayForEach(out, outputs)
	{
		if (name == NULL)
			return out;
		cJSON *n = cJSON_GetObjectItem(out, "name");
		if (cJSON_IsString(n) && strcmp(n->valuestring, name) == 0)
			return out;
	}
	return NULL;
}

static void
emit(cJSON *obj)
{
	char *s = cJSON_PrintUnformatted(obj);
	if (s != NULL) {
		puts(s);
		fflush(stdout);
		cJSON_free(s);
	}
}

int
main(int argc, char *argv[])
{
	const char *output_filter = NULL;
	const char *field	  = NULL;
	int	    opt;

	while ((opt = getopt(argc, argv, "o:f:h")) != -1) {
		switch (opt) {
		case 'o':
			output_filter = optarg;
			break;
		case 'f':
			field = optarg;
			break;
		case 'h':
		default:
			usage();
			return opt == 'h' ? 0 : 1;
		}
	}

	const char *runtime = getenv("XDG_RUNTIME_DIR");
	if (runtime == NULL) {
		fprintf(stderr, "cowbar: XDG_RUNTIME_DIR not set\n");
		return 1;
	}

	char sock_path[4096];
	snprintf(sock_path, sizeof(sock_path), "%s/cow-status.sock", runtime);

	int sockfd = -1;
	for (int retry = 0; retry < 30; retry++) {
		sockfd = socket(AF_UNIX, SOCK_STREAM, 0);
		if (sockfd < 0) {
			perror("cowbar: socket");
			return 1;
		}
		struct sockaddr_un addr = { .sun_family = AF_UNIX };
		strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);
		if (connect(sockfd, (struct sockaddr *)&addr, sizeof(addr)) ==
		    0)
			break;
		close(sockfd);
		sockfd = -1;
		sleep(1);
	}
	if (sockfd < 0) {
		fprintf(stderr, "cowbar: cannot connect to %s\n", sock_path);
		return 1;
	}

	FILE *f = fdopen(sockfd, "r");
	if (f == NULL) {
		perror("cowbar: fdopen");
		close(sockfd);
		return 1;
	}
	setlinebuf(f);

	char line[65536];
	while (fgets(line, sizeof(line), f) != NULL) {
		size_t len = strlen(line);
		if (len > 0 && line[len - 1] == '\n')
			line[len - 1] = '\0';

		cJSON *state = cJSON_Parse(line);
		if (state == NULL)
			continue;

		if (field == NULL && output_filter == NULL) {
			/* No filtering -- relay full snapshot */
			emit(state);
		} else if (field != NULL && strcmp(field, "desks") == 0) {
			/* Extract desks for a specific output */
			cJSON *out = find_output(state, output_filter);
			if (out != NULL) {
				cJSON *desks = cJSON_GetObjectItem(out,
				    "desks");
				cJSON *cur   = cJSON_GetObjectItem(out,
				      "current_desk");
				if (desks != NULL) {
					cJSON *result = cJSON_CreateObject();
					cJSON_AddItemReferenceToObject(result,
					    "desks", desks);
					if (cur != NULL)
						cJSON_AddItemReferenceToObject(
						    result, "current_desk",
						    cur);
					cJSON *name = cJSON_GetObjectItem(out,
					    "name");
					if (name != NULL)
						cJSON_AddItemReferenceToObject(
						    result, "output", name);
					emit(result);
					cJSON_Delete(result);
				}
			}
		} else if (field != NULL && strcmp(field, "focused") == 0) {
			cJSON *focused = cJSON_GetObjectItem(state, "focused");
			if (focused != NULL)
				emit(focused);
		} else if (field != NULL && strcmp(field, "outputs") == 0) {
			cJSON *outputs = cJSON_GetObjectItem(state, "outputs");
			if (outputs != NULL)
				emit(outputs);
		} else if (output_filter != NULL && field == NULL) {
			/* Filter to a specific output's full state */
			cJSON *out = find_output(state, output_filter);
			if (out != NULL)
				emit(out);
		}

		cJSON_Delete(state);
	}

	fclose(f);
	return 0;
}
