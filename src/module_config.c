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
#include "cow/module_config.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cow/util.h"

char *
cow_module_config_default_path(const char *prog)
{
	const char *xdg = getenv("XDG_CONFIG_HOME");
	const char *home;
	char *path;

	if (xdg != NULL && *xdg != '\0') {
		if (asprintf(&path, "%s/cow/%s.conf", xdg, prog) < 0)
			return NULL;
		return path;
	}
	home = getenv("HOME");
	if (home == NULL || *home == '\0')
		return NULL;
	if (asprintf(&path, "%s/.config/cow/%s.conf", home, prog) < 0)
		return NULL;
	return path;
}

char *
cow_module_config_dirname(const char *path)
{
	return cow_path_dirname(path);
}

char *
cow_module_config_resolve_path(const char *config_dir, const char *path)
{
	return cow_path_resolve(config_dir, path);
}

void
cow_module_config_free_fields(char **fields, int n)
{
	for (int i = 0; i < n; i++)
		free(fields[i]);
}

int
cow_module_config_parse_fields(const char *line, char **fields, int max_fields,
    char *err, size_t errsz)
{
	int n = 0;
	const char *p = line;

	while (*p != '\0') {
		while (*p == ' ' || *p == '\t' || *p == '\n')
			p++;
		if (*p == '\0' || *p == '#')
			break;
		if (n == max_fields) {
			snprintf(err, errsz, "too many fields");
			goto fail;
		}

		char buf[1024];
		size_t len = 0;
		if (*p == '"') {
			p++;
			while (*p != '\0' && *p != '"') {
				if (*p == '\\' && p[1] != '\0')
					p++;
				if (len + 1 >= sizeof(buf)) {
					snprintf(err, errsz,
					    "quoted field too long");
					goto fail;
				}
				buf[len++] = *p++;
			}
			if (*p != '"') {
				snprintf(err, errsz, "unterminated quote");
				goto fail;
			}
			p++;
		} else {
			while (*p != '\0' && *p != ' ' && *p != '\t' &&
			    *p != '\n') {
				if (*p == '#')
					break;
				if (len + 1 >= sizeof(buf)) {
					snprintf(err, errsz, "field too long");
					goto fail;
				}
				buf[len++] = *p++;
			}
		}
		buf[len] = '\0';
		fields[n] = strdup(buf);
		if (fields[n] == NULL) {
			snprintf(err, errsz, "out of memory");
			goto fail;
		}
		n++;
	}
	return n;

fail:
	cow_module_config_free_fields(fields, n);
	return -1;
}

bool
cow_module_config_parse_int(const char *s, int *out)
{
	char *end;
	long  n;

	if (s == NULL || *s == '\0')
		return false;
	errno = 0;
	n = strtol(s, &end, 10);
	if (errno != 0 || *end != '\0')
		return false;
	*out = (int)n;
	return true;
}

bool
cow_module_config_parse_bool(const char *s, bool *out)
{
	if (s == NULL)
		return false;
	if (strcmp(s, "true") == 0 || strcmp(s, "yes") == 0 ||
	    strcmp(s, "on") == 0 || strcmp(s, "1") == 0) {
		*out = true;
		return true;
	}
	if (strcmp(s, "false") == 0 || strcmp(s, "no") == 0 ||
	    strcmp(s, "off") == 0 || strcmp(s, "0") == 0) {
		*out = false;
		return true;
	}
	return false;
}

/* Comments must not swallow the next line when they end in a backslash.
 * Count escaping while scanning so a literal doubled backslash is retained.
 */
static bool
line_continues(const char *line, size_t len)
{
	char quote = 0;
	for (size_t i = 0; i < len; i++) {
		if (line[i] == '\\') {
			if (i + 1 == len)
				return true;
			i++;
		} else if (quote) {
			if (line[i] == quote)
				quote = 0;
		} else if (line[i] == '"' || line[i] == '\'')
			quote = line[i];
		else if (line[i] == '#' && line[i + 1] != '{')
			break;
	}
	return false;
}

/* Keep skipped lines blank so each parser reports original line numbers.
 * Only outer section wrappers are removed; their contents retain the
 * owning module's grammar.
 */
ssize_t
cow_module_config_getline(struct cow_module_config_reader *reader,
    char **line, size_t *cap, FILE *file)
{
	/* Return one blank for each joined physical line, after the logical
	 * line. Existing callers can keep counting from its starting line.
	 */
	if (reader->continuation_lines > 0) {
		reader->continuation_lines--;
		(*line)[0] = '\n';
		(*line)[1] = '\0';
		return 1;
	}
	ssize_t len = getline(line, cap, file);
	if (len < 0)
		return len;
	for (;;) {
		size_t end = (size_t)len;
		if (end > 0 && (*line)[end - 1] == '\n')
			end--;
		if (end > 0 && (*line)[end - 1] == '\r')
			end--;
		if (!line_continues(*line, end))
			break;
		char *next = NULL;
		size_t next_cap = 0;
		ssize_t n = getline(&next, &next_cap, file);
		if (n < 0) {
			free(next);
			break;
		}
		if ((size_t)n > SIZE_MAX - end - 1) {
			free(next);
			errno = ENOMEM;
			return -1;
		}
		size_t need = end + (size_t)n + 1;
		if (need > *cap) {
			char *grown = realloc(*line, need);
			if (grown == NULL) {
				free(next);
				return -1;
			}
			*line = grown;
			*cap = need;
		}
		(*line)[end - 1] = ' ';
		memcpy(*line + end, next, (size_t)n + 1);
		len = (ssize_t)(need - 1);
		reader->continuation_lines++;
		free(next);
	}
	bool emit = reader->section ? reader->selected : !reader->scoped_only;
	if (reader->depth == 0) {
		char *fields[4];
		char error[64];
		int n = cow_module_config_parse_fields(*line, fields, 4,
		    error, sizeof(error));
		const char *owner = NULL;
		if (n == 3 && !strcmp(fields[0], "module") &&
		    !strcmp(fields[2], "{"))
			owner = fields[1];
		else if (n == 2 && !strcmp(fields[0], "cow") &&
		    !strcmp(fields[1], "{"))
			owner = "cow";
		if (owner != NULL) {
			reader->section = true;
			reader->selected = !strcmp(owner, reader->prog);
			emit = false;
		}
		if (n >= 0)
			cow_module_config_free_fields(fields, n);
	}
	char quote = 0;
	for (const char *p = *line; *p; p++) {
		if (*p == '\\' && p[1]) {
			p++;
			continue;
		}
		if (quote) {
			if (*p == quote)
				quote = 0;
			continue;
		}
		if (*p == '"' || *p == '\'') {
			quote = *p;
			continue;
		}
		if (*p == '#' && p[1] != '{')
			break;
		if (*p == '{')
			reader->depth++;
		else if (*p == '}') {
			reader->depth--;
			if (reader->section && reader->depth == 0) {
				reader->section = false;
				emit = false;
			}
		}
	}
	if (!emit) {
		(*line)[0] = '\n';
		(*line)[1] = '\0';
		return 1;
	}
	return len;
}

bool
cow_module_config_load(const char *prog, const char *path, int max_fields,
    cow_module_config_cb cb, void *data)
{
	FILE *f;
	char *line = NULL;
	size_t cap = 0;
	unsigned int lineno = 0;
	bool ok = true;
	struct cow_module_config_reader reader = { .prog = prog };

	f = fopen(path, "r");
	if (f == NULL) {
		fprintf(stderr, "%s: %s: %s\n", prog, path, strerror(errno));
		return false;
	}

	while (cow_module_config_getline(&reader, &line, &cap, f) != -1) {
		char *fields[max_fields];
		char err[128] = "";
		const char *why = NULL;
		int n;

		memset(fields, 0, sizeof(fields));
		lineno++;
		n = cow_module_config_parse_fields(line, fields, max_fields,
		    err, sizeof(err));
		if (n < 0) {
			fprintf(stderr, "%s: %s:%u: %s\n", prog, path, lineno,
			    err);
			ok = false;
			continue;
		}
		if (n == 0) {
			cow_module_config_free_fields(fields, n);
			continue;
		}

		struct cow_module_config_line cl = {
			.prog = prog,
			.path = path,
			.lineno = lineno,
			.fields = fields,
			.nfields = n,
		};
		if (!cb(&cl, &why, data)) {
			fprintf(stderr, "%s: %s:%u: %s\n", prog, path, lineno,
			    why != NULL ? why : "bad config");
			ok = false;
		}
		cow_module_config_free_fields(fields, n);
	}

	if (reader.depth != 0 || ferror(f)) {
		fprintf(stderr, "%s: %s: incomplete configuration\n", prog, path);
		ok = false;
	}
	free(line);
	fclose(f);
	return ok;
}

bool
cow_module_config_instances_init(struct cow_module_config_instances *instances,
    const char *prog, const char *index_env)
{
	const char *value;
	int index;

	memset(instances, 0, sizeof(*instances));
	instances->prog = prog;
	instances->index_env = index_env;
	value = getenv(index_env);
	if (value == NULL || *value == '\0')
		return true;
	if (!cow_module_config_parse_int(value, &index) || index < 0) {
		fprintf(stderr, "%s: bad %s\n", prog, index_env);
		return false;
	}
	instances->index = index;
	instances->child = true;
	return true;
}

bool
cow_module_config_instances_select_next(
    struct cow_module_config_instances *instances)
{
	return instances->count++ == instances->index;
}

void
cow_module_config_instances_spawn(
    const struct cow_module_config_instances *instances, char *const argv[])
{
	if (instances->child)
		return;
	for (int i = 1; i < instances->count; i++) {
		pid_t pid = fork();

		if (pid < 0) {
			fprintf(stderr, "%s: fork: %s\n", instances->prog,
			    strerror(errno));
			continue;
		}
		if (pid != 0)
			continue;
		char index[32];
		snprintf(index, sizeof(index), "%d", i);
		setenv(instances->index_env, index, 1);
		execvp(argv[0], argv);
		_exit(127);
	}
}
