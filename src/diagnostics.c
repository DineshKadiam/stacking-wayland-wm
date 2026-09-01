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
#include "cow/diagnostics.h"

#include <cJSON.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

#include "cow/update.h"

struct diag_series {
	uint64_t count;
	uint64_t total_us;
	uint64_t last_us;
	uint64_t max_us;
};

struct diag_state {
	uint64_t started_us;
	uint64_t first_manage_request_us;
	uint64_t manage_started_us;
	uint64_t manage_finished_us;
	uint64_t render_started_us;
	uint64_t manage_requests;
	uint64_t status_requests;
	uint64_t manage_cycles;
	uint64_t requested_manage_cycles;
	uint64_t compositor_manage_cycles;
	uint64_t coalesced_manage_requests;
	uint64_t render_cycles;
	uint64_t status_snapshots;
	uint32_t pending;
	int	 status_clients;
	bool	 manage_request_pending;
	bool	 manage_to_render_pending;
	struct diag_series manage_wait;
	struct diag_series manage_duration;
	struct diag_series manage_to_render;
	struct diag_series render_duration;
};

static struct diag_state diagnostics;

static uint64_t
now_us(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000ULL +
	    (uint64_t)ts.tv_nsec / 1000ULL;
}

static void
series_add(struct diag_series *series, uint64_t value)
{
	series->count++;
	series->total_us += value;
	series->last_us = value;
	if (value > series->max_us)
		series->max_us = value;
}

static void
json_add_series(cJSON *parent, const char *name,
    const struct diag_series *series)
{
	cJSON *obj = cJSON_AddObjectToObject(parent, name);

	cJSON_AddNumberToObject(obj, "samples", (double)series->count);
	if (series->count == 0) {
		cJSON_AddNullToObject(obj, "last_us");
		cJSON_AddNullToObject(obj, "average_us");
		cJSON_AddNullToObject(obj, "maximum_us");
		return;
	}
	cJSON_AddNumberToObject(obj, "last_us", (double)series->last_us);
	cJSON_AddNumberToObject(obj, "average_us",
	    (double)(series->total_us / series->count));
	cJSON_AddNumberToObject(obj, "maximum_us", (double)series->max_us);
}

void
cow_diagnostics_init(void)
{
	diagnostics = (struct diag_state){ 0 };
	diagnostics.started_us = now_us();
}

void
cow_diagnostics_update_request(uint32_t flags)
{
	uint64_t now = now_us();

	if ((flags & COW_UPDATE_MANAGE) != 0) {
		diagnostics.manage_requests++;
		if (!diagnostics.manage_request_pending) {
			diagnostics.first_manage_request_us = now;
			diagnostics.manage_request_pending = true;
		}
	}
	if ((flags & COW_UPDATE_STATUS) != 0)
		diagnostics.status_requests++;
	diagnostics.pending |= flags;
}

void
cow_diagnostics_update_take(uint32_t flags)
{
	diagnostics.pending &= ~flags;
}

void
cow_diagnostics_manage_begin(unsigned requests)
{
	uint64_t now = now_us();

	diagnostics.manage_cycles++;
	diagnostics.manage_started_us = now;
	if (requests > 0 || diagnostics.manage_request_pending) {
		diagnostics.requested_manage_cycles++;
		if (requests > 1)
			diagnostics.coalesced_manage_requests += requests - 1;
		if (diagnostics.manage_request_pending)
			series_add(&diagnostics.manage_wait,
			    now - diagnostics.first_manage_request_us);
	} else {
		diagnostics.compositor_manage_cycles++;
	}
	diagnostics.manage_request_pending = false;
}

void
cow_diagnostics_manage_end(void)
{
	uint64_t now = now_us();

	if (diagnostics.manage_started_us != 0)
		series_add(&diagnostics.manage_duration,
		    now - diagnostics.manage_started_us);
	diagnostics.manage_finished_us = now;
	diagnostics.manage_to_render_pending = true;
	diagnostics.manage_started_us = 0;
}

void
cow_diagnostics_render_begin(void)
{
	uint64_t now = now_us();

	diagnostics.render_cycles++;
	diagnostics.render_started_us = now;
	if (diagnostics.manage_to_render_pending) {
		series_add(&diagnostics.manage_to_render,
		    now - diagnostics.manage_finished_us);
		diagnostics.manage_to_render_pending = false;
	}
}

void
cow_diagnostics_render_end(void)
{
	uint64_t now = now_us();

	if (diagnostics.render_started_us != 0)
		series_add(&diagnostics.render_duration,
		    now - diagnostics.render_started_us);
	diagnostics.render_started_us = 0;
}

void
cow_diagnostics_status_emit(void)
{
	diagnostics.status_snapshots++;
}

void
cow_diagnostics_status_clients(int clients)
{
	diagnostics.status_clients = clients;
}

struct cJSON *
cow_diagnostics_json(void)
{
	struct rusage usage = { 0 };
	uint64_t now = now_us();
	cJSON *root = cJSON_CreateObject();
	cJSON *process = cJSON_AddObjectToObject(root, "process");

	(void)getrusage(RUSAGE_SELF, &usage);
	cJSON_AddNumberToObject(process, "pid", (double)getpid());
	cJSON_AddNumberToObject(process, "uptime_ms",
	    (double)((now - diagnostics.started_us) / 1000ULL));
	cJSON_AddNumberToObject(process, "user_cpu_us",
	    (double)usage.ru_utime.tv_sec * 1000000.0 + usage.ru_utime.tv_usec);
	cJSON_AddNumberToObject(process, "system_cpu_us",
	    (double)usage.ru_stime.tv_sec * 1000000.0 + usage.ru_stime.tv_usec);
	cJSON_AddNumberToObject(process, "peak_rss_bytes",
	    (double)usage.ru_maxrss * 1024.0);

	cJSON *updates = cJSON_AddObjectToObject(root, "updates");
	cJSON_AddNumberToObject(updates, "manage_requests",
	    (double)diagnostics.manage_requests);
	cJSON_AddNumberToObject(updates, "status_requests",
	    (double)diagnostics.status_requests);
	cJSON_AddBoolToObject(updates, "manage_pending",
	    diagnostics.manage_request_pending ||
	    (diagnostics.pending & COW_UPDATE_MANAGE) != 0);
	cJSON_AddBoolToObject(updates, "status_pending",
	    (diagnostics.pending & COW_UPDATE_STATUS) != 0);

	cJSON *manage = cJSON_AddObjectToObject(root, "manage");
	cJSON_AddNumberToObject(manage, "cycles",
	    (double)diagnostics.manage_cycles);
	cJSON_AddNumberToObject(manage, "requested_cycles",
	    (double)diagnostics.requested_manage_cycles);
	cJSON_AddNumberToObject(manage, "compositor_cycles",
	    (double)diagnostics.compositor_manage_cycles);
	cJSON_AddNumberToObject(manage, "coalesced_requests",
	    (double)diagnostics.coalesced_manage_requests);
	json_add_series(manage, "request_wait", &diagnostics.manage_wait);
	json_add_series(manage, "duration", &diagnostics.manage_duration);

	cJSON *render = cJSON_AddObjectToObject(root, "render");
	cJSON_AddNumberToObject(render, "cycles",
	    (double)diagnostics.render_cycles);
	json_add_series(render, "after_manage", &diagnostics.manage_to_render);
	json_add_series(render, "duration", &diagnostics.render_duration);

	cJSON *status = cJSON_AddObjectToObject(root, "status");
	cJSON_AddNumberToObject(status, "snapshots",
	    (double)diagnostics.status_snapshots);
	cJSON_AddNumberToObject(status, "clients", diagnostics.status_clients);

	return root;
}
