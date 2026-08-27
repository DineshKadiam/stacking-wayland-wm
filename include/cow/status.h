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
#if !defined(COW_STATUS_H)
#define COW_STATUS_H

struct cow_view;
struct cow_output;
struct cJSON;

/*
 * Initialize CoW's per-display status socket.
 */
void cow_status_init(void);

/*
 * Close and remove the status socket.
 */
void cow_status_fini(void);

/* Emit a full state snapshot to connected clients. */
void cow_status_emit(void);

void cow_status_add_output_geometry(struct cJSON *, const struct cow_output *);
void cow_status_add_output_desks(struct cJSON *, const struct cow_output *);
void cow_status_add_output_pages(struct cJSON *, const struct cow_output *);

#endif
