/*
 * Copyright (c) 2026 Thomas Adam
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH
 * REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND
 * FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT,
 * INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
 * LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR
 * OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */
#include "cow/gradient.h"

#include <stddef.h>

#include "cow/attrs.h"

struct cow_border_gradient_entry {
	enum cow_view_cfg_option option;
	size_t			 offset;
};

static const struct cow_border_gradient_entry cow_border_gradients[] = {
	{ COW_VIEW_CFG_BORDER_ACTIVE_GRADIENT,
	    offsetof(struct cow_border_colour, gradient) },
	{ COW_VIEW_CFG_BORDER_INACTIVE_GRADIENT,
	    offsetof(struct cow_border_colour, gradient) },
	{ COW_VIEW_CFG_BORDER_ACTIVE_GRADIENT_N,
	    offsetof(struct cow_border_colour, gradient_n) },
	{ COW_VIEW_CFG_BORDER_INACTIVE_GRADIENT_N,
	    offsetof(struct cow_border_colour, gradient_n) },
	{ COW_VIEW_CFG_BORDER_ACTIVE_GRADIENT_S,
	    offsetof(struct cow_border_colour, gradient_s) },
	{ COW_VIEW_CFG_BORDER_INACTIVE_GRADIENT_S,
	    offsetof(struct cow_border_colour, gradient_s) },
	{ COW_VIEW_CFG_BORDER_ACTIVE_GRADIENT_E,
	    offsetof(struct cow_border_colour, gradient_e) },
	{ COW_VIEW_CFG_BORDER_INACTIVE_GRADIENT_E,
	    offsetof(struct cow_border_colour, gradient_e) },
	{ COW_VIEW_CFG_BORDER_ACTIVE_GRADIENT_W,
	    offsetof(struct cow_border_colour, gradient_w) },
	{ COW_VIEW_CFG_BORDER_INACTIVE_GRADIENT_W,
	    offsetof(struct cow_border_colour, gradient_w) },
	{ COW_VIEW_CFG_BORDER_ACTIVE_GRADIENT_NW,
	    offsetof(struct cow_border_colour, gradient_nw) },
	{ COW_VIEW_CFG_BORDER_INACTIVE_GRADIENT_NW,
	    offsetof(struct cow_border_colour, gradient_nw) },
	{ COW_VIEW_CFG_BORDER_ACTIVE_GRADIENT_NE,
	    offsetof(struct cow_border_colour, gradient_ne) },
	{ COW_VIEW_CFG_BORDER_INACTIVE_GRADIENT_NE,
	    offsetof(struct cow_border_colour, gradient_ne) },
	{ COW_VIEW_CFG_BORDER_ACTIVE_GRADIENT_SW,
	    offsetof(struct cow_border_colour, gradient_sw) },
	{ COW_VIEW_CFG_BORDER_INACTIVE_GRADIENT_SW,
	    offsetof(struct cow_border_colour, gradient_sw) },
	{ COW_VIEW_CFG_BORDER_ACTIVE_GRADIENT_SE,
	    offsetof(struct cow_border_colour, gradient_se) },
	{ COW_VIEW_CFG_BORDER_INACTIVE_GRADIENT_SE,
	    offsetof(struct cow_border_colour, gradient_se) },
};

static const struct cow_border_gradient_entry *
cow_border_gradient_entry_for_option(enum cow_view_cfg_option option)
{
	for (size_t i = 0; i < nitems(cow_border_gradients); i++) {
		if (cow_border_gradients[i].option == option)
			return (&cow_border_gradients[i]);
	}
	return (NULL);
}

struct cow_gradient *
cow_border_gradient_for_option(struct cow_border_colour *bc,
    enum cow_view_cfg_option option)
{
	const struct cow_border_gradient_entry *entry =
	    cow_border_gradient_entry_for_option(option);

	if (entry == NULL)
		return (NULL);
	return ((struct cow_gradient *)((char *)bc + entry->offset));
}

static const struct cow_gradient *
cow_border_gradient_const_for_option(const struct cow_border_colour *bc,
    enum cow_view_cfg_option option)
{
	const struct cow_border_gradient_entry *entry =
	    cow_border_gradient_entry_for_option(option);

	if (entry == NULL)
		return (NULL);
	return ((const struct cow_gradient *)((const char *)bc + entry->offset));
}

bool
cow_border_gradient_copy_for_option(struct cow_border_colour *dst,
    const struct cow_border_colour *src, enum cow_view_cfg_option option)
{
	struct cow_gradient	 *d = cow_border_gradient_for_option(dst, option);
	const struct cow_gradient *s =
	    cow_border_gradient_const_for_option(src, option);

	if (d == NULL || s == NULL)
		return (false);
	*d = *s;
	return (true);
}
