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
#ifndef COW_CONTAINER_INTERNAL_H
#define COW_CONTAINER_INTERNAL_H

#include "cow/container.h"

/* Shared only by the runtime container and restart-state implementation. */
struct cow_container_pane *container_pane_from_layout(
    struct cow_container_layout *);
struct cow_container_pane *container_pane_create(void);
void			   container_pane_destroy(struct cow_container_pane *);
bool	       container_pane_minimum(struct cow_container_pane *, bool);
struct cow_box container_pane_content(const struct cow_container_member *);
bool container_frame_delta(struct cow_box, struct cow_box, struct cow_box,
    struct cow_box *);
void container_apply_layout(struct cow_container *);
int  container_allocate_id(void);

#endif
