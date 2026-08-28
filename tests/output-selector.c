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

#include <cJSON.h>
#include <stdio.h>

#include "cow/module_output.h"
#include "cow/output_selector.h"

#define CHECK(expression) do {                                            \
	if (!(expression)) {                                                \
		fprintf(stderr, "check failed at line %d: %s\n", __LINE__,   \
		    #expression);                                             \
		return 1;                                                    \
	}                                                                    \
} while (0)

int
main(void)
{
	const struct cow_output_selector_entry outputs[] = {
		{ .name = "HDMI-A-1", .number = 1 },
		{ .name = "DP-1", .number = 2, .current = true },
		{ .name = "eDP-1", .number = 3 },
	};
	const size_t count = sizeof(outputs) / sizeof(outputs[0]);

	CHECK(cow_output_selector_resolve(NULL, outputs, count) == 1);
	CHECK(cow_output_selector_resolve("", outputs, count) == 1);
	CHECK(cow_output_selector_resolve("@current", outputs, count) == 1);
	CHECK(cow_output_selector_resolve("@next", outputs, count) == 2);
	CHECK(cow_output_selector_resolve("@prev", outputs, count) == 0);
	CHECK(cow_output_selector_resolve("@1", outputs, count) == 0);
	CHECK(cow_output_selector_resolve("@3", outputs, count) == 2);
	CHECK(cow_output_selector_resolve("DP-1", outputs, count) == 1);
	CHECK(cow_output_selector_resolve("@0", outputs, count) == -1);
	CHECK(cow_output_selector_resolve("@4", outputs, count) == -1);
	CHECK(cow_output_selector_resolve("missing", outputs, count) == -1);
	CHECK(cow_output_selector_resolve("@999999999999999999999", outputs,
	    count) == -1);
	CHECK(cow_output_selector_resolve("@current", NULL, 0) == -1);
	CHECK(cow_output_selector_is_number("@1"));
	CHECK(!cow_output_selector_is_number("@current"));

	struct cow_output_selector_entry first_current[] = {
		{ .name = "HDMI-A-1", .number = 1, .current = true },
		{ .name = "DP-1", .number = 2 },
		{ .name = "eDP-1", .number = 3 },
	};
	CHECK(cow_output_selector_resolve("@prev", first_current, count) == 2);
	first_current[0].current = false;
	first_current[2].current = true;
	CHECK(cow_output_selector_resolve("@next", first_current, count) == 0);

	cJSON *json = cJSON_Parse("[{\"name\":\"HDMI-A-1\",\"number\":1},"
	    "{\"name\":\"DP-1\",\"number\":2,\"current\":true}]");
	CHECK(json != NULL);
	cJSON *selected = cow_module_output_resolve(json, "@current");
	CHECK(selected == cJSON_GetArrayItem(json, 1));
	selected = cow_module_output_resolve(json, "@1");
	CHECK(selected == cJSON_GetArrayItem(json, 0));
	cJSON_Delete(json);

	return 0;
}
