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
#if !defined(COW_UPDATE_H)
#define COW_UPDATE_H

#include <stdint.h>

enum cow_update_flags {
	COW_UPDATE_NONE   = 0,
	/* Pending policy must be applied in a River manage sequence. */
	COW_UPDATE_MANAGE = 1U << 0,
	/* Canonical state represented by the status snapshot changed. */
	COW_UPDATE_STATUS = 1U << 1,
};

/*
 * Register MANAGE when an operation creates pending work for River.  Register
 * STATUS where an authoritative CoW field changes, rather than where an
 * operation merely requests that future change.  Requests are coalesced until
 * the main event loop takes the accumulated mask.
 */

void	 cow_update_reset(void);
void	 cow_update_request_at(uint32_t, const char *, int, const char *);
uint32_t cow_update_take(void);
void	 cow_update_manage_begin(void);

#define cow_update_request(flags) \
	cow_update_request_at((flags), __FILE__, __LINE__, __func__)

#endif /* COW_UPDATE_H */
