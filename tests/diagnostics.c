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

#include "cow/diagnostics.h"
#include "cow/update.h"

static cJSON *
item(cJSON *object, const char *name)
{
	return cJSON_GetObjectItemCaseSensitive(object, name);
}

#define CHECK(condition) do { \
	if (!(condition)) { \
		fprintf(stderr, "check failed at line %d: %s\n", __LINE__, \
		    #condition); \
		return 1; \
	} \
} while (0)

int
main(void)
{
	cow_diagnostics_init();
	cow_diagnostics_update_request(COW_UPDATE_MANAGE);
	cow_diagnostics_update_request(COW_UPDATE_MANAGE | COW_UPDATE_STATUS);
	cow_diagnostics_update_take(COW_UPDATE_MANAGE | COW_UPDATE_STATUS);
	cow_diagnostics_manage_begin(2);
	cow_diagnostics_manage_end();
	cow_diagnostics_render_begin();
	cow_diagnostics_render_end();
	cow_diagnostics_status_clients(2);
	cow_diagnostics_status_emit();

	cJSON *root = cow_diagnostics_json();
	CHECK(cJSON_IsObject(root));
	CHECK(item(item(root, "process"), "pid")->valueint > 0);
	cJSON *updates = item(root, "updates");
	CHECK(item(updates, "manage_requests")->valueint == 2);
	CHECK(item(updates, "status_requests")->valueint == 1);
	CHECK(!cJSON_IsTrue(item(updates, "manage_pending")));
	cJSON *manage = item(root, "manage");
	CHECK(item(manage, "cycles")->valueint == 1);
	CHECK(item(manage, "requested_cycles")->valueint == 1);
	CHECK(item(manage, "coalesced_requests")->valueint == 1);
	CHECK(item(item(manage, "request_wait"), "samples")->valueint == 1);
	CHECK(item(item(manage, "duration"), "samples")->valueint == 1);
	cJSON *render = item(root, "render");
	CHECK(item(render, "cycles")->valueint == 1);
	CHECK(item(item(render, "after_manage"), "samples")->valueint == 1);
	CHECK(item(item(render, "duration"), "samples")->valueint == 1);
	cJSON *status = item(root, "status");
	CHECK(item(status, "clients")->valueint == 2);
	CHECK(item(status, "snapshots")->valueint == 1);
	cJSON_Delete(root);
	return 0;
}
