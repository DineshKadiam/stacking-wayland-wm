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
#include <string.h>

#include "cow/config.h"
#include "cow/memory.h"

#define CHECK(expr)                                                          \
	do {                                                                   \
		if (!(expr)) {                                                  \
			fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, \
			    __LINE__, #expr);                                    \
			abort();                                                   \
		}                                                              \
	} while (0)

static void
test_string_helpers(void)
{
	char *value = NULL;

	xstrset(&value, "hello");
	CHECK(strcmp(value, "hello") == 0);
	xstrset(&value, value);
	CHECK(strcmp(value, "hello") == 0);
	xstrset(&value, NULL);
	CHECK(value == NULL);
}

static void
test_border_copy(void)
{
	struct cow_border_colour src = { 0 }, dst;
	src.image = xstrdup("all");
	src.image_ne = xstrdup("ne");

	cow_border_colour_copy(&dst, &src);
	CHECK(strcmp(dst.image, src.image) == 0 && dst.image != src.image);
	CHECK(strcmp(dst.image_ne, src.image_ne) == 0 &&
	    dst.image_ne != src.image_ne);
	cow_border_colour_fini(&src);
	cow_border_colour_fini(&dst);
	CHECK(src.image == NULL && src.image_ne == NULL);
	CHECK(dst.image == NULL && dst.image_ne == NULL);
}

static void
test_titlebar_and_container_copy(void)
{
	struct cow_titlebar_config titlebar = { 0 }, titlebar_copy;
	struct cow_container_config container = { 0 }, container_copy;
	titlebar.font_name = xstrdup("sans 10");
	titlebar.buttons[3].image = xstrdup("button.svg");
	container.tab_active_image = xstrdup("tab.svg");

	cow_titlebar_config_copy(&titlebar_copy, &titlebar);
	cow_container_config_copy(&container_copy, &container);
	CHECK(titlebar_copy.font_name != titlebar.font_name);
	CHECK(titlebar_copy.buttons[3].image != titlebar.buttons[3].image);
	CHECK(container_copy.tab_active_image != container.tab_active_image);
	cow_titlebar_config_fini(&titlebar);
	cow_titlebar_config_fini(&titlebar_copy);
	cow_container_config_fini(&container);
	cow_container_config_fini(&container_copy);
}

int
main(void)
{
	test_string_helpers();
	test_border_copy();
	test_titlebar_and_container_copy();
	return 0;
}
