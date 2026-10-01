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
#if !defined(COW_MODULE_CONFIG_H)
#define COW_MODULE_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <sys/types.h>

struct cow_module_config_reader {
	const char *prog;
	int depth;
	bool section;
	bool selected;
	bool scoped_only;
	unsigned int continuation_lines;
};

ssize_t cow_module_config_getline(struct cow_module_config_reader *,
    char **, size_t *, FILE *);

struct cow_module_config_line {
	const char   *prog;
	const char   *path;
	unsigned int  lineno;
	char	    **fields;
	int	      nfields;
};

struct cow_module_config_instances {
	const char *prog;
	const char *index_env;
	int	    index;
	int	    count;
	bool	    child;
};

typedef bool (*cow_module_config_cb)(const struct cow_module_config_line *,
    const char **, void *);

char *cow_module_config_default_path(const char *);
char *cow_module_config_dirname(const char *);
char *cow_module_config_resolve_path(const char *, const char *);

void cow_module_config_free_fields(char **, int);
int  cow_module_config_parse_fields(const char *, char **, int, char *,
    size_t);

bool cow_module_config_parse_int(const char *, int *);
bool cow_module_config_parse_bool(const char *, bool *);

bool cow_module_config_load(const char *, const char *, int,
    cow_module_config_cb, void *);

bool cow_module_config_instances_init(struct cow_module_config_instances *,
    const char *, const char *);
bool cow_module_config_instances_select_next(
    struct cow_module_config_instances *);
void cow_module_config_instances_spawn(
    const struct cow_module_config_instances *, char *const []);

#endif /* COW_MODULE_CONFIG_H */
