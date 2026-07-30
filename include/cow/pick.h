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
#ifndef COW_PICK_H
#define COW_PICK_H

#include <stdbool.h>
#include <stdint.h>

#include <wayland-util.h>

struct wl_surface;

bool cow_pick_pointer_enter(struct wl_surface *, uint32_t, wl_fixed_t,
    wl_fixed_t);
bool cow_pick_pointer_leave(struct wl_surface *);
bool cow_pick_pointer_motion(uint32_t, wl_fixed_t, wl_fixed_t);
bool cow_pick_pointer_button(uint32_t, uint32_t, uint32_t);

/*
 * Cancel the active pick session, if any.  No-op when no session is
 * running.  Used by output add/remove paths and shutdown.
 */
void cow_pick_cancel_active(void);
void cow_pick_run_deferred(void);

#endif /* COW_PICK_H */
