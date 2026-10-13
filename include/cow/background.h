/*
 * Copyright (c) 2026 Thomas Adam
 *
 * Permission to use, copy, modify, and distribute this software for any
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
#ifndef COW_BACKGROUND_H
#define COW_BACKGROUND_H

#include <stdbool.h>

struct cow_config;
struct cow_output;
struct cow_desk;
struct cJSON;
struct cow_pixbuf;

void cow_background_fini(struct cow_config *);
bool cow_background_colour(const struct cow_output *, const struct cow_desk *,
    int, int, float[4]);
struct cJSON *cow_background_snapshot(const struct cow_output *);
/* Configured global/output property, excluding desk and page overrides. */
struct cJSON *cow_background_config_value(const char *, bool);
void cow_background_paint(struct cow_output *, struct cow_pixbuf *);

#endif
