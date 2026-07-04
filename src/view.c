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
#include "cow/view.h"

#include <cJSON.h>
#include <cairo/cairo.h>
#include <fnmatch.h>
#include <pango/pangocairo.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cow/colour.h"
#include "cow/cmd.h"
#include "cow/compat.h"
#include "cow/config.h"
#include "cow/decoration.h"
#include "cow/desk_ops.h"
#include "cow/draw.h"
#include "cow/gradient.h"
#include "cow/image.h"
#include "cow/keybinding.h"
#include "cow/memory.h"
#include "cow/output.h"
#include "cow/rule.h"
#include "cow/pointer_op.h"
#include "cow/seat.h"
#include "cow/shm.h"
#include "cow/stack.h"
#include "cow/status.h"
#include "cow/util.h"
#include "cow/wm.h"

#include "river-window-management-v1-client-protocol.h"

static int next_container_id;

static int
titlebar_button_vector_index(enum cow_view_cfg_option option)
{
	if (option < COW_VIEW_CFG_TITLEBAR_BUTTON_0_VECTOR ||
	    option > COW_VIEW_CFG_TITLEBAR_BUTTON_9_IMAGE ||
	    ((option - COW_VIEW_CFG_TITLEBAR_BUTTON_0_VECTOR) % 3) != 0)
		return -1;
	return (option - COW_VIEW_CFG_TITLEBAR_BUTTON_0_VECTOR) / 3;
}

static int
titlebar_button_action_index(enum cow_view_cfg_option option)
{
	if (option < COW_VIEW_CFG_TITLEBAR_BUTTON_0_ACTION ||
	    option > COW_VIEW_CFG_TITLEBAR_BUTTON_9_IMAGE ||
	    ((option - COW_VIEW_CFG_TITLEBAR_BUTTON_0_ACTION) % 3) != 0)
		return -1;
	return (option - COW_VIEW_CFG_TITLEBAR_BUTTON_0_ACTION) / 3;
}

static int
titlebar_button_image_index(enum cow_view_cfg_option option)
{
	if (option < COW_VIEW_CFG_TITLEBAR_BUTTON_0_IMAGE ||
	    option > COW_VIEW_CFG_TITLEBAR_BUTTON_9_IMAGE ||
	    ((option - COW_VIEW_CFG_TITLEBAR_BUTTON_0_IMAGE) % 3) != 0)
		return -1;
	return (option - COW_VIEW_CFG_TITLEBAR_BUTTON_0_IMAGE) / 3;
}

static char *
xstrdup0(const char *s)
{
	return s != NULL ? xstrdup(s) : NULL;
}

static void
titlebar_button_actions_free(struct cow_titlebar_config *tb)
{
	for (int i = 0; i < COW_TITLEBAR_NUM_BUTTONS; i++) {
		xfree(tb->buttons[i].action);
		tb->buttons[i].action = NULL;
	}
}

static void
titlebar_button_images_free(struct cow_titlebar_config *tb)
{
	for (int i = 0; i < COW_TITLEBAR_NUM_BUTTONS; i++) {
		xfree(tb->buttons[i].image);
		tb->buttons[i].image = NULL;
	}
}

static void
titlebar_button_images_dup(struct cow_titlebar_config *dst,
    const struct cow_titlebar_config *src)
{
	for (int i = 0; i < COW_TITLEBAR_NUM_BUTTONS; i++)
		dst->buttons[i].image = xstrdup0(src->buttons[i].image);
}

static void
titlebar_button_actions_dup(struct cow_titlebar_config *dst,
    const struct cow_titlebar_config *src)
{
	for (int i = 0; i < COW_TITLEBAR_NUM_BUTTONS; i++)
		dst->buttons[i].action = xstrdup0(src->buttons[i].action);
}

static void
border_images_free(struct cow_border_colour *bc)
{
	xfree(bc->image);
	xfree(bc->image_n);
	xfree(bc->image_s);
	xfree(bc->image_e);
	xfree(bc->image_w);
	xfree(bc->image_nw);
	xfree(bc->image_ne);
	xfree(bc->image_sw);
	xfree(bc->image_se);
	bc->image = bc->image_n = bc->image_s = bc->image_e = NULL;
	bc->image_w = bc->image_nw = bc->image_ne = NULL;
	bc->image_sw = bc->image_se = NULL;
}

static void
border_images_dup(struct cow_border_colour *dst,
    const struct cow_border_colour *src)
{
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

static char **
border_image_slot(struct cow_border_colour *bc, enum cow_view_cfg_option option)
{
	switch (option) {
	case COW_VIEW_CFG_BORDER_ACTIVE_IMAGE:
	case COW_VIEW_CFG_BORDER_INACTIVE_IMAGE:
		return &bc->image;
	case COW_VIEW_CFG_BORDER_ACTIVE_IMAGE_N:
	case COW_VIEW_CFG_BORDER_INACTIVE_IMAGE_N:
		return &bc->image_n;
	case COW_VIEW_CFG_BORDER_ACTIVE_IMAGE_S:
	case COW_VIEW_CFG_BORDER_INACTIVE_IMAGE_S:
		return &bc->image_s;
	case COW_VIEW_CFG_BORDER_ACTIVE_IMAGE_E:
	case COW_VIEW_CFG_BORDER_INACTIVE_IMAGE_E:
		return &bc->image_e;
	case COW_VIEW_CFG_BORDER_ACTIVE_IMAGE_W:
	case COW_VIEW_CFG_BORDER_INACTIVE_IMAGE_W:
		return &bc->image_w;
	case COW_VIEW_CFG_BORDER_ACTIVE_IMAGE_NW:
	case COW_VIEW_CFG_BORDER_INACTIVE_IMAGE_NW:
		return &bc->image_nw;
	case COW_VIEW_CFG_BORDER_ACTIVE_IMAGE_NE:
	case COW_VIEW_CFG_BORDER_INACTIVE_IMAGE_NE:
		return &bc->image_ne;
	case COW_VIEW_CFG_BORDER_ACTIVE_IMAGE_SW:
	case COW_VIEW_CFG_BORDER_INACTIVE_IMAGE_SW:
		return &bc->image_sw;
	case COW_VIEW_CFG_BORDER_ACTIVE_IMAGE_SE:
	case COW_VIEW_CFG_BORDER_INACTIVE_IMAGE_SE:
		return &bc->image_se;
	default:
		return NULL;
	}
}

/*
 * Per-view config snapshot helpers.  See struct cow_view_cfg in
 * cow/view.h for the rationale.  These three functions own the
 * lifecycle of the heap-allocated strings that live inside the
 * snapshot, so callers (cow_view_create / cow_view_destroy / cmd-set
 * after a global write) need only invoke them.
 */
static void
view_cfg_copy_from_global(struct cow_view_cfg *dst)
{
	if (cow_config == NULL) {
		memset(dst, 0, sizeof(*dst));
		dst->border	   = COW_CFG_DEFAULT_BORDER;
		dst->corner_length = COW_CFG_DEFAULT_CORNER_LENGTH;
		dst->icon_size	   = COW_CFG_DEFAULT_ICON_SIZE;
		dst->border_style  = COW_BORDER_STYLE_FVWM;
		cow_colour_convert(dst->icon_background, 0x465457);
		cow_colour_convert(dst->icon_foreground, 0xFFFFFF);
		dst->icon_title_relief = false;
		return;
	}
	dst->border	     = cow_config->border;
	dst->corner_length   = cow_config->corner_length;
	dst->handles	     = cow_config->handles;
	dst->border_style    = cow_config->border_style;
	dst->border_active   = cow_config->border_active;
	dst->border_inactive = cow_config->border_inactive;
	border_images_dup(&dst->border_active, &cow_config->border_active);
	border_images_dup(&dst->border_inactive, &cow_config->border_inactive);
	dst->titlebar	     = cow_config->titlebar;
	/*
	 * cow_titlebar_config carries an owned char *font_name pointer.
	 * The struct copy above shares the pointer with cow_config; null
	 * it so a later free in this snapshot doesn't double-free, then
	 * dup if the global has one.
	 */
	dst->titlebar.font_name = xstrdup0(cow_config->titlebar.font_name);
	dst->titlebar.active_image = xstrdup0(cow_config->titlebar.active_image);
	dst->titlebar.inactive_image =
	    xstrdup0(cow_config->titlebar.inactive_image);
	titlebar_button_actions_dup(&dst->titlebar, &cow_config->titlebar);
	titlebar_button_images_dup(&dst->titlebar, &cow_config->titlebar);
	dst->container		= cow_config->container;
	dst->container.tab_active_image =
	    xstrdup0(cow_config->container.tab_active_image);
	dst->container.tab_inactive_image =
	    xstrdup0(cow_config->container.tab_inactive_image);
	dst->icon_size		= cow_config->icon_size;
	dst->icon_image		= xstrdup0(cow_config->icon_image);
	memcpy(dst->icon_background, cow_config->icon_background,
	    sizeof(float) * 4);
	memcpy(dst->icon_foreground, cow_config->icon_foreground,
	    sizeof(float) * 4);
	dst->icon_title_relief = cow_config->icon_title_relief;
}

static bool
view_cfg_option_index(enum cow_view_cfg_option option, size_t *word,
    uint64_t *bit)
{
	if (option >= COW_VIEW_CFG_OPTION_COUNT)
		return false;
	*word = option / 64;
	*bit = 1ULL << (option % 64);
	return true;
}

static void
border_colour_set_all(struct cow_border_colour *dst,
    const struct cow_border_colour *src)
{
	memcpy(dst->n, src->n, sizeof(float) * 4);
	memcpy(dst->s, src->s, sizeof(float) * 4);
	memcpy(dst->e, src->e, sizeof(float) * 4);
	memcpy(dst->w, src->w, sizeof(float) * 4);
	memcpy(dst->nw, src->nw, sizeof(float) * 4);
	memcpy(dst->ne, src->ne, sizeof(float) * 4);
	memcpy(dst->sw, src->sw, sizeof(float) * 4);
	memcpy(dst->se, src->se, sizeof(float) * 4);
	dst->gradient = src->gradient;
	dst->gradient_n = src->gradient_n;
	dst->gradient_s = src->gradient_s;
	dst->gradient_e = src->gradient_e;
	dst->gradient_w = src->gradient_w;
	dst->gradient_nw = src->gradient_nw;
	dst->gradient_ne = src->gradient_ne;
	dst->gradient_sw = src->gradient_sw;
	dst->gradient_se = src->gradient_se;
	dst->has_corners = src->has_corners;
	dst->has_nw = src->has_nw;
	dst->has_ne = src->has_ne;
	dst->has_sw = src->has_sw;
	dst->has_se = src->has_se;
}

static void
border_colour_set_edge(struct cow_border_colour *dst,
    const struct cow_border_colour *src, enum cow_view_cfg_option option)
{
	float *d = NULL;
	const float *s = NULL;
	bool *has = NULL;
	bool has_value = false;

	switch (option) {
	case COW_VIEW_CFG_BORDER_ACTIVE_N:
	case COW_VIEW_CFG_BORDER_INACTIVE_N:
		d = dst->n; s = src->n; break;
	case COW_VIEW_CFG_BORDER_ACTIVE_S:
	case COW_VIEW_CFG_BORDER_INACTIVE_S:
		d = dst->s; s = src->s; break;
	case COW_VIEW_CFG_BORDER_ACTIVE_E:
	case COW_VIEW_CFG_BORDER_INACTIVE_E:
		d = dst->e; s = src->e; break;
	case COW_VIEW_CFG_BORDER_ACTIVE_W:
	case COW_VIEW_CFG_BORDER_INACTIVE_W:
		d = dst->w; s = src->w; break;
	case COW_VIEW_CFG_BORDER_ACTIVE_NW:
	case COW_VIEW_CFG_BORDER_INACTIVE_NW:
		d = dst->nw; s = src->nw; has = &dst->has_nw;
		has_value = src->has_nw; break;
	case COW_VIEW_CFG_BORDER_ACTIVE_NE:
	case COW_VIEW_CFG_BORDER_INACTIVE_NE:
		d = dst->ne; s = src->ne; has = &dst->has_ne;
		has_value = src->has_ne; break;
	case COW_VIEW_CFG_BORDER_ACTIVE_SW:
	case COW_VIEW_CFG_BORDER_INACTIVE_SW:
		d = dst->sw; s = src->sw; has = &dst->has_sw;
		has_value = src->has_sw; break;
	case COW_VIEW_CFG_BORDER_ACTIVE_SE:
	case COW_VIEW_CFG_BORDER_INACTIVE_SE:
		d = dst->se; s = src->se; has = &dst->has_se;
		has_value = src->has_se; break;
	default:
		return;
	}
	memcpy(d, s, sizeof(float) * 4);
	if (has != NULL) {
		*has = has_value;
		dst->has_corners = dst->has_nw || dst->has_ne ||
		    dst->has_sw || dst->has_se;
	}
}

void
cow_view_cfg_set_override(struct cow_view_cfg *cfg,
    enum cow_view_cfg_option option)
{
	size_t	 word;
	uint64_t bit;

	if (view_cfg_option_index(option, &word, &bit))
		cfg->overrides[word] |= bit;
}

void
cow_view_cfg_apply_global_option(struct cow_view_cfg *cfg,
    enum cow_view_cfg_option option)
{
	size_t	 word;
	uint64_t bit;

	if (cow_config == NULL ||
	    !view_cfg_option_index(option, &word, &bit) ||
	    (cfg->overrides[word] & bit) != 0)
		return;

	int idx = titlebar_button_vector_index(option);
	if (idx != -1) {
		cfg->titlebar.buttons[idx].vector =
		    cow_config->titlebar.buttons[idx].vector;
		return;
	}
	idx = titlebar_button_action_index(option);
	if (idx != -1) {
		xfree(cfg->titlebar.buttons[idx].action);
		cfg->titlebar.buttons[idx].action =
		    xstrdup0(cow_config->titlebar.buttons[idx].action);
		return;
	}
	idx = titlebar_button_image_index(option);
	if (idx != -1) {
		xfree(cfg->titlebar.buttons[idx].image);
		cfg->titlebar.buttons[idx].image =
		    xstrdup0(cow_config->titlebar.buttons[idx].image);
		return;
	}

	switch (option) {
	case COW_VIEW_CFG_BORDER:
		cfg->border = cow_config->border;
		break;
	case COW_VIEW_CFG_CORNER_LENGTH:
		cfg->corner_length = cow_config->corner_length;
		break;
	case COW_VIEW_CFG_HANDLES:
		cfg->handles = cow_config->handles;
		break;
	case COW_VIEW_CFG_BORDER_STYLE:
		cfg->border_style = cow_config->border_style;
		break;
	case COW_VIEW_CFG_BORDER_ACTIVE:
		border_colour_set_all(&cfg->border_active,
		    &cow_config->border_active);
		break;
	case COW_VIEW_CFG_BORDER_INACTIVE:
		border_colour_set_all(&cfg->border_inactive,
		    &cow_config->border_inactive);
		break;
	case COW_VIEW_CFG_BORDER_ACTIVE_N:
	case COW_VIEW_CFG_BORDER_ACTIVE_S:
	case COW_VIEW_CFG_BORDER_ACTIVE_E:
	case COW_VIEW_CFG_BORDER_ACTIVE_W:
	case COW_VIEW_CFG_BORDER_ACTIVE_NW:
	case COW_VIEW_CFG_BORDER_ACTIVE_NE:
	case COW_VIEW_CFG_BORDER_ACTIVE_SW:
	case COW_VIEW_CFG_BORDER_ACTIVE_SE:
		border_colour_set_edge(&cfg->border_active,
		    &cow_config->border_active, option);
		break;
	case COW_VIEW_CFG_BORDER_ACTIVE_GRADIENT:
		cfg->border_active.gradient = cow_config->border_active.gradient;
		break;
	case COW_VIEW_CFG_BORDER_ACTIVE_GRADIENT_N:
	case COW_VIEW_CFG_BORDER_ACTIVE_GRADIENT_S:
	case COW_VIEW_CFG_BORDER_ACTIVE_GRADIENT_E:
	case COW_VIEW_CFG_BORDER_ACTIVE_GRADIENT_W:
	case COW_VIEW_CFG_BORDER_ACTIVE_GRADIENT_NW:
	case COW_VIEW_CFG_BORDER_ACTIVE_GRADIENT_NE:
	case COW_VIEW_CFG_BORDER_ACTIVE_GRADIENT_SW:
	case COW_VIEW_CFG_BORDER_ACTIVE_GRADIENT_SE:
		cow_border_gradient_copy_for_option(&cfg->border_active,
		    &cow_config->border_active, option);
		break;
	case COW_VIEW_CFG_BORDER_ACTIVE_IMAGE:
	case COW_VIEW_CFG_BORDER_ACTIVE_IMAGE_N:
	case COW_VIEW_CFG_BORDER_ACTIVE_IMAGE_S:
	case COW_VIEW_CFG_BORDER_ACTIVE_IMAGE_E:
	case COW_VIEW_CFG_BORDER_ACTIVE_IMAGE_W:
	case COW_VIEW_CFG_BORDER_ACTIVE_IMAGE_NW:
	case COW_VIEW_CFG_BORDER_ACTIVE_IMAGE_NE:
	case COW_VIEW_CFG_BORDER_ACTIVE_IMAGE_SW:
	case COW_VIEW_CFG_BORDER_ACTIVE_IMAGE_SE: {
		char **dst = border_image_slot(&cfg->border_active, option);
		char **src = border_image_slot(&cow_config->border_active,
		    option);
		if (dst != NULL && src != NULL) {
			xfree(*dst);
			*dst = xstrdup0(*src);
		}
		break;
	}
	case COW_VIEW_CFG_BORDER_INACTIVE_N:
	case COW_VIEW_CFG_BORDER_INACTIVE_S:
	case COW_VIEW_CFG_BORDER_INACTIVE_E:
	case COW_VIEW_CFG_BORDER_INACTIVE_W:
	case COW_VIEW_CFG_BORDER_INACTIVE_NW:
	case COW_VIEW_CFG_BORDER_INACTIVE_NE:
	case COW_VIEW_CFG_BORDER_INACTIVE_SW:
	case COW_VIEW_CFG_BORDER_INACTIVE_SE:
		border_colour_set_edge(&cfg->border_inactive,
		    &cow_config->border_inactive, option);
		break;
	case COW_VIEW_CFG_BORDER_INACTIVE_GRADIENT:
		cfg->border_inactive.gradient =
		    cow_config->border_inactive.gradient;
		break;
	case COW_VIEW_CFG_BORDER_INACTIVE_GRADIENT_N:
	case COW_VIEW_CFG_BORDER_INACTIVE_GRADIENT_S:
	case COW_VIEW_CFG_BORDER_INACTIVE_GRADIENT_E:
	case COW_VIEW_CFG_BORDER_INACTIVE_GRADIENT_W:
	case COW_VIEW_CFG_BORDER_INACTIVE_GRADIENT_NW:
	case COW_VIEW_CFG_BORDER_INACTIVE_GRADIENT_NE:
	case COW_VIEW_CFG_BORDER_INACTIVE_GRADIENT_SW:
	case COW_VIEW_CFG_BORDER_INACTIVE_GRADIENT_SE:
		cow_border_gradient_copy_for_option(&cfg->border_inactive,
		    &cow_config->border_inactive, option);
		break;
	case COW_VIEW_CFG_BORDER_INACTIVE_IMAGE:
	case COW_VIEW_CFG_BORDER_INACTIVE_IMAGE_N:
	case COW_VIEW_CFG_BORDER_INACTIVE_IMAGE_S:
	case COW_VIEW_CFG_BORDER_INACTIVE_IMAGE_E:
	case COW_VIEW_CFG_BORDER_INACTIVE_IMAGE_W:
	case COW_VIEW_CFG_BORDER_INACTIVE_IMAGE_NW:
	case COW_VIEW_CFG_BORDER_INACTIVE_IMAGE_NE:
	case COW_VIEW_CFG_BORDER_INACTIVE_IMAGE_SW:
	case COW_VIEW_CFG_BORDER_INACTIVE_IMAGE_SE: {
		char **dst = border_image_slot(&cfg->border_inactive, option);
		char **src = border_image_slot(&cow_config->border_inactive,
		    option);
		if (dst != NULL && src != NULL) {
			xfree(*dst);
			*dst = xstrdup0(*src);
		}
		break;
	}
	case COW_VIEW_CFG_TITLEBAR_ENABLED:
		cfg->titlebar.enabled = cow_config->titlebar.enabled;
		break;
	case COW_VIEW_CFG_TITLEBAR_ACTIVE_COLOUR:
		memcpy(cfg->titlebar.active_colour,
		    cow_config->titlebar.active_colour, sizeof(float) * 4);
		break;
	case COW_VIEW_CFG_TITLEBAR_INACTIVE_COLOUR:
		memcpy(cfg->titlebar.inactive_colour,
		    cow_config->titlebar.inactive_colour, sizeof(float) * 4);
		break;
	case COW_VIEW_CFG_TITLEBAR_ACTIVE_GRADIENT:
		cfg->titlebar.active_gradient =
		    cow_config->titlebar.active_gradient;
		break;
	case COW_VIEW_CFG_TITLEBAR_INACTIVE_GRADIENT:
		cfg->titlebar.inactive_gradient =
		    cow_config->titlebar.inactive_gradient;
		break;
	case COW_VIEW_CFG_TITLEBAR_ACTIVE_IMAGE:
		xfree(cfg->titlebar.active_image);
		cfg->titlebar.active_image =
		    xstrdup0(cow_config->titlebar.active_image);
		break;
	case COW_VIEW_CFG_TITLEBAR_INACTIVE_IMAGE:
		xfree(cfg->titlebar.inactive_image);
		cfg->titlebar.inactive_image =
		    xstrdup0(cow_config->titlebar.inactive_image);
		break;
	case COW_VIEW_CFG_TITLEBAR_FG_ACTIVE:
		memcpy(cfg->titlebar.fg_active, cow_config->titlebar.fg_active,
		    sizeof(float) * 4);
		break;
	case COW_VIEW_CFG_TITLEBAR_FG_INACTIVE:
		memcpy(cfg->titlebar.fg_inactive,
		    cow_config->titlebar.fg_inactive, sizeof(float) * 4);
		break;
	case COW_VIEW_CFG_TITLEBAR_HEIGHT:
		cfg->titlebar.height = cow_config->titlebar.height;
		break;
	case COW_VIEW_CFG_TITLEBAR_SQUEEZE:
		cfg->titlebar.squeeze.mode = cow_config->titlebar.squeeze.mode;
		break;
	case COW_VIEW_CFG_TITLEBAR_SQUEEZE_JUSTIFY:
		cfg->titlebar.squeeze.justify =
		    cow_config->titlebar.squeeze.justify;
		break;
	case COW_VIEW_CFG_TITLEBAR_SQUEEZE_WIDTH:
		cfg->titlebar.squeeze.width =
		    cow_config->titlebar.squeeze.width;
		break;
	case COW_VIEW_CFG_TITLEBAR_SQUEEZE_OFFSET:
		cfg->titlebar.squeeze.offset =
		    cow_config->titlebar.squeeze.offset;
		break;
	case COW_VIEW_CFG_TITLEBAR_SQUEEZE_MIN_WIDTH:
		cfg->titlebar.squeeze.min_width =
		    cow_config->titlebar.squeeze.min_width;
		break;
	case COW_VIEW_CFG_CONTAINER_TAB_ACTIVE_COLOUR:
		memcpy(cfg->container.tab_active, cow_config->container.tab_active,
		    sizeof(float) * 4);
		cfg->container.has_tab_active =
		    cow_config->container.has_tab_active;
		break;
	case COW_VIEW_CFG_CONTAINER_TAB_INACTIVE_COLOUR:
		memcpy(cfg->container.tab_inactive,
		    cow_config->container.tab_inactive, sizeof(float) * 4);
		cfg->container.has_tab_inactive =
		    cow_config->container.has_tab_inactive;
		break;
	case COW_VIEW_CFG_CONTAINER_TAB_ACTIVE_GRADIENT:
		cfg->container.tab_active_gradient =
		    cow_config->container.tab_active_gradient;
		break;
	case COW_VIEW_CFG_CONTAINER_TAB_INACTIVE_GRADIENT:
		cfg->container.tab_inactive_gradient =
		    cow_config->container.tab_inactive_gradient;
		break;
	case COW_VIEW_CFG_CONTAINER_TAB_ACTIVE_IMAGE:
		xfree(cfg->container.tab_active_image);
		cfg->container.tab_active_image =
		    xstrdup0(cow_config->container.tab_active_image);
		break;
	case COW_VIEW_CFG_CONTAINER_TAB_INACTIVE_IMAGE:
		xfree(cfg->container.tab_inactive_image);
		cfg->container.tab_inactive_image =
		    xstrdup0(cow_config->container.tab_inactive_image);
		break;
	case COW_VIEW_CFG_CONTAINER_TAB_FG_ACTIVE:
		memcpy(cfg->container.tab_fg_active,
		    cow_config->container.tab_fg_active, sizeof(float) * 4);
		cfg->container.has_tab_fg_active =
		    cow_config->container.has_tab_fg_active;
		break;
	case COW_VIEW_CFG_CONTAINER_TAB_FG_INACTIVE:
		memcpy(cfg->container.tab_fg_inactive,
		    cow_config->container.tab_fg_inactive, sizeof(float) * 4);
		cfg->container.has_tab_fg_inactive =
		    cow_config->container.has_tab_fg_inactive;
		break;
	case COW_VIEW_CFG_CONTAINER_TAB_SUNKEN:
		cfg->container.tab_sunken_inactive =
		    cow_config->container.tab_sunken_inactive;
		break;
	case COW_VIEW_CFG_FONT:
		xfree(cfg->font_name);
		cfg->font_name = xstrdup0(cow_config->font_name);
		break;
	case COW_VIEW_CFG_ICON_SIZE:
		cfg->icon_size = cow_config->icon_size;
		break;
	case COW_VIEW_CFG_ICON_IMAGE:
		xfree(cfg->icon_image);
		cfg->icon_image = xstrdup0(cow_config->icon_image);
		break;
	case COW_VIEW_CFG_ICON_BACKGROUND:
		memcpy(cfg->icon_background, cow_config->icon_background,
		    sizeof(float) * 4);
		break;
	case COW_VIEW_CFG_ICON_FOREGROUND:
		memcpy(cfg->icon_foreground, cow_config->icon_foreground,
		    sizeof(float) * 4);
		break;
	case COW_VIEW_CFG_ICON_TITLE_RELIEF:
		cfg->icon_title_relief = cow_config->icon_title_relief;
		break;
	default:
		break;
	}
}

void
cow_view_cfg_init_from_global(struct cow_view_cfg *cfg)
{
	memset(cfg, 0, sizeof(*cfg));
	view_cfg_copy_from_global(cfg);
	if (cow_config != NULL && cow_config->font_name != NULL)
		cfg->font_name = xstrdup(cow_config->font_name);
}

void
cow_view_cfg_apply_global(struct cow_view_cfg *cfg)
{
	cow_view_cfg_fini(cfg);
	memset(cfg, 0, sizeof(*cfg));
	view_cfg_copy_from_global(cfg);
	if (cow_config != NULL && cow_config->font_name != NULL)
		cfg->font_name = xstrdup(cow_config->font_name);
}

void
cow_view_cfg_fini(struct cow_view_cfg *cfg)
{
	xfree(cfg->font_name);
	xfree(cfg->icon_image);
	xfree(cfg->titlebar.font_name);
	xfree(cfg->titlebar.active_image);
	xfree(cfg->titlebar.inactive_image);
	xfree(cfg->container.tab_active_image);
	xfree(cfg->container.tab_inactive_image);
	border_images_free(&cfg->border_active);
	border_images_free(&cfg->border_inactive);
	titlebar_button_actions_free(&cfg->titlebar);
	titlebar_button_images_free(&cfg->titlebar);
	cfg->font_name		= NULL;
	cfg->icon_image		= NULL;
	cfg->titlebar.font_name = NULL;
}

/*
 * river_window_v1 event handlers
 */

static void
window_handle_closed(void *data, __unused struct river_window_v1 *rw)
{
	struct cow_view *view = data;

	fprintf(stderr, "cow: window closed (app_id=%s)\n",
	    view->app_id ? view->app_id : "(null)");
	view->closed = true;
}

static void
window_handle_dimensions_hint(void *data, __unused struct river_window_v1 *rw,
    int32_t min_width, int32_t min_height, int32_t max_width,
    int32_t max_height)
{
	struct cow_view *view = data;
	view->min_width	      = min_width;
	view->min_height      = min_height;
	view->max_width	      = max_width;
	view->max_height      = max_height;
}

static void
window_handle_dimensions(void *data, __unused struct river_window_v1 *rw,
    int32_t width, int32_t height)
{
	struct cow_view *view = data;

	bool changed = (view->content_width != width ||
	    view->content_height != height);

	/* Mark decoration dirty if dimensions changed so the frame
	 * gets redrawn on the next render cycle. */
	if (changed) {
		view->dec.dirty = true;
		cow_wm_request_manage_dirty();
	}

	view->content_width  = width;
	view->content_height = height;

	/*
	 * Apply a deferred position now that the new (smaller) buffer
	 * dimensions are in effect.  Without this defer/apply pair,
	 * a maximise that shrinks the window briefly renders the old
	 * (larger) buffer at the new top-left, which on multi-output
	 * setups visually overshoots into a neighbouring monitor.
	 */
	if (view->deferred_position) {
		view->x			= view->deferred_x;
		view->y			= view->deferred_y;
		view->deferred_position = false;
		if (view->output != NULL && view->desk != NULL) {
			view->vx = (view->x - view->output->x) +
			    view->desk->page_vx;
			view->vy = (view->y - view->output->y) +
			    view->desk->page_vy;
		}
	}

	bool first_dims	     = !view->has_dimensions;
	view->has_dimensions = true;

	/*
	 * Fire on-map / on-desk on the first dimensions event.  Rule
	 * actions (window-move, etc.) require has_dimensions to be set,
	 * so firing earlier from cow_view_configure() would silently
	 * no-op them.
	 */
	if (first_dims && !view->on_map_fired) {
		view->on_map_fired = true;
		cow_decor_apply_runtime_active(view);
		cow_rule_fire(COW_RULE_ON_MAP, view, view->desk,
		    view->output);
		cow_rule_fire(COW_RULE_ON_DESK, view, view->desk,
		    view->output);
	}

	/*
	 * Notify status clients (cowpager, cowbar, ...) so that
	 * window-thumbnail miniatures and any size-derived UI track
	 * the new dimensions promptly.  Without this, a window's
	 * IPC-reported `width` / `height` only refreshes when some
	 * unrelated status event (focus change, desk switch, view
	 * add/destroy) happens to fire next, which can leave a
	 * just-unmaximized window still looking maximized in the
	 * pager for an arbitrary stretch of time.
	 *
	 * Skip the no-op case so identical-size redraws don't
	 * needlessly re-broadcast the entire snapshot.
	 */
	if (changed)
		cow_status_emit();
}

static void
window_handle_app_id(void *data, __unused struct river_window_v1 *rw,
    const char *app_id)
{
	struct cow_view *view = data;

	xfree(view->app_id);
	view->app_id = app_id ? xstrdup(app_id) : NULL;
}

static void
window_handle_title(void *data, __unused struct river_window_v1 *rw,
    const char *title)
{
	struct cow_view *view = data;

	const char *old   = view->title;
	const char *fresh = title ? title : "";
	bool changed = (old == NULL) ? (fresh[0] != '\0') :
				       (strcmp(old, fresh) != 0);

	xfree(view->title);
	view->title	= title ? xstrdup(title) : NULL;
	view->dec.dirty = true;
	cow_wm_request_manage_dirty();
	cow_status_emit();

	if (changed)
		cow_rule_fire(COW_RULE_ON_TITLE_CHANGE, view, view->desk,
		    view->output);
}

static void
window_handle_parent(void *data, __unused struct river_window_v1 *rw,
    struct river_window_v1 *parent)
{
	struct cow_view *view = data;

	if (parent != NULL) {
		view->parent = cow_view_find(parent);
	} else {
		view->parent = NULL;
	}
}

static void
window_handle_decoration_hint(void *data, __unused struct river_window_v1 *rw,
    uint32_t hint)
{
	struct cow_view *view = data;
	view->decoration_hint = hint;
}

static void
window_handle_pointer_move_requested(void *data,
    __unused struct river_window_v1 *rw, struct river_seat_v1 *rs)
{
	struct cow_view *view = data;
	struct cow_seat *seat = cow_seat_find(rs);
	if (seat != NULL)
		cow_pointer_op_start_move(seat, view);
}

static void
window_handle_pointer_resize_requested(void *data,
    __unused struct river_window_v1 *rw, struct river_seat_v1 *rs,
    uint32_t edges)
{
	struct cow_view *view = data;
	struct cow_seat *seat = cow_seat_find(rs);
	if (seat != NULL)
		cow_pointer_op_start_resize(seat, view, edges);
}

static void
window_handle_show_window_menu_requested(__unused void *data,
    __unused struct river_window_v1 *rw, __unused int32_t x, __unused int32_t y)
{
	return;
}

static void
window_handle_maximize_requested(void *data,
    __unused struct river_window_v1   *rw)
{
	struct cow_view *view = data;
	if (!view->maximized)
		cow_view_maximize(view);
}

static void
window_handle_unmaximize_requested(void *data,
    __unused struct river_window_v1	*rw)
{
	struct cow_view *view = data;
	if (view->maximized)
		cow_view_unmaximize(view);
}

static void
window_handle_fullscreen_requested(void *data,
    __unused struct river_window_v1	*rw,
    __unused struct river_output_v1	*output)
{
	struct cow_view *view = data;
	if (!view->fullscreen)
		cow_view_fullscreen(view);
}

static void
window_handle_exit_fullscreen_requested(void *data,
    __unused struct river_window_v1	     *rw)
{
	struct cow_view *view = data;
	if (view->fullscreen)
		cow_view_exit_fullscreen(view);
}

static void
window_handle_minimize_requested(__unused void *data,
    __unused struct river_window_v1	       *rw)
{
	/* TODO: hide the window */
}

static void
window_handle_unreliable_pid(__unused void *data,
    __unused struct river_window_v1 *rw, __unused int32_t pid)
{
}

static void
window_handle_presentation_hint(__unused void *data,
    __unused struct river_window_v1 *rw, __unused uint32_t hint)
{
}

static void
window_handle_identifier(void *data, __unused struct river_window_v1 *rw,
    const char *identifier)
{
	struct cow_view *view = data;
	xfree(view->identifier);
	view->identifier = identifier ? xstrdup(identifier) : NULL;
}

static const struct river_window_v1_listener window_listener = {
	.closed			    = window_handle_closed,
	.dimensions_hint	    = window_handle_dimensions_hint,
	.dimensions		    = window_handle_dimensions,
	.app_id			    = window_handle_app_id,
	.title			    = window_handle_title,
	.parent			    = window_handle_parent,
	.decoration_hint	    = window_handle_decoration_hint,
	.pointer_move_requested	    = window_handle_pointer_move_requested,
	.pointer_resize_requested   = window_handle_pointer_resize_requested,
	.show_window_menu_requested = window_handle_show_window_menu_requested,
	.maximize_requested	    = window_handle_maximize_requested,
	.unmaximize_requested	    = window_handle_unmaximize_requested,
	.fullscreen_requested	    = window_handle_fullscreen_requested,
	.exit_fullscreen_requested  = window_handle_exit_fullscreen_requested,
	.minimize_requested	    = window_handle_minimize_requested,
	.unreliable_pid		    = window_handle_unreliable_pid,
	.presentation_hint	    = window_handle_presentation_hint,
	.identifier		    = window_handle_identifier,
};

struct cow_view *
cow_view_create(struct river_window_v1 *rw)
{
	struct cow_view *view = xcalloc(1, sizeof(*view));

	view->river_window = rw;

	/* xcalloc zeroed the struct; assign only non-zero defaults. */
	view->use_ssd		   = true;
	view->pressed_button	   = -1;
	view->icon_inverted_button = -1;

	/* New windows start on top of the stack within the default
	 * layer.  Layered ordering is enforced by stack.c's pass; raising
	 * never crosses a layer boundary. */
	view->raise_seq = ++cow_wm.raise_seq_counter;
	view->layer	= cow_config != NULL ? cow_config->default_layer :
					       COW_CFG_DEFAULT_LAYER;
	view->orphan_desk_nr = -1;

	cow_view_cfg_init_from_global(&view->cfg);
	cow_decoration_init(view);

	/* Assign to the current output and its active sheet */
	if (cow_wm.current_output != NULL) {
		view->output = cow_wm.current_output;
		view->desk   = cow_wm.current_output->current_desk;
	}

	wl_list_init(&view->desk_views);
	wl_list_init(&view->output_views);

	river_window_v1_add_listener(rw, &window_listener, view);

	wl_list_insert(&cow_wm.views, &view->wm_link);
	cow_status_emit();

	return view;
}

/* Clear all WM-wide back-references to 'view' (drag, pending focus,
 * focused). */
static void
view_clear_wm_refs(struct cow_view *view)
{
	if (cow_wm.drag_drop_target == view)
		cow_wm.drag_drop_target = NULL;
	if (cow_wm.drag_drop_source == view)
		cow_wm.drag_drop_source = NULL;
	if (cow_wm.pending_focus == view)
		cow_wm.pending_focus = NULL;
	if (cow_wm.focused_view == view)
		cow_wm.focused_view = NULL;
}

/*
 * Clear all per-seat back-references to 'view'.  Without this, any
 * seat that had pointer/keyboard focus on the about-to-die view keeps
 * a dangling pointer; the render loop's next dereference of
 * interacted_view / pressed_view / etc. cascades into spurious
 * requests on already-destroyed river window objects, which panics
 * river inside Window.destroy.
 */
static void
view_clear_seat_refs(struct cow_view *view)
{
	struct cow_seat *s;
	wl_list_for_each(s, &cow_wm.seats, wm_link)
	    cow_seat_forget_view(s, view);
}

/*
 * Detach 'view' from its container without cascading (callers are
 * tearing the view down).  Frees the container if this was the last
 * member; otherwise re-activates a sibling and marks it for repaint.
 */
static void
view_detach_container_silent(struct cow_view *view)
{
	if (view->container_member == NULL)
		return;
	struct cow_container_member *m		= view->container_member;
	struct cow_container	    *c		= m->container;
	bool			     was_active = (c->active == m);
	wl_list_remove(&m->link);
	c->n_members--;
	view->container_member = NULL;
	xfree(m);
	if (c->n_members == 0) {
		wl_list_remove(&c->wm_link);
		xfree(c);
	} else if (was_active) {
		c->active = wl_container_of(c->members.next, c->active, link);
		c->active->view->use_ssd   = true;
		c->active->view->dec.dirty = true;
	}
}

void
cow_view_destroy(struct cow_view *view)
{
	cow_status_emit();
	view_clear_wm_refs(view);
	view_clear_seat_refs(view);
	view_detach_container_silent(view);

	wl_list_remove(&view->wm_link);
	wl_list_remove(&view->desk_views);
	wl_list_remove(&view->output_views);

	cow_decoration_fini(&view->dec);

	COW_DESTROY_AND_NULL(view->node, river_node_v1_destroy);
	COW_DESTROY_AND_NULL(view->decoration, river_decoration_v1_destroy);
	COW_DESTROY_AND_NULL(view->decoration_surface, wl_surface_destroy);

	river_window_v1_destroy(view->river_window);

	xfree(view->identifier);
	xfree(view->app_id);
	xfree(view->title);
	cow_view_clear_orphan(view);
	cow_view_cfg_fini(&view->cfg);
	xfree(view);
}

void
cow_view_clear_orphan(struct cow_view *view)
{
	xfree(view->orphan_output_name);
	view->orphan_output_name = NULL;
	view->orphan_desk_nr	 = -1;
	view->orphan_page_vx	 = 0;
	view->orphan_page_vy	 = 0;
}

void
cow_view_configure(struct cow_view *view)
{
	if (view->configured)
		return;

	/* Always use SSD -- the WM controls decorations, not the client.
	 * We tell the window to use SSD regardless of its preference.
	 * Windows that "only support CSD" will just draw their own
	 * decorations inside the content area, and we draw ours outside. */
	river_window_v1_use_ssd(view->river_window);
	view->use_ssd = true;

	/* Set capabilities */
	river_window_v1_set_capabilities(view->river_window,
	    RIVER_WINDOW_V1_CAPABILITIES_MAXIMIZE |
		RIVER_WINDOW_V1_CAPABILITIES_FULLSCREEN |
		RIVER_WINDOW_V1_CAPABILITIES_MINIMIZE |
		RIVER_WINDOW_V1_CAPABILITIES_WINDOW_MENU);

	/*
	 * Trigger River's .ready -> .initialized state transition by calling
	 * propose_dimensions.  Without this call, River never sends the first
	 * configure and the window stays invisible.
	 *
	 * Proposing 0x0 means "no preference":
	 * - XDG shell apps (Chrome, pavucontrol): receive configure(0, 0)
	 *   which means "choose your own size" per the XDG shell spec.
	 * - XWayland apps (xterm): River substitutes the X11 surface's own
	 *   geometry for 0, so xterm -g 80x24 is honoured.
	 */
	river_window_v1_propose_dimensions(view->river_window, 0, 0);

	view->configured = true;

	/* Focus and raise new windows on the next manage cycle */
	view->needs_raise = true;
	view->needs_focus = true;
	cow_wm_request_manage_dirty();
}

/* Forward declaration: defined later, used by view_cleanup_stale_decoration. */
static void view_drop_decoration(struct cow_view *view);

/* ----------------------------------------------------------------------
 * cow_view_apply_render -- helpers
 *
 * The top-level apply_render dispatcher reads as a list of named
 * phases.  Each helper below covers one logical step and is called in
 * order: cleanup, placement, geometry sync, node positioning,
 * stacking, visibility, clip.
 * ---------------------------------------------------------------------- */

/*
 * Deferred decoration cleanup.  When a view loses SSD ownership (e.g.
 * a container tab was deactivated) its decoration surface must stay
 * alive until this render cycle so that any pending window_interaction
 * events are still routed to a valid surface.  Destroy it now, before
 * hide/show, so the compositor releases the surface atomically with
 * render_finish.
 */
static void
view_cleanup_stale_decoration(struct cow_view *view)
{
	if (view->use_ssd || view->decoration == NULL)
		return;
	view_drop_decoration(view);
}

/*
 * Compute the under-mouse placement origin.  Centres the frame on the
 * pointer position then clamps so the full frame stays within the
 * usable area (which already has reserved areas applied).  Clamping
 * max before min keeps the top-left visible on screens smaller than
 * the window.
 */
static void
view_place_undermouse(struct cow_view *view, int dec_left, int dec_top)
{
	struct cow_box *ua = &view->output->usable_area;
	int		px = ua->x + ua->width / 2;
	int		py = ua->y + ua->height / 2;
	cow_seat_get_pointer(&px, &py);
	int cw	= view->content_width > 0 ? view->content_width : 640;
	int ch	= view->content_height > 0 ? view->content_height : 400;
	int fw	= cw + 2 * view->dec.border_width;
	int fh	= ch + dec_top + view->dec.border_width;
	view->x = px - fw / 2 + dec_left;
	view->y = py - fh / 2 + dec_top;

	if (ua->width > 0) {
		int max_x = ua->x + ua->width - fw + dec_left;
		int min_x = ua->x + dec_left;
		if (view->x > max_x)
			view->x = max_x;
		if (view->x < min_x)
			view->x = min_x;
	}
	if (ua->height > 0) {
		int max_y = ua->y + ua->height - fh + dec_top;
		int min_y = ua->y + dec_top;
		if (view->y > max_y)
			view->y = max_y;
		if (view->y < min_y)
			view->y = min_y;
	}
}

static void
view_place_centre(struct cow_view *view, int dec_left, int dec_top)
{
	struct cow_box *ua = &view->output->usable_area;
	int		fw = view->content_width + 2 * view->dec.border_width;
	int fh	= view->content_height + dec_top + view->dec.border_width;
	view->x = ua->x + (ua->width - fw) / 2 + dec_left;
	view->y = ua->y + (ua->height - fh) / 2 + dec_top;
}

static void
view_place_cascade(struct cow_view *view, int dec_left, int dec_top, int gap)
{
	static int	cascade_offset = 0;
	struct cow_box *ua	       = &view->output->usable_area;
	view->x	       = ua->x + dec_left + gap + cascade_offset;
	view->y	       = ua->y + dec_top + gap + cascade_offset;
	cascade_offset = (cascade_offset + 30) % 150;
}

/*
 * Set vx/vy from the freshly-computed x/y so the window appears on the
 * page the user is currently viewing, then clamp the virtual position
 * so the window's top-left corner lands within the current page.  A
 * negative or over-width vx would cause cow_view_visible to immediately
 * hide the window.
 */
static void
view_clamp_to_current_page(struct cow_view *view)
{
	struct cow_box	*ua  = &view->output->usable_area;
	struct cow_desk *pd  = view->output->current_desk;
	int		 vpx = pd->page_vx;
	int		 vpy = pd->page_vy;
	int		 ow  = ua->width;
	int		 oh  = ua->height;
	view->vx	     = (view->x - view->output->x) + vpx;
	view->vy	     = (view->y - view->output->y) + vpy;
	if (ow > 0 && view->vx < vpx)
		view->vx = vpx;
	if (ow > 0 && view->vx >= vpx + ow)
		view->vx = vpx + ow - 1;
	if (oh > 0 && view->vy < vpy)
		view->vy = vpy;
	if (oh > 0 && view->vy >= vpy + oh)
		view->vy = vpy + oh - 1;
	view->x = view->output->x + (view->vx - vpx);
	view->y = view->output->y + (view->vy - vpy);
}

/*
 * Initial window placement based on the configured strategy.  Picks the
 * pointer's output rather than the output that created the view, then
 * delegates to the placement-specific helpers.  No-op if the view has
 * already been placed.
 */
static void
view_compute_initial_placement(struct cow_view *view)
{
	if (view->placed)
		return;

	if (view->output == NULL)
		return;

	struct cow_seat *ps = cow_seat_first_with_pointer();
	if (ps != NULL) {
		struct cow_output *ptr_out = cow_output_at(ps->pointer_x,
		    ps->pointer_y);
		if (ptr_out != NULL)
			cow_view_reassign_output(view, ptr_out);
	}

	int gap	     = cow_config ? cow_config->gap : COW_CFG_DEFAULT_GAP;
	int dec_top  = view->dec.border_width + view->dec.title_thickness;
	int dec_left = view->dec.border_width;
	enum cow_placement_mode mode = cow_config ? cow_config->placement :
						    COW_PLACEMENT_CASCADE;

	switch (mode) {
	case COW_PLACEMENT_UNDERMOUSE:
		view_place_undermouse(view, dec_left, dec_top);
		break;
	case COW_PLACEMENT_CENTRE:
		view_place_centre(view, dec_left, dec_top);
		break;
	case COW_PLACEMENT_CASCADE:
	default:
		view_place_cascade(view, dec_left, dec_top, gap);
		break;
	}
	view_clamp_to_current_page(view);
	view->placed = true;
}

/*
 * Position node + apply raise/lower.  River assigns a window to
 * whichever output contains its position, even for hidden windows; an
 * off-page window may have a large negative or positive y/x that
 * lands inside a physically adjacent monitor.  Park hidden views one
 * pixel below the bounding box of all outputs (always off-screen on
 * Wayland) so River doesn't migrate them.  vx/vy stay correct.
 */
static void
view_apply_node_position(struct cow_view *view)
{
	int render_x = view->x;
	int render_y = view->y;
	if (view->output != NULL && !cow_view_visible(view, view->output)) {
		render_x = 0;
		render_y = cow_outputs_max_bottom() + 1;
	}
	river_node_v1_set_position(view->node, render_x, render_y);

	/*
	 * Raise / lower no longer call river_node_v1_place_{top,bottom}
	 * directly: the global stacking order is owned by stack.c and
	 * enforced once per manage cycle.  Here we only update the
	 * intra-layer raise_seq counter and mark the stack dirty so the
	 * deferred pass re-issues node ordering with layer constraints
	 * respected.
	 */
	if (view->needs_raise && view->node != NULL) {
		/*
		 * Raising any container member raises the whole
		 * container together.  Assign a fresh raise_seq to
		 * every member so the stack sort keeps them contiguous
		 * within their shared layer.
		 */
		if (view->container_member != NULL) {
			struct cow_container *c =
			    view->container_member->container;
			uint64_t seq = ++cow_wm.raise_seq_counter;
			struct cow_container_member *m;
			wl_list_for_each(m, &c->members, link)
			{
				if (m->view != NULL)
					m->view->raise_seq = seq;
			}
			/* Bias the active member by +0 (already seq),
			 * non-active members by -1 ... so the active
			 * one sits on top of its peers.  This preserves
			 * existing visual semantics (active member
			 * draws on top of its container peers). */
			if (c->active != NULL && c->active->view != NULL) {
				wl_list_for_each(m, &c->members, link)
				{
					if (m->view != NULL && m != c->active &&
					    m->view->raise_seq > 0)
						m->view->raise_seq--;
				}
			}
		} else {
			view->raise_seq = ++cow_wm.raise_seq_counter;
		}
		view->needs_raise = false;
		cow_stack_mark_dirty();
	}
	if (view->pending_lower && view->node != NULL) {
		/* Push raise_seq to one below the layer's current floor so
		 * the deferred sort renders this view bottommost within
		 * its layer. */
		uint64_t floor = cow_stack_min_raise_seq_in_layer(
		    cow_view_effective_layer(view));
		view->raise_seq	    = (floor > 0) ? floor - 1 : 0;
		view->pending_lower = false;
		cow_stack_mark_dirty();
	}

	/*
	 * For contained views, pin the node to the container's position.
	 * propose_dimensions for out-of-sync members is handled by the
	 * pending_resize mechanism in the manage sequence (via
	 * cow_view_set_pending_geometry), NOT here.  Calling
	 * river_window_v1_propose_dimensions from the render sequence
	 * is a sequence_order protocol violation.
	 */
	if (view->container_member != NULL) {
		struct cow_container *c = view->container_member->container;
		river_node_v1_set_position(view->node, c->x, c->y);
	}
}

/* Show or hide the view's surface based on visibility, and update
 * decoration / container z-order while it's visible. */
static void
view_apply_show_hide(struct cow_view *view)
{
	if (view->output == NULL || !cow_view_visible(view, view->output)) {
		river_window_v1_hide(view->river_window);
		return;
	}

	river_window_v1_show(view->river_window);

	/*
	 * Container z-ordering is owned by the global stack pass
	 * (stack.c).  Container members share a layer and receive a
	 * fanned raise_seq when any member is raised, so the sort
	 * keeps them contiguous and the active member on top.
	 */

	if (view->use_ssd) {
		bool active = (view == cow_wm.focused_view);
		cow_decoration_update(view, active);
	}
}

/* Apply pending content clip box.  Used by shade to hide window
 * content while keeping the decoration (titlebar + borders) visible. */
static void
view_apply_pending_clip(struct cow_view *view)
{
	if (!view->pending_clip)
		return;
	river_window_v1_set_content_clip_box(view->river_window, view->clip_x,
	    view->clip_y, view->clip_w, view->clip_h);
	view->pending_clip = false;
}

void
cow_view_apply_render(struct cow_view *view)
{
	if (!view->has_dimensions || view->closed)
		return;
	if (view->output == NULL)
		return;

	view_cleanup_stale_decoration(view);

	if (view->node == NULL)
		view->node = river_window_v1_get_node(view->river_window);

	view_compute_initial_placement(view);

	/*
	 * Window arrived with a pre-set position (e.g. XWayland with a
	 * saved geometry) that bypassed the placement block above.  vx/vy
	 * are still zero; seed them from x/y now so the window lands on
	 * the correct page instead of always page (0,0).
	 */
	if (!view->placed && view->output != NULL && view->desk != NULL) {
		cow_view_sync_vxy_from_global(view);
		view->placed = true;
	}

	/*
	 * Do NOT clamp view->x/y to the usable area here.  The rendered
	 * x/y is legitimately negative or out-of-bounds for windows that
	 * live on a different page from the one currently displayed -- their
	 * vx/vy are correct and the render correctly places them off-screen.
	 * Boundary clamping is applied at placement time and when windows
	 * are explicitly moved or resized.
	 *
	 * Compute the global render position from vx/vy and the current
	 * page viewport offset.  With a 1x1 grid (page_vx=page_vy=0):
	 *   x = output->x + vx,  y = output->y + vy
	 * matching the single-page behaviour.  When the user scrolls to
	 * page (1,0):
	 *   page_vx = output_width  →  x = output->x + vx - output_width
	 * sliding the window one screen-width to the left.
	 */
	cow_view_sync_global_from_vxy(view);

	view_apply_node_position(view);
	view_apply_show_hide(view);
	view_apply_pending_clip(view);
}

void
cow_view_reassign_output(struct cow_view *view, struct cow_output *output)
{
	if (output == NULL || output == view->output)
		return;

	/* Remove from old output's sheet and visible list */
	wl_list_remove(&view->desk_views);
	wl_list_remove(&view->output_views);

	/* Assign to new output's current sheet */
	view->output = output;
	view->desk   = output->current_desk;
	wl_list_insert(&output->current_desk->views, &view->desk_views);
	wl_list_insert(&output->visible_views, &view->output_views);

	view->dec.dirty = true;
}

void
cow_view_update_output(struct cow_view *view)
{
	if (view->output == NULL)
		return;

	/* Use the centre of the view's frame to determine which
	 * output it belongs to. */
	int tt = view->dec.title_thickness;
	int cx = view->x + view->content_width / 2;
	int cy = view->y - tt / 2 + view->content_height / 2;

	struct cow_output *target = cow_output_at(cx, cy);
	if (target != NULL && target != view->output) {
		cow_view_reassign_output(view, target);
		cow_view_clear_orphan(view);
	}
}

void
cow_view_set_pending_geometry(struct cow_view *view, int x, int y, int w, int h)
{
	view->pending_resize = true;
	view->pending_x	     = x;
	view->pending_y	     = y;
	view->pending_width  = w;
	view->pending_height = h;
	view->dec.dirty	     = true;
	cow_wm_request_manage_dirty();
}

struct cow_view *
cow_view_find_by_identifier(const char *identifier)
{
	if (identifier == NULL)
		return NULL;
	struct cow_view *view;
	wl_list_for_each(view, &cow_wm.views, wm_link)
	{
		if (view->closed || view->identifier == NULL)
			continue;
		if (strcmp(view->identifier, identifier) == 0)
			return view;
	}
	return NULL;
}

bool
cow_view_matches_selector(const struct cow_view *view, const char *spec)
{
	if (view == NULL || spec == NULL || spec[0] == '\0')
		return false;
	switch (spec[0]) {
	case '%':
		if (view->app_id == NULL)
			return false;
		return strcmp(view->app_id, spec + 1) == 0;
	case '/': {
		if (view->title == NULL)
			return false;
		const char *pat	  = spec + 1;
		size_t	    plen  = strlen(pat);
		bool	    strip = (plen > 0 && pat[plen - 1] == '/');
		if (!strip)
			return strstr(view->title, pat) != NULL;
		char tmp[1024];
		if (plen - 1 >= sizeof(tmp))
			return false;
		memcpy(tmp, pat, plen - 1);
		tmp[plen - 1] = '\0';
		return strstr(view->title, tmp) != NULL;
	}
	case '#':
		if (view->identifier == NULL)
			return false;
		return strcmp(view->identifier, spec + 1) == 0;
	default:
		return false;
	}
}

struct cow_view *
cow_view_walk(struct cow_view *start, bool forward,
    bool (*pred)(const struct cow_view *))
{
	struct wl_list *head = &cow_wm.views;
	struct wl_list *cur  = &start->wm_link;

	for (;;) {
		cur = forward ? cur->next : cur->prev;
		if (cur == head) {
			/* Skip the list sentinel */
			cur = forward ? cur->next : cur->prev;
		}
		if (cur == &start->wm_link)
			return NULL; /* wrapped around without a match */

		struct cow_view *v = wl_container_of(cur, v, wm_link);
		if (pred(v))
			return v;
	}
}

/* ================================================================
 * View utility functions (moved from header)
 * ================================================================ */

bool
cow_view_focusable(const struct cow_view *v)
{
	return !v->closed && v->has_dimensions &&
	    !(v->flags & COW_VIEW_ICONIFIED_FLAG);
}

uint8_t
cow_view_effective_layer(const struct cow_view *v)
{
	if (v == NULL)
		return COW_CFG_DEFAULT_LAYER;
	/*
	 * Container members share a single layer.  Use the container's
	 * active member's layer when this view participates; if there's
	 * no active member (transient), fall back to the view's own.
	 */
	if (v->container_member != NULL) {
		struct cow_container *c = v->container_member->container;
		if (c != NULL && c->active != NULL && c->active->view != NULL)
			return c->active->view->layer;
	}
	return v->layer;
}

uint8_t
cow_view_set_layer(struct cow_view *view, int requested)
{
	if (view == NULL)
		return COW_CFG_DEFAULT_LAYER;

	int max = (cow_config != NULL) ? (int)cow_config->max_layer :
					 COW_CFG_MAX_LAYER;
	if (max < 0)
		max = 0;
	if (max > 255)
		max = 255;
	if (requested < 0)
		requested = 0;
	if (requested > max)
		requested = max;
	uint8_t new_layer = (uint8_t)requested;

	/* The rule payload is a small JSON object describing the
	 * transition; rule bodies can read it via $COW_EVENT in an
	 * `exec` line, e.g.:
	 *
	 *   rule -g -n log-layer -T on-layer-change \
	 *       exec sh -c 'echo "$COW_EVENT" >> ~/cow-layer.log'
	 */
	bool any_changed = false;
	if (view->container_member != NULL) {
		struct cow_container *c = view->container_member->container;
		struct cow_container_member *m;
		wl_list_for_each(m, &c->members, link)
		{
			if (m->view == NULL)
				continue;
			if (m->view->layer == new_layer)
				continue;
			uint8_t prev   = m->view->layer;
			m->view->layer = new_layer;
			any_changed    = true;

			char payload[64];

			/* FIXME: we can/should use cJSOJ here. */
			snprintf(payload, sizeof payload,
			    "{\"prev\":%u,\"current\":%u}", prev, new_layer);
			cow_rule_fire_json(COW_RULE_ON_LAYER_CHANGE, m->view,
			    m->view->desk, m->view->output, payload);
		}
	} else if (view->layer != new_layer) {
		uint8_t prev = view->layer;
		view->layer  = new_layer;
		any_changed  = true;

		char payload[64];
		snprintf(payload, sizeof payload,
		    "{\"prev\":%u,\"current\":%u}", prev, new_layer);
		cow_rule_fire_json(COW_RULE_ON_LAYER_CHANGE, view, view->desk,
		    view->output, payload);
	}

	if (any_changed) {
		cow_stack_mark_dirty();
		cow_wm_request_manage_dirty();
	}
	return new_layer;
}

void
cow_view_save_geometry(struct cow_view *view)
{
	/* Save virtual (page-grid) coordinates, not the rendered
	 * global position which may be negative when the window is
	 * on a different page from the current viewport. */
	view->saved_x	   = view->vx;
	view->saved_y	   = view->vy;
	view->saved_width  = view->content_width;
	view->saved_height = view->content_height;
}

void
cow_view_set_maximized(struct cow_view *view, bool max)
{
	bool changed = (view->maximized != max);

	/* Keep the per-axis flags in step with the unified one for
	 * call sites that don't model an axis -- drag-to-unmax,
	 * fullscreen toggle, state restore on session reload, etc.
	 * The cmd-window per-axis paths set maximized_h / maximized_v
	 * directly and then derive maximized from their OR. */
	view->maximized	  = max;
	view->maximized_h = max;
	view->maximized_v = max;

	/*
	 * Notify status clients of the flip so per-window UI (the
	 * pager's maximized hint, status bars, etc.) update without
	 * having to wait for an unrelated status event.  The
	 * dimensions handler will emit again once the client acks the
	 * resize; that's fine, status snapshots are cheap.
	 */
	if (changed)
		cow_status_emit();
}

struct cow_box
cow_view_frame_box(const struct cow_view *v)
{
	return cow_view_frame_box_at(v, v->x, v->y);
}

struct cow_box
cow_view_frame_box_at(const struct cow_view *v, int x, int y)
{
	int bw = v->dec.border_width;
	int tt = v->dec.title_thickness;
	return (struct cow_box) {
		.x	= x - bw,
		.y	= y - bw - tt,
		.width	= v->content_width + 2 * bw,
		.height = v->content_height + bw + tt + bw,
	};
}

/* ================================================================
 * View helper functions (public API used by wm.c, cmd-*.c)
 * ================================================================ */

int
cow_view_maximize_button_index(const struct cow_view *view)
{
	const struct cow_titlebar_config *tb = (view != NULL) ?
	    &view->cfg.titlebar :
	    (cow_config != NULL ? &cow_config->titlebar : NULL);
	if (tb == NULL)
		return -1;
	for (int i = 0; i < COW_TITLEBAR_NUM_BUTTONS; i++) {
		const char *action = tb->buttons[i].action;
		if (action != NULL &&
		    strncmp(action, "window-maximize", 15) == 0)
			return i;
	}
	return -1;
}

void
cow_view_maximize(struct cow_view *view)
{
	if (view->output == NULL)
		return;

	/* Save virtual coordinates so cow_view_unmaximize can restore
	 * them correctly via vx/vy regardless of the current page.
	 * Only save if the window has been placed; if maximize_requested
	 * fires before the first render (e.g. Chrome at startup) vx/vy
	 * are still zero and saving them would cause unmaximize to
	 * teleport the window to vx=0,vy=0. */
	if (view->placed) {
		view->saved_x = view->vx;
		view->saved_y = view->vy;
	} else {
		/* Not yet placed — defer the save; unmaximize will fall
		 * back to placement coordinates set later. */
		view->saved_x = 0;
		view->saved_y = 0;
	}
	view->saved_width  = view->content_width;
	view->saved_height = view->content_height;

	struct cow_box *ua = &view->output->usable_area;
	int		bw = view->dec.border_width;
	int		tt = view->dec.title_thickness;

	view->x = ua->x + bw;
	view->y = ua->y + bw + tt;
	cow_view_sync_vxy_from_global(view);
	int w = ua->width - 2 * bw;
	int h = ua->height - 2 * bw - tt;
	cow_clamp_dimensions(&w, &h);
	river_window_v1_propose_dimensions(view->river_window, w, h);
	river_window_v1_inform_maximized(view->river_window);
	cow_view_set_maximized(view, true);

	/*
	 * Sync the container with the maximized geometry.  Use the proposed
	 * w/h directly because the dimensions event hasn't arrived yet and
	 * view->content_width still holds the old value.
	 */
	cow_view_sync_container_geometry(view, view->x, view->y, w, h);

	int mbi = cow_view_maximize_button_index(view);
	if (mbi >= 0) {
		view->icon_inverted_button = mbi;
		view->dec.dirty		   = true;
	}
}

void
cow_view_unmaximize(struct cow_view *view)
{
	/* saved_x/y hold virtual (vx/vy) coordinates saved by
	 * cow_view_maximize.  If maximize fired before the window was
	 * placed (e.g. Chrome's startup maximize_requested), saved_x/y
	 * are zero and must not be restored — clear placed so normal
	 * placement runs on the next render instead. */
	if (!view->placed) {
		/* Placement hasn't run yet; just drop the maximized state
		 * and let cow_view_apply_render place the window normally. */
		cow_view_set_maximized(view, false);
		river_window_v1_propose_dimensions(view->river_window,
		    view->saved_width, view->saved_height);
		river_window_v1_inform_unmaximized(view->river_window);
		view->dec.dirty = true;
		return;
	}
	/* saved_x/y are virtual (vx/vy) coordinates */
	view->vx = view->saved_x;
	view->vy = view->saved_y;
	/* Recompute global render position from restored virtual coords */
	if (view->output != NULL && view->desk != NULL) {
		cow_view_sync_global_from_vxy(view);
	} else {
		view->x = view->saved_x;
		view->y = view->saved_y;
	}
	river_window_v1_propose_dimensions(view->river_window,
	    view->saved_width, view->saved_height);
	river_window_v1_inform_unmaximized(view->river_window);
	cow_view_set_maximized(view, false);

	/* Restore container geometry to the saved (pre-maximize) values.
	 * Use view->x/y (already recomputed from vx/vy) as the global coord. */
	cow_view_sync_container_geometry(view, view->x, view->y,
	    view->saved_width, view->saved_height);

	view->icon_inverted_button = -1;
	view->dec.dirty		   = true;
}

void
cow_view_shade(struct cow_view *view, int direction)
{
	if (view->sd != COW_SHADE_NONE) {
		cow_view_unshade(view);
		return;
	}

	if (direction == 0)
		direction = COW_SHADE_NORTH;

	view->shade_saved_width	 = view->content_width;
	view->shade_saved_height = view->content_height;
	view->sd		 = direction;

	/* Clip the window content to 1x1 pixel so it's effectively
	 * invisible.  0x0 disables clipping per the protocol.
	 * Applied during the render callback via pending_clip. */
	view->pending_clip = true;
	view->clip_x	   = 0;
	view->clip_y	   = 0;
	view->clip_w	   = 1;
	view->clip_h	   = 1;
	cow_wm_request_manage_dirty();

	view->dec.dirty = true;

	cow_rule_fire(COW_RULE_ON_SHADE, view, view->desk, view->output);
}

void
cow_view_unshade(struct cow_view *view)
{
	if (view->sd == COW_SHADE_NONE)
		return;

	int sw = view->shade_saved_width;
	int sh = view->shade_saved_height;

	view->sd = COW_SHADE_NONE;

	view->content_width  = sw;
	view->content_height = sh;

	/* Disable clipping (0 width/height = disabled per protocol). */
	view->pending_clip = true;
	view->clip_x	   = 0;
	view->clip_y	   = 0;
	view->clip_w	   = 0;
	view->clip_h	   = 0;

	view->dec.dirty		      = true;
	view->dec.last_content_width  = -1;
	view->dec.last_content_height = -1;
	if (view->dec.buffer != NULL) {
		cow_shm_buffer_destroy(view->dec.buffer);
		view->dec.buffer = NULL;
	}

	cow_wm_request_manage_dirty();

	cow_rule_fire(COW_RULE_ON_UNSHADE, view, view->desk, view->output);
}

void
cow_view_fullscreen(struct cow_view *view)
{
	if (view->output == NULL)
		return;

	if (!view->fullscreen) {
		/*
		 * Save the virtual position (vx/vy) rather than the rendered
		 * global position (x/y).  view->x is the render-cycle output
		 * of "output->x + (vx - page_vx)" and may be a stale negative
		 * value if the window is on a different page from the viewport.
		 * Restoring from vx/vy via pending_resize gives the correct
		 * position on whichever page the window lives on.
		 */
		view->saved_x	   = view->vx;
		view->saved_y	   = view->vy;
		view->saved_width  = view->content_width;
		view->saved_height = view->content_height;
	}

	view->x	 = view->output->x;
	view->y	 = view->output->y;
	view->vx = 0;
	view->vy = 0;
	river_window_v1_propose_dimensions(view->river_window,
	    view->output->width, view->output->height);
	river_window_v1_fullscreen(view->river_window,
	    view->output->river_output);
	river_window_v1_inform_fullscreen(view->river_window);
	view->fullscreen = true;
	view->use_ssd	 = false;
}

void
cow_view_exit_fullscreen(struct cow_view *view)
{
	/*
	 * saved_x/saved_y hold the virtual (vx/vy) coordinates from before
	 * fullscreen — restore them directly.  The render cycle will compute
	 * the on-screen position from vx/vy and the current page offset.
	 */
	view->vx = view->saved_x;
	view->vy = view->saved_y;
	if (view->output != NULL) {
		if (view->desk != NULL) {
			cow_view_sync_global_from_vxy(view);
		} else {
			view->x = view->output->x + view->vx;
			view->y = view->output->y + view->vy;
		}
	}
	river_window_v1_propose_dimensions(view->river_window,
	    view->saved_width, view->saved_height);
	river_window_v1_exit_fullscreen(view->river_window);
	river_window_v1_inform_not_fullscreen(view->river_window);
	view->fullscreen = false;
	view->use_ssd	 = true;
}

/* ================================================================
 * Iconify (minimize) support
 * ================================================================ */

/*
 * Compute icon positions for all iconified views on an output.
 * Icons are laid out in a row at the bottom of the usable area.
 */
void
cow_view_layout_icons_on_output(struct cow_output *output)
{
	/* Use the global icon_size for the row geometry; individual
	 * views may carry a different icon_size in their snapshot but
	 * the layout grid is uniform per-output to keep icons aligned. */
	int icon_sz = cow_config ? cow_config->icon_size :
				   COW_CFG_DEFAULT_ICON_SIZE;
	int icon_h  = icon_sz + 20; /* extra height for text below */
	int icon_w  = icon_sz;
	int gap	    = 8;
	int x	    = output->usable_area.x + gap;
	int y = output->usable_area.y + output->usable_area.height - icon_h -
	    gap;

	struct cow_view *view;
	wl_list_for_each(view, &cow_wm.views, wm_link)
	{
		if (!cow_view_is_iconified(view) || view->closed)
			continue;
		if (view->output != output)
			continue;

		view->icon_width = icon_w;
		view->icon_height = icon_h;
		if (view->icon_manual_position)
			continue;

		view->icon_x	  = x;
		view->icon_y	  = y;

		x += icon_w + gap;
		if (x + icon_w >
		    output->usable_area.x + output->usable_area.width) {
			x = output->usable_area.x + gap;
			y -= icon_h + gap;
		}
	}
}

/*
 * Render the icon surface for an iconified view.
 * Draws a bordered rectangle with the app_id as text.
 */
/* Lazily create the icon's wl_surface, river shell surface, and node.
 * Returns false if any allocation failed (caller should bail). */
static bool
icon_ensure_surface(struct cow_view *view)
{
	if (view->icon_surface == NULL) {
		view->icon_surface = wl_compositor_create_surface(
		    cow_wm.compositor);
		if (view->icon_surface == NULL)
			return false;
	}
	if (view->icon_shell == NULL) {
		view->icon_shell = river_window_manager_v1_get_shell_surface(
		    cow_wm.manager, view->icon_surface);
		if (view->icon_shell == NULL)
			return false;
	}
	if (view->icon_node == NULL)
		view->icon_node = river_shell_surface_v1_get_node(
		    view->icon_shell);
	return true;
}

/* Allocate (or resize) the icon's SHM buffer.  Returns false on
 * allocation failure. */
static bool
icon_ensure_buffer(struct cow_view *view, int icon_w, int icon_h)
{
	if (view->icon_buffer != NULL &&
	    (view->icon_buffer->width != icon_w ||
		view->icon_buffer->height != icon_h)) {
		cow_shm_buffer_destroy(view->icon_buffer);
		view->icon_buffer = NULL;
	}
	if (view->icon_buffer == NULL) {
		view->icon_buffer = cow_shm_buffer_create(icon_w, icon_h);
		if (view->icon_buffer == NULL)
			return false;
	}
	return true;
}

/* Fill the icon box (centred at the top of the icon area) and draw
 * 3D relief if border_style != NONE. */
static void
icon_draw_box(struct cow_pixbuf *p, int icon_sz, uint32_t px_base,
    uint32_t px_lt, uint32_t px_dk, enum cow_border_style border_style)
{
	int box_x = (p->width - icon_sz) / 2;
	int box_y = 0;
	int bw	  = 3;

	cow_draw_fill_rect(p, box_x, box_y, icon_sz, icon_sz, px_base);

	if (border_style == COW_BORDER_STYLE_NONE)
		return;

	for (int r = 0; r < bw; r++) {
		cow_draw_hline(p, box_x + r, box_x + icon_sz - 1 - r, box_y + r,
		    px_lt);
		cow_draw_vline(p, box_x + r, box_y + r, box_y + icon_sz - 1 - r,
		    px_lt);
		cow_draw_hline(p, box_x + r, box_x + icon_sz - 1 - r,
		    box_y + icon_sz - 1 - r, px_dk);
		cow_draw_vline(p, box_x + icon_sz - 1 - r, box_y + r,
		    box_y + icon_sz - 1 - r, px_dk);
	}
}

static void
icon_draw_image(struct cow_pixbuf *p, int icon_sz, const char *path)
{
	if (path == NULL || icon_sz <= 0)
		return;

	int box_x = (p->width - icon_sz) / 2;
	int pad = icon_sz >= 16 ? 4 : 1;
	int size = icon_sz - 2 * pad;

	if (size <= 0)
		return;
	cow_image_render_rect(path, p, box_x + pad, pad, size, size);
}

/* Render the icon's app_id label into the strip below the icon box. */
static void
icon_draw_label(struct cow_pixbuf *p, int icon_sz, const char *label,
    const float *fg, const struct cow_colour_triple *bc_t,
    enum cow_border_style border_style, bool relief)
{
	int text_y	= icon_sz + 2;
	int text_area_h = p->height - text_y;
	if (text_area_h <= 0)
		return;

	if (relief) {
		cow_draw_fill_rect(p, 0, text_y, p->width, text_area_h,
		    bc_t->base);
		if (border_style != COW_BORDER_STYLE_NONE) {
			int bw = 3;
			for (int r = 0; r < bw && r < text_area_h / 2; r++) {
				cow_draw_hline(p, r, p->width - 1 - r,
				    text_y + r, bc_t->lt);
				cow_draw_vline(p, r, text_y + r,
				    text_y + text_area_h - 1 - r, bc_t->lt);
				cow_draw_hline(p, r, p->width - 1 - r,
				    text_y + text_area_h - 1 - r, bc_t->dk);
				cow_draw_vline(p, p->width - 1 - r, text_y + r,
				    text_y + text_area_h - 1 - r, bc_t->dk);
			}
		}
	}

	/* Create a cairo surface over just the text area. */
	cairo_surface_t *cs = cairo_image_surface_create_for_data(
	    (unsigned char *)(p->pixels + text_y * p->stride_px),
	    CAIRO_FORMAT_ARGB32, p->width, text_area_h, p->stride_px * 4);
	cairo_t *cr = cairo_create(cs);

	const char	     *fname  = cow_config && cow_config->font_name ?
		       cow_config->font_name :
		       "monospace 9";
	PangoLayout	     *layout = pango_cairo_create_layout(cr);
	PangoFontDescription *fd = pango_font_description_from_string(fname);
	/* Use a smaller font for the icon label */
	pango_font_description_set_size(fd, 8 * PANGO_SCALE);
	pango_layout_set_font_description(layout, fd);
	pango_layout_set_text(layout, label, -1);
	pango_layout_set_width(layout, p->width * PANGO_SCALE);
	pango_layout_set_alignment(layout, PANGO_ALIGN_CENTER);
	pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);

	cairo_set_source_rgba(cr, fg[0], fg[1], fg[2], fg[3]);
	pango_cairo_show_layout(cr, layout);

	g_object_unref(layout);
	pango_font_description_free(fd);
	cairo_destroy(cr);
	cairo_surface_destroy(cs);
}

/* Stipple every base-coloured pixel with alternating light/dark to
 * mark sticky iconified windows. */
static void
icon_draw_sticky_stipple(struct cow_pixbuf *p, uint32_t px_base, uint32_t px_lt,
    uint32_t px_dk)
{
	for (int y = 0; y < p->height; y++) {
		uint32_t *row = p->pixels + y * p->stride_px;
		for (int x = 0; x < p->width; x++) {
			if (row[x] != px_base)
				continue;
			row[x] = ((x + y) % 2 == 0) ? px_lt : px_dk;
		}
	}
}

/* Attach the buffer, damage the surface, sync to the next commit, and
 * position the icon node. */
static void
icon_commit(struct cow_view *view, int icon_w, int icon_h)
{
	wl_surface_attach(view->icon_surface, view->icon_buffer->buffer, 0, 0);
	wl_surface_damage_buffer(view->icon_surface, 0, 0, icon_w, icon_h);
	river_shell_surface_v1_sync_next_commit(view->icon_shell);
	wl_surface_commit(view->icon_surface);

	river_node_v1_set_position(view->icon_node, view->icon_x, view->icon_y);
}

void
cow_view_icon_render(struct cow_view *view)
{
	if (!cow_view_is_iconified(view) || view->closed)
		return;
	if (view->output == NULL)
		return;

	int icon_w = view->icon_width;
	int icon_h = view->icon_height;
	if (icon_w <= 0 || icon_h <= 0)
		return;

	if (!icon_ensure_surface(view))
		return;
	if (!icon_ensure_buffer(view, icon_w, icon_h))
		return;

	struct cow_pixbuf p = {
		.pixels	   = view->icon_buffer->data,
		.stride_px = view->icon_buffer->stride / 4,
		.width	   = icon_w,
		.height	   = icon_h,
	};

	/* Clear to transparent. */
	cow_draw_clear_rect(&p, 0, 0, icon_w, icon_h);

	struct cow_colour_triple bc_t;
	cow_colour_make_triple(&bc_t, view->cfg.icon_background);

	int icon_sz = view->cfg.icon_size;

	icon_draw_box(&p, icon_sz, bc_t.base, bc_t.lt, bc_t.dk,
	    view->cfg.border_style);
	icon_draw_image(&p, icon_sz, view->cfg.icon_image);
	icon_draw_label(&p, icon_sz, view->app_id ? view->app_id : "?",
	    view->cfg.icon_foreground, &bc_t, view->cfg.border_style,
	    view->cfg.icon_title_relief);
	if (view->flags & COW_VIEW_STICKY_FLAG)
		icon_draw_sticky_stipple(&p, bc_t.base, bc_t.lt, bc_t.dk);
	icon_commit(view, icon_w, icon_h);
}

static void
cow_view_icon_destroy(struct cow_view *view)
{
	if (view->icon_buffer != NULL) {
		cow_shm_buffer_destroy(view->icon_buffer);
		view->icon_buffer = NULL;
	}
	if (view->icon_node != NULL) {
		view->icon_node = NULL;
	}
	if (view->icon_shell != NULL) {
		river_shell_surface_v1_destroy(view->icon_shell);
		view->icon_shell = NULL;
	}
	if (view->icon_surface != NULL) {
		wl_surface_destroy(view->icon_surface);
		view->icon_surface = NULL;
	}
}

void
cow_view_iconify(struct cow_view *view)
{
	if (cow_view_is_iconified(view))
		return;

	/* Save geometry for restore */
	cow_view_save_geometry(view);

	view->flags |= COW_VIEW_ICONIFIED_FLAG;

	/* Layout all icons on this output */
	if (view->output != NULL)
		cow_view_layout_icons_on_output(view->output);

	/* Focus next visible view */
	struct cow_view *next = cow_view_walk(view, true, cow_view_focusable);
	if (next != NULL && next != view) {
		cow_wm.pending_focus = next;
	}

	cow_wm_request_manage_dirty();

	cow_rule_fire(COW_RULE_ON_ICONIFY, view, view->desk, view->output);
}

void
cow_view_deiconify(struct cow_view *view)
{
	if (!cow_view_is_iconified(view))
		return;

	view->flags &= ~COW_VIEW_ICONIFIED_FLAG;

	/* Destroy the icon surface */
	cow_view_icon_destroy(view);

	/* Re-layout remaining icons on this output */
	if (view->output != NULL)
		cow_view_layout_icons_on_output(view->output);

	/* Focus the restored window */
	view->needs_raise    = true;
	cow_wm.pending_focus = view;
	cow_wm_request_manage_dirty();

	cow_rule_fire(COW_RULE_ON_DEICONIFY, view, view->desk, view->output);
}

struct cow_view *
cow_view_find(struct river_window_v1 *river_window)
{
	struct cow_view *view;
	wl_list_for_each(view, &cow_wm.views, wm_link)
	{
		if (view->river_window == river_window)
			return view;
	}
	return NULL;
}

/*
 * Remove a view's decoration surfaces so the container can reassign them.
 */
static void
view_drop_decoration(struct cow_view *view)
{
	if (view->decoration != NULL) {
		river_decoration_v1_destroy(view->decoration);
		view->decoration = NULL;
	}
	if (view->decoration_surface != NULL) {
		wl_surface_destroy(view->decoration_surface);
		view->decoration_surface = NULL;
	}
	cow_decoration_fini(&view->dec);
	cow_decoration_init(view);
}

struct cow_container *
cow_container_create(struct cow_view *first, struct cow_view *second)
{
	struct cow_container *c = xcalloc(1, sizeof(*c));

	wl_list_init(&c->members);
	c->n_members = 0;
	c->id	     = next_container_id++;

	/* Geometry, desk and output come from the drop target (first) */
	c->x		  = first->x;
	c->y		  = first->y;
	c->content_width  = first->content_width;
	c->content_height = first->content_height;
	c->desk		  = first->desk;
	c->output	  = first->output;
	c->flags	  = first->flags;

	wl_list_insert(&cow_wm.containers, &c->wm_link);

	cow_container_add(c, first);
	cow_container_add(c, second);

	/* first (the drop target) is the active tab */
	c->active = wl_container_of(c->members.next, c->active, link);

	cow_wm_request_manage_dirty();
	return c;
}

bool
cow_container_add(struct cow_container *c, struct cow_view *view)
{
	if (view->container_member != NULL)
		return false;

	struct cow_container_member *m = xcalloc(1, sizeof(*m));
	m->view			       = view;
	m->container		       = c;
	wl_list_insert(c->members.prev, &m->link); /* append */
	c->n_members++;

	view->container_member = m;

	/* Save pre-container geometry for restoration on detach */
	m->pre_x      = view->x;
	m->pre_y      = view->y;
	m->pre_width  = view->content_width;
	m->pre_height = view->content_height;

	/* Non-active members must not draw their own frame */
	if (c->active != NULL && c->active != m) {
		view->use_ssd = false;
		view_drop_decoration(view);
	}

	/* Sync dimensions to container */
	cow_view_set_pending_geometry(view, c->x, c->y, c->content_width,
	    c->content_height);
	return true;
}

void
cow_container_activate(struct cow_container *c,
    struct cow_container_member		    *member)
{
	if (c->active == member)
		return;

	/* Deactivate the current active member.
	 * Do NOT destroy the decoration here: the old surface must remain
	 * alive until the render cycle so that window_interaction events
	 * from rapid clicks are still routed to a valid decoration surface.
	 * The render cycle's deferred cleanup (cow_view_apply_render) will
	 * destroy it once the new active member's decoration is ready. */
	if (c->active != NULL) {
		struct cow_view *old = c->active->view;
		old->use_ssd	     = false;
		old->dec.dirty	     = true;
		old->needs_raise     = false;
	}

	c->active		  = member;
	member->view->use_ssd	  = true;
	member->view->dec.dirty	  = true;
	member->view->needs_raise = true;

	/*
	 * Re-sync the new active member's geometry to the container's
	 * authoritative position.  Without this, a member whose vx/vy
	 * was set on a different page (e.g. the container was scrolled
	 * to another page after the member joined) would be deemed
	 * off-page by cow_view_visible() and silently hidden, making
	 * the tab click appear to do nothing while the user-visible
	 * focus appears to land on the wrong window.
	 */
	struct cow_view *nv = member->view;
	if (nv->output != NULL && nv->desk != NULL) {
		nv->x  = c->x;
		nv->y  = c->y;
		nv->vx = (nv->x - nv->output->x) + nv->desk->page_vx;
		nv->vy = (nv->y - nv->output->y) + nv->desk->page_vy;
	}

	cow_wm.pending_focus = member->view;
	cow_wm_request_manage_dirty();
}

static void
container_redraw(struct cow_container *c)
{
	if (c->active != NULL && c->active->view->use_ssd)
		c->active->view->dec.dirty = true;
	cow_wm_request_manage_dirty();
}

void
cow_container_swap_next(struct cow_container *c)
{
	if (c->active == NULL)
		return;
	struct wl_list *cur  = &c->active->link;
	struct wl_list *next = cur->next;
	if (next == &c->members)
		return; /* already the last tab */

	/* Remove cur from its current position then re-insert after next */
	wl_list_remove(cur);
	wl_list_insert(next, cur);
	container_redraw(c);
}

void
cow_container_swap_prev(struct cow_container *c)
{
	if (c->active == NULL)
		return;
	struct wl_list *cur  = &c->active->link;
	struct wl_list *prev = cur->prev;
	if (prev == &c->members)
		return; /* already the first tab */

	/* Remove cur then re-insert before prev (i.e. after prev->prev) */
	struct wl_list *before_prev = prev->prev;
	wl_list_remove(cur);
	wl_list_insert(before_prev, cur);
	container_redraw(c);
}

void
cow_container_activate_next(struct cow_container *c)
{
	struct cow_container_member *m;

	if (c->active == NULL) {
		if (wl_list_empty(&c->members))
			return;
		m = wl_container_of(c->members.next, m, link);
		cow_container_activate(c, m);
		return;
	}

	struct wl_list *next = c->active->link.next;
	if (next == &c->members)
		next = next->next; /* wrap */
	m = wl_container_of(next, m, link);
	cow_container_activate(c, m);
}

void
cow_container_activate_prev(struct cow_container *c)
{
	struct cow_container_member *m;

	if (c->active == NULL) {
		if (wl_list_empty(&c->members))
			return;
		m = wl_container_of(c->members.prev, m, link);
		cow_container_activate(c, m);
		return;
	}

	struct wl_list *prev = c->active->link.prev;
	if (prev == &c->members)
		prev = prev->prev; /* wrap */
	m = wl_container_of(prev, m, link);
	cow_container_activate(c, m);
}

void
cow_container_remove_view(struct cow_view *view)
{
	struct cow_container_member *m = view->container_member;
	if (m == NULL)
		return;

	struct cow_container *c		 = m->container;
	bool		      was_active = (c->active == m);

	wl_list_remove(&m->link);
	c->n_members--;
	view->container_member = NULL;
	int saved_pre_x	       = m->pre_x;
	int saved_pre_y	       = m->pre_y;
	int saved_pre_width    = m->pre_width;
	int saved_pre_height   = m->pre_height;
	xfree(m);
	m = NULL;

	/* Restore independent decoration */
	view->use_ssd = true;
	cow_decoration_fini(&view->dec);
	cow_decoration_init(view);

	/* Restore pre-container geometry */
	if (saved_pre_width > 0 && saved_pre_height > 0) {
		view->x = saved_pre_x;
		view->y = saved_pre_y;
		river_window_v1_propose_dimensions(view->river_window,
		    saved_pre_width, saved_pre_height);
		view->proposed_width  = saved_pre_width;
		view->proposed_height = saved_pre_height;
	} else {
		view->x = c->x + 30;
		view->y = c->y + 30;
	}
	view->dec.dirty = true;

	if (c->n_members <= 1) {
		/* Disband: release the last member too */
		if (!wl_list_empty(&c->members)) {
			struct cow_container_member *last =
			    wl_container_of(c->members.next, last, link);
			struct cow_view *lv = last->view;
			wl_list_remove(&last->link);
			c->n_members--;
			lv->container_member = NULL;
			xfree(last);
			lv->use_ssd = true;
			cow_decoration_fini(&lv->dec);
			cow_decoration_init(lv);
			lv->dec.dirty = true;
		}
		wl_list_remove(&c->wm_link);
		xfree(c);
	} else if (was_active) {
		/* Activate the first remaining member */
		struct cow_container_member *next =
		    wl_container_of(c->members.next, next, link);
		c->active = NULL; /* clear first so activate doesn't skip */
		cow_container_activate(c, next);
	}

	cow_wm_request_manage_dirty();
}

void
cow_container_destroy(struct cow_container *c)
{
	struct cow_container_member *m, *tmp;
	wl_list_for_each_safe(m, tmp, &c->members, link)
	{
		m->view->container_member = NULL;
		m->view->use_ssd	  = true;
		wl_list_remove(&m->link);
		xfree(m);
	}
	wl_list_remove(&c->wm_link);
	xfree(c);
}

bool
cow_view_is_iconified(const struct cow_view *view)
{
	return (view->flags & COW_VIEW_ICONIFIED_FLAG) != 0;
}

struct cow_container *
cow_view_container(const struct cow_view *view)
{
	if (view->container_member == NULL)
		return NULL;
	return view->container_member->container;
}

bool
cow_view_is_active_tab(const struct cow_view *view)
{
	if (view->container_member == NULL)
		return true; /* uncontained views are always "active" */
	return view->container_member->container->active ==
	    view->container_member;
}

bool
cow_view_is_drop_target(const struct cow_view *view)
{
	return cow_wm.drag_drop_target == view;
}

void
cow_clamp_dimensions(int *w, int *h)
{
	if (*w < COW_MIN_WINDOW_DIM)
		*w = COW_MIN_WINDOW_DIM;
	if (*h < COW_MIN_WINDOW_DIM)
		*h = COW_MIN_WINDOW_DIM;
}

bool
cow_view_skip_for_expand(const struct cow_view *other,
    const struct cow_view		       *self)
{
	return other == self || other->closed ||
	    (other->flags & COW_VIEW_ICONIFIED_FLAG) ||
	    other->output != self->output;
}

void
cow_view_sync_container_pos(struct cow_view *view)
{
	if (view->container_member == NULL)
		return;

	struct cow_container	    *c = view->container_member->container;
	struct cow_container_member *m;

	c->x = view->x;
	c->y = view->y;

	wl_list_for_each(m, &c->members, link)
	{
		if (m->view == view)
			continue;
		m->view->x = c->x;
		m->view->y = c->y;

		/*
		 * Keep the sibling's virtual (vx/vy) coords in step with
		 * its global ones.  Without this, cow_view_apply_render
		 * later recomputes x/y from the stale vx/vy
		 */
		if (m->view->output != NULL && m->view->desk != NULL) {
			m->view->vx = (m->view->x - m->view->output->x) +
			    m->view->desk->page_vx;
			m->view->vy = (m->view->y - m->view->output->y) +
			    m->view->desk->page_vy;
		}
		m->view->dec.dirty = true;
	}
}

void
cow_view_sync_container_geometry(struct cow_view *view, int x, int y, int w,
    int h)
{
	if (view->container_member == NULL)
		return;

	struct cow_container	    *c = view->container_member->container;
	struct cow_container_member *m;

	c->x		  = x;
	c->y		  = y;
	c->content_width  = w;
	c->content_height = h;

	wl_list_for_each(m, &c->members, link)
	{
		if (m->view == view)
			continue;
		m->view->x = x;
		m->view->y = y;

		if (m->view->output != NULL && m->view->desk != NULL) {
			m->view->vx = (m->view->x - m->view->output->x) +
			    m->view->desk->page_vx;
			m->view->vy = (m->view->y - m->view->output->y) +
			    m->view->desk->page_vy;
		}
		m->view->dec.dirty = true;
		cow_view_set_pending_geometry(m->view, x, y, w, h);
	}
}

void
cow_view_warp_to_focused(void)
{
	const struct cow_view *v = cow_wm.pending_focus != NULL ?
	    cow_wm.pending_focus :
	    cow_wm.focused_view;
	int		       cx, cy;

	if (v == NULL)
		return;

	cow_view_centre(v, &cx, &cy);
	cow_wm_request_pointer_warp(cx, cy);
	cow_wm_request_manage_dirty();
}

void
cow_view_centre(const struct cow_view *v, int *cx, int *cy)
{
	*cx = v->x + v->content_width / 2;
	*cy = v->y - v->dec.title_thickness / 2 + v->content_height / 2;
}

void
cow_view_sync_vxy_from_global(struct cow_view *view)
{
	if (view->output == NULL || view->desk == NULL)
		return;
	view->vx = (view->x - view->output->x) + view->desk->page_vx;
	view->vy = (view->y - view->output->y) + view->desk->page_vy;
}

void
cow_view_sync_global_from_vxy(struct cow_view *view)
{
	if (view->output == NULL || view->desk == NULL)
		return;
	view->x = view->output->x + (view->vx - view->desk->page_vx);
	view->y = view->output->y + (view->vy - view->desk->page_vy);
}

void
cow_view_restore_saved_geometry(struct cow_view *view)
{
	/*
	 * saved_x/y are in virtual (vx/vy) coordinates.  Convert to global
	 * for the pending_resize, which will re-sync vx/vy when applied.
	 */
	int gx = view->saved_x;
	int gy = view->saved_y;
	if (view->output != NULL && view->desk != NULL) {
		gx = view->output->x + (view->saved_x - view->desk->page_vx);
		gy = view->output->y + (view->saved_y - view->desk->page_vy);
	}
	cow_view_set_pending_geometry(view, gx, gy, view->saved_width,
	    view->saved_height);
	cow_view_set_maximized(view, false);
	view->icon_inverted_button = -1;
}

void
cow_view_json_add_identity(struct cJSON *obj, const struct cow_view *view)
{
	cJSON_AddStringToObject(obj, "id",
	    view->identifier ? view->identifier : "");
	cJSON_AddStringToObject(obj, "app_id",
	    view->app_id ? view->app_id : "");
	cJSON_AddStringToObject(obj, "title", view->title ? view->title : "");
}

void
cow_view_json_add_placement(struct cJSON *obj, const struct cow_view *view)
{
	cJSON_AddNumberToObject(obj, "desk", view->desk ? view->desk->nr : -1);
	cJSON_AddStringToObject(obj, "output",
	    (view->output && view->output->name) ? view->output->name : "");
	cJSON_AddNumberToObject(obj, "output_number",
	    view->output ? view->output->number : 0);
}
