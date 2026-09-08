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

#include "cow/cmd_scan.h"

void
cow_cmd_scan_init(struct cow_cmd_scan *scan)
{
	scan->brace_depth = 0;
	scan->in_squote   = false;
	scan->in_dquote   = false;
	scan->in_comment  = false;
	scan->escaping	   = false;
}

enum cow_cmd_scan_result
cow_cmd_scan_buffer(struct cow_cmd_scan *scan, const char *buf, size_t len,
    size_t *end)
{
	for (size_t i = 0; i < len; i++) {
		char c = buf[i];

		if (scan->in_comment) {
			if (c == '\n') {
				scan->in_comment = false;
				if (scan->brace_depth == 0) {
					if (end != NULL)
						*end = i;
					return (COW_CMD_SCAN_COMPLETE);
				}
			}
			continue;
		}
		if (scan->escaping) {
			scan->escaping = false;
			continue;
		}
		if (scan->in_squote) {
			if (c == '\'')
				scan->in_squote = false;
			continue;
		}
		if (scan->in_dquote) {
			if (c == '\\')
				scan->escaping = true;
			else if (c == '"')
				scan->in_dquote = false;
			continue;
		}

		if (c == '\'') {
			scan->in_squote = true;
			continue;
		}
		if (c == '"') {
			scan->in_dquote = true;
			continue;
		}
		/* #{...} is DSL interpolation, not a line comment.  Counting its
		 * braces is harmless (the pair balances) and ensures any command
		 * body following it on the same line is still seen.
		 */
		if (c == '#' && (i + 1 == len || buf[i + 1] != '{')) {
			scan->in_comment = true;
			continue;
		}
		if (c == '{') {
			scan->brace_depth++;
			continue;
		}
		if (c == '}') {
			if (scan->brace_depth > 0)
				scan->brace_depth--;
			continue;
		}
		if (c == '\n' && scan->brace_depth == 0) {
			if (end != NULL)
				*end = i;
			return (COW_CMD_SCAN_COMPLETE);
		}
	}

	return (COW_CMD_SCAN_INCOMPLETE);
}

enum cow_cmd_scan_result
cow_cmd_find_terminator(const char *buf, size_t len, size_t *end)
{
	struct cow_cmd_scan scan;

	cow_cmd_scan_init(&scan);
	return (cow_cmd_scan_buffer(&scan, buf, len, end));
}

enum cow_cmd_scan_result
cow_cmd_find_matching_brace(const char *buf, size_t len, size_t *end)
{
	struct cow_cmd_scan scan;

	if (len == 0 || buf[0] != '{')
		return (COW_CMD_SCAN_ERROR);

	cow_cmd_scan_init(&scan);
	for (size_t i = 0; i < len; i++) {
		char c = buf[i];

		if (scan.escaping) {
			scan.escaping = false;
			continue;
		}
		if (scan.in_squote) {
			if (c == '\'')
				scan.in_squote = false;
			continue;
		}
		if (scan.in_dquote) {
			if (c == '\\')
				scan.escaping = true;
			else if (c == '"')
				scan.in_dquote = false;
			continue;
		}

		if (c == '\'') {
			scan.in_squote = true;
			continue;
		}
		if (c == '"') {
			scan.in_dquote = true;
			continue;
		}
		if (c == '#' && (i + 1 == len || buf[i + 1] != '{')) {
			while (i + 1 < len && buf[i + 1] != '\n')
				i++;
			continue;
		}
		if (c == '{') {
			scan.brace_depth++;
			continue;
		}
		if (c == '}' && scan.brace_depth > 0) {
			scan.brace_depth--;
			if (scan.brace_depth == 0) {
				if (end != NULL)
					*end = i;
				return (COW_CMD_SCAN_COMPLETE);
			}
		}
	}

	return (COW_CMD_SCAN_INCOMPLETE);
}
