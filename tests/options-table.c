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

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cow/options.h"

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
	for (enum cow_view_cfg_option option = 0;
	    option < COW_VIEW_CFG_OPTION_COUNT; option++) {
		const struct cow_option_entry *entry =
		    cow_option_for_view_cfg(option);
		if (entry != NULL)
			CHECK(entry->view_offset >=
			    offsetof(struct cow_view_cfg, border));
	}
	CHECK(cow_option_for_view_cfg(COW_VIEW_CFG_BORDER_RESIZE) == NULL);
	const struct cow_option_entry *titlebar =
	    cow_option_for_view_cfg(COW_VIEW_CFG_TITLEBAR_ENABLED);
	CHECK(titlebar != NULL);
	CHECK(strcmp(titlebar->name, "titlebar.enabled") == 0);
	return 0;
}
