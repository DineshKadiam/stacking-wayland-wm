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
#if !defined(COW_POLICY_H)
#define COW_POLICY_H

#include <stdbool.h>

struct cow_desk;
struct cow_output;
struct cow_view;

/* Matching and scope shared by executable rules and declarative styles. */
struct cow_policy {
	char *name;
	bool  is_global;
	char *output_name;
	int   desk_nr;
	char *desk_name;
	char *match_spec;
};

void cow_policy_init(struct cow_policy *);
void cow_policy_fini(struct cow_policy *);
bool cow_policy_parse_scope(const char *, struct cow_policy *, char **);
bool cow_policy_scope_matches(const struct cow_policy *,
    const struct cow_desk *, const struct cow_output *);
bool cow_policy_view_matches(const struct cow_policy *,
    const struct cow_view *);
bool cow_policy_matches_view(const struct cow_policy *,
    const struct cow_view *);

#endif /* COW_POLICY_H */
