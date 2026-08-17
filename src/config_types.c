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
#include "cow/config.h"

#include "cow/memory.h"

void
cow_border_colour_copy(struct cow_border_colour *dst,
    const struct cow_border_colour *src)
{
	*dst = *src;
	dst->image = xstrdup0(src->image);
	dst->image_n = xstrdup0(src->image_n);
	dst->image_s = xstrdup0(src->image_s);
	dst->image_e = xstrdup0(src->image_e);
	dst->image_w = xstrdup0(src->image_w);
	dst->image_nw = xstrdup0(src->image_nw);
	dst->image_ne = xstrdup0(src->image_ne);
	dst->image_sw = xstrdup0(src->image_sw);
	dst->image_se = xstrdup0(src->image_se);
}

void
cow_border_colour_fini(struct cow_border_colour *colour)
{
	if (colour == NULL)
		return;
	xfree(colour->image);
	xfree(colour->image_n);
	xfree(colour->image_s);
	xfree(colour->image_e);
	xfree(colour->image_w);
	xfree(colour->image_nw);
	xfree(colour->image_ne);
	xfree(colour->image_sw);
	xfree(colour->image_se);
	colour->image = colour->image_n = colour->image_s = NULL;
	colour->image_e = colour->image_w = colour->image_nw = NULL;
	colour->image_ne = colour->image_sw = colour->image_se = NULL;
}

void
cow_titlebar_config_copy(struct cow_titlebar_config *dst,
    const struct cow_titlebar_config *src)
{
	*dst = *src;
	dst->active_image = xstrdup0(src->active_image);
	dst->inactive_image = xstrdup0(src->inactive_image);
	dst->font_name = xstrdup0(src->font_name);
	for (int i = 0; i < COW_TITLEBAR_NUM_BUTTONS; i++)
		dst->buttons[i].image = xstrdup0(src->buttons[i].image);
}

void
cow_titlebar_config_fini(struct cow_titlebar_config *titlebar)
{
	if (titlebar == NULL)
		return;
	xfree(titlebar->active_image);
	xfree(titlebar->inactive_image);
	xfree(titlebar->font_name);
	titlebar->active_image = NULL;
	titlebar->inactive_image = NULL;
	titlebar->font_name = NULL;
	for (int i = 0; i < COW_TITLEBAR_NUM_BUTTONS; i++) {
		xfree(titlebar->buttons[i].image);
		titlebar->buttons[i].image = NULL;
	}
}

void
cow_container_config_copy(struct cow_container_config *dst,
    const struct cow_container_config *src)
{
	*dst = *src;
	dst->tab_active_image = xstrdup0(src->tab_active_image);
	dst->tab_inactive_image = xstrdup0(src->tab_inactive_image);
}

void
cow_container_config_fini(struct cow_container_config *container)
{
	if (container == NULL)
		return;
	xfree(container->tab_active_image);
	xfree(container->tab_inactive_image);
	container->tab_active_image = NULL;
	container->tab_inactive_image = NULL;
}
