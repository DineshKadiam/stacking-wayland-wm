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

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cow/memory.h"
#include "appmenu-entry.h"
#include "appmenu-launch.h"

static const char *
string(const cJSON *app, const char *key)
{
	const cJSON *value = cJSON_GetObjectItemCaseSensitive(app, key);

	return cJSON_IsString(value) ? value->valuestring : "";
}

static void
add(cJSON *args, const char *value)
{
	cJSON_AddItemToArray(args, cJSON_CreateString(value));
}

/* Exec quoting is not shell syntax; expansion happens after tokenization. */
cJSON *
appmenu_arguments(const cJSON *app)
{
	char *line = appmenu_unescape(string(app, "Exec")), *p = line;
	cJSON *args = cJSON_CreateArray();
	int file_codes = 0;

	while (*p) {
		char *token, *out, *expanded;
		size_t length, used = 0, capacity;
		int quoted, removed = 0;

		while (*p == ' ' || *p == '\t')
			p++;
		if (!*p)
			break;
		quoted = *p == '"';
		if (quoted)
			p++;
		token = xmalloc(strlen(p) + 1);
		out = token;
		while (*p && (quoted ? *p != '"' : (*p != ' ' && *p != '\t'))) {
			if (quoted && *p == '\\' && p[1] && strchr("\"`$\\", p[1])) {
				p++;
			} else if (strchr(quoted ? "\n\r`$\\" : "\n\r\"'\\><~|&;$*?#()`", *p)) {
				free(token);
				goto invalid;
			}
			*out++ = *p++;
		}
		*out = '\0';
		if (quoted && (*p++ != '"' || (*p && *p != ' ' && *p != '\t'))) {
			free(token);
			goto invalid;
		}
		length = strlen(token);
		capacity = length + 1;
		expanded = xmalloc(capacity);
		for (size_t i = 0; i < length; i++) {
			const char *replacement = NULL;
			char literal[2] = {token[i], '\0'};

			if (token[i] == '%') {
				char code = token[++i];

				if (!code || (quoted && code != '%'))
					goto bad_token;
				if (strchr("fFuU", code)) {
					if (++file_codes > 1)
						goto bad_token;
					if (strchr("FU", code) && length != 2)
						goto bad_token;
					removed = 1;
					continue;
				}
				if (strchr("dDnNvm", code)) {
					removed = 1;
					continue;
				}
				switch (code) {
				case '%': replacement = "%"; break;
				case 'c': replacement = string(app, "Name"); break;
				case 'k': replacement = string(app, "file"); break;
				case 'i':
					if (length != 2)
						goto bad_token;
					if (*string(app, "Icon")) {
						add(args, "--icon");
						add(args, string(app, "Icon"));
					}
					removed = 1;
					continue;
				default:
					goto bad_token;
				}
			}
			if (!replacement)
				replacement = literal;
			size_t n = strlen(replacement);
			if (used + n + 1 > capacity) {
				capacity = used + n + length + 1;
				expanded = xrealloc(expanded, capacity, 1);
			}
			memcpy(expanded + used, replacement, n);
			used += n;
		}
		expanded[used] = '\0';
		if (used || !removed)
			add(args, expanded);
		free(expanded);
		free(token);
		continue;
bad_token:
		free(expanded);
		free(token);
		goto invalid;
	}
	free(line);
	if (args->child && *args->child->valuestring &&
	    !strchr(args->child->valuestring, '='))
		return args;
	cJSON_Delete(args);
	return NULL;
invalid:
	free(line);
	cJSON_Delete(args);
	return NULL;
}

int
appmenu_launch(const char *file, const char *terminal)
{
	char *path = realpath(file, NULL), **argv = NULL;
	cJSON *app = path ? appmenu_application(path) : NULL, *args = NULL, *item;
	size_t n = 0;

	if (!app || !(args = appmenu_arguments(app))) {
		fprintf(stderr, "cow-appmenu: cannot launch %s: unavailable or unsupported entry\n", file);
		goto out;
	}
	argv = xcalloc(cJSON_GetArraySize(args) + 3, sizeof(*argv));
	if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(app, "Terminal"))) {
		argv[n++] = (char *)terminal;
		argv[n++] = "-e";
	}
	cJSON_ArrayForEach(item, args)
		argv[n++] = item->valuestring;
	if (*string(app, "Path") && chdir(string(app, "Path")) == -1) {
		fprintf(stderr, "cow-appmenu: chdir %s: %s\n",
		    string(app, "Path"), strerror(errno));
		goto out;
	}
	execvp(argv[0], argv);
	fprintf(stderr, "cow-appmenu: exec %s: %s\n", argv[0], strerror(errno));
out:
	free(argv);
	cJSON_Delete(args);
	cJSON_Delete(app);
	free(path);
	return 1;
}
