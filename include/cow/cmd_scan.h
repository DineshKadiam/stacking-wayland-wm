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
#if !defined(COW_CMD_SCAN_H)
#define COW_CMD_SCAN_H

#include <stdbool.h>
#include <stddef.h>

struct cow_cmd_scan {
	int  brace_depth;
	bool in_squote;
	bool in_dquote;
	bool in_comment;
	bool escaping;
};

enum cow_cmd_scan_result {
	COW_CMD_SCAN_COMPLETE,
	COW_CMD_SCAN_INCOMPLETE,
	COW_CMD_SCAN_ERROR,
};

void cow_cmd_scan_init(struct cow_cmd_scan *);
enum cow_cmd_scan_result cow_cmd_scan_buffer(struct cow_cmd_scan *,
    const char *, size_t, size_t *);
enum cow_cmd_scan_result cow_cmd_find_terminator(const char *, size_t,
    size_t *);
enum cow_cmd_scan_result cow_cmd_find_matching_brace(const char *, size_t,
    size_t *);

#endif /* COW_CMD_SCAN_H */
