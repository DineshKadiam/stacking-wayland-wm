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
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cJSON.h>

#include "cow/json_util.h"
#include "cow/shm_util.h"

#define CHECK(expr)                                                          \
	do {                                                                   \
		if (!(expr)) {                                                  \
			fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, \
			    __LINE__, #expr);                                    \
			abort();                                                   \
		}                                                              \
	} while (0)

static void
test_json_copy(void)
{
	cJSON *obj = cJSON_CreateObject();
	char buf[5] = "old";

	cJSON_AddStringToObject(obj, "value", "abcdef");
	CHECK(cow_json_copy_string(obj, "value", buf, sizeof(buf)));
	CHECK(strcmp(buf, "abcd") == 0);
	CHECK(!cow_json_copy_string(obj, "missing", buf, sizeof(buf)));
	CHECK(buf[0] == '\0');
	cJSON_Delete(obj);
}

static void
test_shm_size(void)
{
	int stride = 0;
	size_t size = 0;

	CHECK(cow_shm_argb8888_size(10, 20, &stride, &size));
	CHECK(stride == 40 && size == 800);
	CHECK(!cow_shm_argb8888_size(0, 20, &stride, &size));
	CHECK(!cow_shm_argb8888_size(INT_MAX, 20, &stride, &size));
}

int
main(void)
{
	test_json_copy();
	test_shm_size();
	return 0;
}
