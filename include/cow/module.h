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
#if !defined(COW_MODULE_H)
#define COW_MODULE_H

#include <stdbool.h>
#include <stddef.h>

#include "cow/module_protocol.h"

#define COW_MODULE_SOCKET_PATH_MAX     4096
#define COW_MODULE_RESPONSE_INITIAL    65536
#define COW_MODULE_STATUS_LINE_MAX     131072
#define COW_MODULE_STATUS_RETRIES      30
#define COW_MODULE_STATUS_RETRY_DELAY  1
#define COW_MODULE_ACK_BUFFER_SIZE     4096

bool  cow_module_cmd_path(char *, size_t, const char *);
bool  cow_module_status_path(char *, size_t, const char *);
int   cow_module_connect_path(const char *, const char *);
int   cow_module_cmd_connect(const char *);
int   cow_module_cmd_connect_quiet(const char *);
int   cow_module_status_connect(const char *, int, unsigned int);
int   cow_module_status_connect_quiet(const char *);
bool  cow_module_cmd_send(int, const char *, bool);
char *cow_module_read_response(int, size_t, bool);
char *cow_module_cmd_request(const char *, const char *, bool);
bool  cow_module_cmd_execute(const char *, const char *, bool);

#endif
