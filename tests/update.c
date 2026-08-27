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

#include <stdio.h>
#include <stdlib.h>

#include "cow/update.h"

#define CHECK(expr)                                                          \
	do {                                                                   \
		if (!(expr)) {                                                  \
			fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, \
			    __LINE__, #expr);                                    \
			abort();                                                   \
		}                                                              \
	} while (0)

int
main(void)
{
	cow_update_reset();
	CHECK(cow_update_take() == COW_UPDATE_NONE);

	cow_update_request(COW_UPDATE_STATUS);
	cow_update_request(COW_UPDATE_STATUS);
	CHECK(cow_update_take() == COW_UPDATE_STATUS);
	CHECK(cow_update_take() == COW_UPDATE_NONE);

	cow_update_request(COW_UPDATE_MANAGE | COW_UPDATE_STATUS);
	CHECK(cow_update_take() ==
	    (COW_UPDATE_MANAGE | COW_UPDATE_STATUS));

	cow_update_request(COW_UPDATE_MANAGE);
	cow_update_manage_begin();
	CHECK(cow_update_take() == COW_UPDATE_NONE);

	return 0;
}
