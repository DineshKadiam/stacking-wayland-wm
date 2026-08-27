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

#include <stdint.h>

#include "cow/log.h"
#include "cow/update.h"

struct cow_update_state {
	uint32_t    pending;
	unsigned    manage_requests;
	const char *last_manage_file;
	const char *last_manage_function;
	int	    last_manage_line;
};

static struct cow_update_state update_state;

void
cow_update_reset(void)
{
	update_state = (struct cow_update_state){ 0 };
}

void
cow_update_request_at(uint32_t flags, const char *file, int line,
    const char *function)
{
	if ((flags & COW_UPDATE_MANAGE) != 0) {
		update_state.manage_requests++;
		update_state.last_manage_file = file;
		update_state.last_manage_line = line;
		update_state.last_manage_function = function;
		cow_log_trace("manage cycle requested by %s:%d %s", file, line,
		    function);
	}
	update_state.pending |= flags;
}

uint32_t
cow_update_take(void)
{
	uint32_t pending = update_state.pending;
	update_state.pending = COW_UPDATE_NONE;
	return pending;
}

void
cow_update_manage_begin(void)
{
	/*
	 * The new River sequence satisfies any manage update queued before it.
	 * Requests made by work within the sequence are registered afterwards
	 * and therefore remain pending for a follow-up sequence.
	 */
	update_state.pending &= ~COW_UPDATE_MANAGE;

	if (update_state.manage_requests == 0) {
		cow_log_trace("manage cycle begin (trigger=compositor)");
	} else {
		cow_log_trace("manage cycle begin (requests=%u "
		    "last_request=%s:%d %s)", update_state.manage_requests,
		    update_state.last_manage_file, update_state.last_manage_line,
		    update_state.last_manage_function);
	}
	update_state.manage_requests = 0;
}
