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
#ifndef COW_DIAGNOSTICS_H
#define COW_DIAGNOSTICS_H

#include <stdint.h>

struct cJSON;

void cow_diagnostics_init(void);
void cow_diagnostics_update_request(uint32_t);
void cow_diagnostics_update_take(uint32_t);
void cow_diagnostics_manage_begin(unsigned);
void cow_diagnostics_manage_end(void);
void cow_diagnostics_render_begin(void);
void cow_diagnostics_render_end(void);
void cow_diagnostics_status_emit(void);
void cow_diagnostics_status_clients(int);

struct cJSON *cow_diagnostics_json(void);

#endif /* COW_DIAGNOSTICS_H */
