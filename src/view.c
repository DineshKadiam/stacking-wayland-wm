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
#include "cow/log.h"

#include <cJSON.h>
#include <cairo/cairo.h>
#include <fnmatch.h>
#include <pango/pangocairo.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cow/app_icon.h"
#include "cow/colour.h"
#include "cow/cmd.h"
#include "cow/config.h"
#include "cow/decoration.h"
#include "cow/desk_ops.h"
#include "cow/draw.h"
#include "cow/draw_exec.h"
#include "cow/format.h"
#include "cow/gradient.h"
#include "cow/image.h"
#include "cow/icon_placement.h"
#include "cow/keybinding.h"
#include "cow/keyboard_layout.h"
#include "cow/memory.h"
#include "cow/output.h"
#include "cow/options.h"
#include "cow/placement.h"
#include "cow/pick.h"
#include "cow/pointer_op.h"
#include "cow/rule.h"
#include "cow/seat.h"
#include "cow/selection.h"
#include "cow/shm.h"
#include "cow/stack.h"
#include "cow/style.h"
#include "cow/titlebar.h"
#include "cow/util.h"
#include "cow/update.h"
#include "cow/wm.h"

#include "river-window-management-v1-client-protocol.h"

#define COW_MIN_ID_PREFIX 8

/*
 * Remove a view's decoration surfaces so the container can reassign them.
 */
void
cow_view_drop_decoration(struct cow_view *view)
{
	cow_decoration_fini(&view->dec);
	if (view->decoration != NULL) {
		river_decoration_v1_destroy(view->decoration);
		view->decoration = NULL;
	}
	if (view->decoration_surface != NULL) {
		wl_surface_destroy(view->decoration_surface);
		view->decoration_surface = NULL;
	}
	cow_decoration_init(view);
}

static void cow_view_icon_destroy(struct cow_view *);
static void view_refit_maximized(struct cow_view *);

static bool
shade_uses_titlebar_axis(const struct cow_view *view, int direction)
{
	switch (view->cfg.titlebar.position) {
	case COW_TITLEBAR_TOP:
	case COW_TITLEBAR_BOTTOM:
		return direction == COW_SHADE_NORTH ||
		    direction == COW_SHADE_SOUTH;
	case COW_TITLEBAR_LEFT:
	case COW_TITLEBAR_RIGHT:
		return direction == COW_SHADE_EAST ||
		    direction == COW_SHADE_WEST;
	default:
		return false;
	}
}

static int
titlebar_button_index(enum cow_view_cfg_option option,
    enum cow_view_cfg_option first)
{
	if (option < first ||
	    option > COW_VIEW_CFG_TITLEBAR_BUTTON_9_INACTIVE_FG)
		return -1;

	int offset = option - first;
	if (offset % COW_VIEW_CFG_TITLEBAR_BUTTON_STRIDE != 0)
		return -1;

	int idx = offset / COW_VIEW_CFG_TITLEBAR_BUTTON_STRIDE;
	return idx < COW_TITLEBAR_NUM_BUTTONS ? idx : -1;
}

static int
titlebar_button_vector_index(enum cow_view_cfg_option option)
{
	return titlebar_button_index(option,
	    COW_VIEW_CFG_TITLEBAR_BUTTON_0_VECTOR);
}

static int
titlebar_button_image_index(enum cow_view_cfg_option option)
{
	return titlebar_button_index(option,
	    COW_VIEW_CFG_TITLEBAR_BUTTON_0_IMAGE);
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
		dst->icon_enabled = true;
		dst->icon_size	   = COW_CFG_DEFAULT_ICON_SIZE;
		dst->icon_label_area = COW_CFG_DEFAULT_ICON_LABEL_AREA;
		dst->border_style  = COW_BORDER_STYLE_FVWM;
		dst->icon_title_relief = COW_RELIEF_RAISED;
		return;
	}
	dst->border	     = cow_config->border;
	dst->corner_length   = cow_config->corner_length;
	dst->handles	     = cow_config->handles;
	dst->border_style    = cow_config->border_style;
	cow_border_colour_copy(&dst->border_active,
	    &cow_config->border_active);
	cow_border_colour_copy(&dst->border_inactive,
	    &cow_config->border_inactive);
	memcpy(dst->border_resize, cow_config->border_resize,
	    sizeof(dst->border_resize));
	dst->has_border_resize = cow_config->has_border_resize;
	cow_titlebar_config_copy(&dst->titlebar, &cow_config->titlebar);
	cow_container_config_copy(&dst->container, &cow_config->container);
	dst->icon_enabled	= cow_config->icon_enabled;
	dst->icon_size		= cow_config->icon_size;
	dst->icon_label_area = cow_config->icon_label_area;
	dst->icon_image		= xstrdup0(cow_config->icon_image);
	dst->icon_format	= xstrdup0(cow_config->icon_format);
	dst->title_format	= xstrdup0(cow_config->title_format);
	dst->pane_title_format = xstrdup0(cow_config->pane_title_format);
	dst->icon_background = cow_config->icon_background;
	dst->icon_foreground = cow_config->icon_foreground;
	dst->icon_active_background = cow_config->icon_active_background;
	dst->icon_active_foreground = cow_config->icon_active_foreground;
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

bool
cow_view_cfg_is_override(const struct cow_view_cfg *cfg,
    enum cow_view_cfg_option option)
{
	size_t word;
	uint64_t bit;

	return (cfg != NULL && view_cfg_option_index(option, &word, &bit) &&
	    (cfg->overrides[word] & bit) != 0);
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

	const struct cow_option_entry *entry =
	    cow_option_for_view_cfg(option);
	if (entry != NULL) {
		void *dst = (char *)cfg + entry->view_offset;
		const void *src = (const char *)cow_config + entry->config_offset;
		switch (entry->action) {
		case COW_OPTION_SCALAR_BOOL:
			memcpy(dst, src, sizeof(bool));
			return;
		case COW_OPTION_SCALAR_INT:
			memcpy(dst, src, sizeof(int));
			return;
		case COW_OPTION_SCALAR_STRING:
		case COW_OPTION_IMAGE:
			xstrset(dst, *(char *const *)src);
			return;
		case COW_OPTION_COLOUR:
			memcpy(dst, src, sizeof(float) * 4);
			return;
		case COW_OPTION_OPTIONAL_COLOUR:
			memcpy(dst, src, sizeof(struct cow_optional_colour));
			return;
		default:
			break;
		}
	}

	int idx = titlebar_button_vector_index(option);
	if (idx != -1) {
		cfg->titlebar.buttons[idx].vector =
		    cow_config->titlebar.buttons[idx].vector;
		return;
	}
	idx = titlebar_button_image_index(option);
	if (idx != -1) {
		xstrset(&cfg->titlebar.buttons[idx].image,
		    cow_config->titlebar.buttons[idx].image);
		return;
	}
	idx = titlebar_button_index(option,
	    COW_VIEW_CFG_TITLEBAR_BUTTON_0_ACTIVE_RELIEF);
	if (idx != -1) {
		cfg->titlebar.buttons[idx].active_relief =
		    cow_config->titlebar.buttons[idx].active_relief;
		return;
	}
	idx = titlebar_button_index(option,
	    COW_VIEW_CFG_TITLEBAR_BUTTON_0_INACTIVE_RELIEF);
	if (idx != -1) {
		cfg->titlebar.buttons[idx].inactive_relief =
		    cow_config->titlebar.buttons[idx].inactive_relief;
		return;
	}
	idx = titlebar_button_index(option,
	    COW_VIEW_CFG_TITLEBAR_BUTTON_0_ACTIVE_COLOUR);
	if (idx != -1) {
		memcpy(cfg->titlebar.buttons[idx].active_colour,
		    cow_config->titlebar.buttons[idx].active_colour,
		    sizeof(cfg->titlebar.buttons[idx].active_colour));
		cfg->titlebar.buttons[idx].has_active_colour =
		    cow_config->titlebar.buttons[idx].has_active_colour;
		return;
	}
	idx = titlebar_button_index(option,
	    COW_VIEW_CFG_TITLEBAR_BUTTON_0_INACTIVE_COLOUR);
	if (idx != -1) {
		memcpy(cfg->titlebar.buttons[idx].inactive_colour,
		    cow_config->titlebar.buttons[idx].inactive_colour,
		    sizeof(cfg->titlebar.buttons[idx].inactive_colour));
		cfg->titlebar.buttons[idx].has_inactive_colour =
		    cow_config->titlebar.buttons[idx].has_inactive_colour;
		return;
	}
	idx = titlebar_button_index(option,
	    COW_VIEW_CFG_TITLEBAR_BUTTON_0_ACTIVE_FG);
	if (idx != -1) {
		memcpy(cfg->titlebar.buttons[idx].active_fg,
		    cow_config->titlebar.buttons[idx].active_fg,
		    sizeof(cfg->titlebar.buttons[idx].active_fg));
		cfg->titlebar.buttons[idx].has_active_fg =
		    cow_config->titlebar.buttons[idx].has_active_fg;
		return;
	}
	idx = titlebar_button_index(option,
	    COW_VIEW_CFG_TITLEBAR_BUTTON_0_INACTIVE_FG);
	if (idx != -1) {
		memcpy(cfg->titlebar.buttons[idx].inactive_fg,
		    cow_config->titlebar.buttons[idx].inactive_fg,
		    sizeof(cfg->titlebar.buttons[idx].inactive_fg));
		cfg->titlebar.buttons[idx].has_inactive_fg =
		    cow_config->titlebar.buttons[idx].has_inactive_fg;
		return;
	}

	switch (option) {
	case COW_VIEW_CFG_BORDER_STYLE:
		cfg->border_style = cow_config->border_style;
		break;
	case COW_VIEW_CFG_BORDER_RESIZE:
		memcpy(cfg->border_resize, cow_config->border_resize,
		    sizeof(cfg->border_resize));
		cfg->has_border_resize = cow_config->has_border_resize;
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
		if (dst != NULL && src != NULL)
			xstrset(dst, *src);
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
		if (dst != NULL && src != NULL)
			xstrset(dst, *src);
		break;
	}
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
		xstrset(&cfg->titlebar.active_image,
		    cow_config->titlebar.active_image);
		break;
	case COW_VIEW_CFG_TITLEBAR_INACTIVE_IMAGE:
		xstrset(&cfg->titlebar.inactive_image,
		    cow_config->titlebar.inactive_image);
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
	case COW_VIEW_CFG_TITLEBAR_POSITION:
		cfg->titlebar.position = cow_config->titlebar.position;
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
	case COW_VIEW_CFG_TITLEBAR_ACTIVE_RELIEF:
		cfg->titlebar.active_relief = cow_config->titlebar.active_relief;
		break;
	case COW_VIEW_CFG_TITLEBAR_INACTIVE_RELIEF:
		cfg->titlebar.inactive_relief =
		    cow_config->titlebar.inactive_relief;
		break;
	case COW_VIEW_CFG_TITLEBAR_ACTIVE_TITLE_JUSTIFY:
		cfg->titlebar.active_title_justify =
		    cow_config->titlebar.active_title_justify;
		break;
	case COW_VIEW_CFG_TITLEBAR_INACTIVE_TITLE_JUSTIFY:
		cfg->titlebar.inactive_title_justify =
		    cow_config->titlebar.inactive_title_justify;
		break;
	case COW_VIEW_CFG_TITLEBAR_BUTTON_STYLE:
		cfg->titlebar.button_style = cow_config->titlebar.button_style;
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
		xstrset(&cfg->container.tab_active_image,
		    cow_config->container.tab_active_image);
		break;
	case COW_VIEW_CFG_CONTAINER_TAB_INACTIVE_IMAGE:
		xstrset(&cfg->container.tab_inactive_image,
		    cow_config->container.tab_inactive_image);
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
	case COW_VIEW_CFG_FONT:
		xstrset(&cfg->font_name, cow_config->font_name);
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
cow_view_cfg_rebase_global(struct cow_view_cfg *cfg)
{
	for (enum cow_view_cfg_option option = 0;
	    option < COW_VIEW_CFG_OPTION_COUNT; option++)
		cow_view_cfg_apply_global_option(cfg, option);
}

void
cow_view_cfg_fini(struct cow_view_cfg *cfg)
{
	xfree(cfg->font_name);
	xfree(cfg->title_format);
	xfree(cfg->pane_title_format);
	xfree(cfg->icon_image);
	xfree(cfg->icon_format);
	cow_titlebar_config_fini(&cfg->titlebar);
	cow_container_config_fini(&cfg->container);
	cow_border_colour_fini(&cfg->border_active);
	cow_border_colour_fini(&cfg->border_inactive);
	cfg->font_name		= NULL;
	cfg->title_format	= NULL;
	cfg->pane_title_format = NULL;
	cfg->icon_image		= NULL;
	cfg->icon_format	= NULL;
}

static char *
short_identifier(const struct cow_view *view)
{
	const char *id = view->identifier;

	if (id == NULL || id[0] == '\0')
		return xstrdup("");

	size_t id_len	  = strlen(id);
	size_t prefix_len = COW_MIN_ID_PREFIX;
	if (prefix_len > id_len)
		prefix_len = id_len;

	/* Extend prefix until no other view shares it */
	while (prefix_len < id_len) {
		bool		 unique = true;
		struct cow_view *other;
		wl_list_for_each(other, &cow_wm.views, wm_link)
		{
			if (other == view || other->closed)
				continue;
			if (other->identifier == NULL)
				continue;
			if (strncmp(id, other->identifier, prefix_len) == 0) {
				unique = false;
				break;
			}
		}
		if (unique)
			break;
		prefix_len++;
	}

	char *out;
	xasprintf(&out, "%.*s", (int)prefix_len, id);
	return (out);
}

struct title_format_ctx {
	char		**buf;
	size_t		 *cap;
	size_t		 *len;
	const char	 *title;
	const char	 *short_id;
	const char	 *id;
	const char	 *app_id;
	const char	 *pane_name;
};

static void
append_mem(char **buf, size_t *cap, size_t *len, const char *s, size_t slen)
{
	if (*cap < *len + slen + 1) {
		size_t ncap = *cap == 0 ? 64 : *cap;

		while (ncap < *len + slen + 1)
			ncap *= 2;
		*buf = xrealloc(*buf, ncap, 1);
		*cap = ncap;
	}
	memcpy(*buf + *len, s, slen);
	*len += slen;
	(*buf)[*len] = '\0';
}

static void
append_str(char **buf, size_t *cap, size_t *len, const char *s)
{
	append_mem(buf, cap, len, s, strlen(s));
}

static void
title_format_text(void *data, const char *text, size_t len)
{
	struct title_format_ctx *ctx = data;

	append_mem(ctx->buf, ctx->cap, ctx->len, text, len);
}

static void
title_format_conversion(void *data, char conversion)
{
	struct title_format_ctx *ctx = data;
	const char		*value = "";

	switch (conversion) {
	case 'n':
		value = ctx->title;
		break;
	case 't':
		value = ctx->short_id;
		break;
	case 'I':
		value = ctx->id;
		break;
	case 'c':
		value = ctx->app_id;
		break;
	case 'p':
		value = ctx->pane_name;
		break;
	}
	append_str(ctx->buf, ctx->cap, ctx->len, value);
}

char *
cow_view_make_display_title(const struct cow_view *view, const char *fmt)
{
	if (fmt == NULL)
		fmt = "%n";
	const char *title = view->title != NULL ? view->title : "";
	const char *id = view->identifier != NULL ? view->identifier : "";
	const char *app_id = view->app_id != NULL ? view->app_id : "";
	struct cow_container_pane *pane = cow_view_pane(view);
	char	   *sid = short_identifier(view);
	char	   *out = NULL;
	size_t	    cap = 0, len = 0;
	struct title_format_ctx ctx = {
		.buf = &out,
		.cap = &cap,
		.len = &len,
		.title = title,
		.short_id = sid,
		.id = id,
		.app_id = app_id,
		.pane_name = pane && pane->name ? pane->name : "",
	};

	cow_format_parse(fmt, "ntIcp", false, title_format_text,
	    title_format_conversion, &ctx, NULL);
	if (out == NULL)
		out = xstrdup("");
	xfree(sid);
	return (out);
}

/*
 * river_window_v1 event handlers
 */

static void
window_handle_closed(void *data, __unused struct river_window_v1 *rw)
{
	struct cow_view *view = data;

	cow_log_info("window closed (app_id=%s)",
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
	(void)cow_container_refresh(view);
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
		cow_update_request(COW_UPDATE_MANAGE);
	}

	view->content_width  = width;
	view->content_height = height;
	/* Keep the geometry restored by unshade consistent when the window is
	 * hidden.
	 */
	if (view->sd != COW_SHADE_NONE) {
		view->shade_saved_width = width;
		view->shade_saved_height = height;
	}

	/*
	 * Apply a deferred position now that the new (smaller) buffer
	 * dimensions are in effect.  Without this defer/apply pair,
	 * a maximise that shrinks the window briefly renders the old
	 * (larger) buffer at the new top-left, which on multi-output
	 * setups visually overshoots into a neighbouring monitor.
	 */
	bool applied_deferred_position = view->deferred_position;
	if (view->deferred_position) {
		view->x = view->deferred_anchor_right ?
		    view->deferred_anchor_right_x - width :
		    view->deferred_x;
		view->y = view->deferred_anchor_bottom ?
		    view->deferred_anchor_bottom_y - height :
		    view->deferred_y;
		view->deferred_position = false;
		view->deferred_anchor_right = false;
		view->deferred_anchor_bottom = false;
		if (view->output != NULL && view->desk != NULL) {
			view->vx = (view->x - view->output->x) +
			    view->desk->page_vx;
			view->vy = (view->y - view->output->y) +
			    view->desk->page_vy;
		}
	}
	/*
	 * A tiled pane reports its own accepted size, not the group's size.
	 * Only explicit WM operations may change a split container's frame.
	 */
	if ((changed || applied_deferred_position) &&
	    !cow_container_is_split(view))
		cow_view_sync_container_geometry(view, view->x, view->y,
		    width, height);

	bool first_dims	     = !view->has_dimensions;
	view->has_dimensions = true;

	/*
	 * Fire on-map / on-desk on the first dimensions event.  Rule
	 * actions (window-move, etc.) require has_dimensions to be set,
	 * so firing earlier from cow_view_configure() would silently
	 * no-op them.
	 */
	if (first_dims && !view->on_map_fired) {
		cow_log_info("window mapped (id=%s app_id=%s title=%s size=%dx%d%s)",
		    view->identifier ? view->identifier : "unknown",
		    view->app_id ? view->app_id : "unknown",
		    view->title ? view->title : "", width, height,
		    view->parent != NULL ? " parented" : "");
		view->on_map_fired = true;
		cow_decor_apply_runtime_active(view);
		cow_rule_fire(COW_RULE_ON_MAP, view, view->desk,
		    view->output);
		cow_rule_fire(COW_RULE_ON_DESK, view, view->desk,
		    view->output);
		cow_style_apply_pending_initial(view);
	}

	view_refit_maximized(view);

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
	if (changed || applied_deferred_position)
		cow_update_request(COW_UPDATE_STATUS);
}

static void
view_mark_title_dirty(struct cow_view *view)
{
	view->dec.dirty = true;
	if (view->container_member == NULL)
		return;

	struct cow_container *c = view->container_member->container;
	if (c->active != NULL)
		c->active->view->dec.dirty = true;
}

static void
window_handle_app_id(void *data, __unused struct river_window_v1 *rw,
    const char *app_id)
{
	struct cow_view *view = data;

	xstrset(&view->app_id, app_id);
	cow_container_window_identify(view);
	xfree(view->application_icon);
	view->application_icon = cow_app_icon_resolve(view->app_id,
	    view->cfg.icon_size);
	if (cow_view_is_iconified(view))
		cow_view_icon_refresh(view);
	view_mark_title_dirty(view);
	if (view->on_map_fired)
		cow_style_apply_for_event(view, false);
	cow_update_request(COW_UPDATE_MANAGE | COW_UPDATE_STATUS);
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

	xstrset(&view->title, title);
	view_mark_title_dirty(view);
	cow_update_request(COW_UPDATE_MANAGE | COW_UPDATE_STATUS);

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
		cow_pointer_op_start_resize(seat, view, edges, false);
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
window_handle_presentation_hint(void *data,
    __unused struct river_window_v1 *rw, uint32_t hint)
{
	struct cow_view *view = data;

	if (hint != RIVER_OUTPUT_V1_PRESENTATION_MODE_ASYNC)
		hint = RIVER_OUTPUT_V1_PRESENTATION_MODE_VSYNC;

	view->presentation_hint = hint;
	cow_update_request(COW_UPDATE_MANAGE);
}

static void
window_handle_identifier(void *data, __unused struct river_window_v1 *rw,
    const char *identifier)
{
	struct cow_view *view = data;
	xstrset(&view->identifier, identifier);
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
	wl_list_init(&view->selections);

	river_window_v1_add_listener(rw, &window_listener, view);

	wl_list_insert(&cow_wm.views, &view->wm_link);
	cow_update_request(COW_UPDATE_STATUS);

	return view;
}

/* Clear all WM-wide back-references to 'view' (drag, pending focus,
 * focused). */
static void
view_clear_wm_refs(struct cow_view *view)
{
	struct cow_view *child;
	wl_list_for_each(child, &cow_wm.views, wm_link)
	{
		if (child->parent == view)
			child->parent = NULL;
	}
	if (cow_wm.drag_drop_target == view)
		cow_wm.drag_drop_target = NULL;
	if (cow_wm.drag_drop_source == view)
		cow_wm.drag_drop_source = NULL;
	if (cow_wm.pending_focus == view)
		cow_wm.pending_focus = NULL;
	if (cow_wm.focused_view == view)
		cow_wm_set_focused_view(NULL);
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


void
cow_view_destroy(struct cow_view *view)
{
	struct cow_output *output = view->output;
	bool		   was_iconified = cow_view_is_iconified(view);

	cow_update_request(COW_UPDATE_STATUS);
	cow_keyboard_layout_forget_view(view);
	view_clear_wm_refs(view);
	view_clear_seat_refs(view);
	cow_pick_forget_view(view);
	cow_container_window_forget(view);
	cow_container_forget_view(view);
	cow_view_icon_destroy(view);
	cow_selection_fini_view(view);

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
	xfree(view->application_icon);
	xfree(view->title);
	cow_style_cancel_pending_initial(view);
	xfree(view->keyboard_layout);
	cow_view_clear_orphan(view);
	cow_view_cfg_fini(&view->cfg);
	xfree(view);

	if (was_iconified && output != NULL && !output->removed)
		cow_view_layout_icons_on_output(output);
}

static void
view_clear_orphan(struct cow_view *view)
{
	xfree(view->orphan_output_name);
	view->orphan_output_name = NULL;
	view->orphan_desk_nr	 = -1;
	view->orphan_page_vx	 = 0;
	view->orphan_page_vy	 = 0;
}

void
cow_view_clear_orphan(struct cow_view *view)
{
	struct cow_container *c = cow_view_container(view);
	if (c == NULL) {
		view_clear_orphan(view);
		return;
	}
	struct cow_container_member *m;
	wl_list_for_each(m, &c->members, link)
	{
		view_clear_orphan(m->view);
	}
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
	struct cow_container *frame	= cow_container_window_owner(view);
	bool		      focus_new = !view->container_placeholder &&
	    (frame == NULL || cow_container_window_count(frame) == 0);
	view->needs_raise = focus_new;
	view->needs_focus = focus_new;
	cow_update_request(COW_UPDATE_MANAGE);
}

/* Forward declaration: defined later, used by view_cleanup_stale_decoration. */

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
	cow_view_drop_decoration(view);
}

/*
 * Compute the under-mouse placement origin.  Centres the frame on the
 * pointer position then clamps so the full frame stays within the
 * usable area (which already has reserved areas applied).  Clamping
 * max before min keeps the top-left visible on screens smaller than
 * the window.
 */
static void
view_place_undermouse(struct cow_view *view, int frame_w, int frame_h,
    int content_x, int content_y)
{
	struct cow_box *ua = &view->output->usable_area;
	int		px = ua->x + ua->width / 2;
	int		py = ua->y + ua->height / 2;
	cow_seat_get_pointer(&px, &py);
	view->x = px - frame_w / 2 + content_x;
	view->y = py - frame_h / 2 + content_y;

	if (ua->width > 0) {
		int max_x = ua->x + ua->width - frame_w + content_x;
		int min_x = ua->x + content_x;
		if (view->x > max_x)
			view->x = max_x;
		if (view->x < min_x)
			view->x = min_x;
	}
	if (ua->height > 0) {
		int max_y = ua->y + ua->height - frame_h + content_y;
		int min_y = ua->y + content_y;
		if (view->y > max_y)
			view->y = max_y;
		if (view->y < min_y)
			view->y = min_y;
	}
}

static void
view_place_centre(struct cow_view *view, int frame_w, int frame_h,
    int content_x, int content_y)
{
	struct cow_box *ua = &view->output->usable_area;
	view->x = ua->x + (ua->width - frame_w) / 2 + content_x;
	view->y = ua->y + (ua->height - frame_h) / 2 + content_y;
}

static void
view_place_cascade(struct cow_view *view, int content_x, int content_y, int gap)
{
	static int	cascade_offset = 0;
	struct cow_box *ua	       = &view->output->usable_area;
	view->x	       = ua->x + content_x + gap + cascade_offset;
	view->y	       = ua->y + content_y + gap + cascade_offset;
	cascade_offset = (cascade_offset + 30) % 150;
}

static bool
view_is_placement_obstacle(const struct cow_view *view,
    const struct cow_output *output)
{
	return view->placed && view->has_dimensions &&
	    cow_view_visible(view, output);
}

static void
view_place_empty(struct cow_view *view, int frame_w, int frame_h,
    int content_x, int content_y, int gap)
{
	const struct cow_box *usable = &view->output->usable_area;

	size_t obstacle_count = 0;
	struct cow_view *other;
	wl_list_for_each(other, &cow_wm.views, wm_link)
	{
		if (view_is_placement_obstacle(other, view->output))
			obstacle_count++;
	}

	struct cow_box *obstacles = NULL;
	if (obstacle_count > 0)
		obstacles = xcalloc(obstacle_count, sizeof(*obstacles));

	size_t i = 0;
	wl_list_for_each(other, &cow_wm.views, wm_link)
	{
		if (!view_is_placement_obstacle(other, view->output))
			continue;
		obstacles[i++] = cow_view_frame_box(other);
	}

	int x, y;
	bool found = cow_placement_find_empty(usable, frame_w, frame_h,
	    obstacles, obstacle_count, gap, &x, &y);
	xfree(obstacles);
	if (found) {
		view->x = x + content_x;
		view->y = y + content_y;
		return;
	}

	if (gap < 0)
		gap = 0;
	view_place_cascade(view, content_x, content_y, gap);
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

	if (cow_draw_exec_apply_pending(view)) {
		view_clamp_to_current_page(view);
		if (view->pending_resize) {
			view->pending_x = view->x;
			view->pending_y = view->y;
		}
		return;
	}

	struct cow_seat *ps = cow_seat_first_with_pointer();
	if (ps != NULL) {
		struct cow_output *ptr_out = cow_output_at(ps->pointer_x,
		    ps->pointer_y);
		if (ptr_out != NULL)
			cow_view_reassign_output(view, ptr_out);
	}

	int gap	     = cow_config ? cow_config->gap : COW_CFG_DEFAULT_GAP;
	int place_w = view->content_width > 0 ? view->content_width : 640;
	int place_h = view->content_height > 0 ? view->content_height : 400;
	int frame_w, frame_h, content_x, content_y;
	cow_titlebar_frame_geometry(view, place_w, place_h, &frame_w,
	    &frame_h, &content_x, &content_y);
	enum cow_placement_mode mode = cow_config ? cow_config->placement :
						    COW_PLACEMENT_CASCADE;

	switch (mode) {
	case COW_PLACEMENT_UNDERMOUSE:
		view_place_undermouse(view, frame_w, frame_h, content_x,
		    content_y);
		break;
	case COW_PLACEMENT_CENTRE:
		view_place_centre(view, frame_w, frame_h, content_x,
		    content_y);
		break;
	case COW_PLACEMENT_EMPTY:
		view_place_empty(view, frame_w, frame_h, content_x, content_y, gap);
		break;
	case COW_PLACEMENT_CASCADE:
	default:
		view_place_cascade(view, content_x, content_y, gap);
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
 * Wayland) so River doesn't migrate them.  Account for the decoration
 * offset so the entire outer frame, not only the client content, is parked
 * below the outputs.  vx/vy stay correct.
 */
static void
view_apply_node_position(struct cow_view *view)
{
	int render_x = view->x;
	int render_y = view->y;
	if (view->output != NULL && !cow_view_visible(view, view->output)) {
		struct cow_box frame = cow_view_frame_box_at(view, 0, 0);
		render_x = 0;
		render_y = cow_outputs_max_bottom() + 1 - frame.y;
	}
	river_node_v1_set_position(view->node, render_x, render_y);

	/* Keep pane positions and off-screen parking computed above. */
	cow_stack_apply_pending(view);
}

/* Show or hide the view's surface based on visibility, and update
 * decoration / container z-order while it's visible. */
static void
view_apply_show_hide(struct cow_view *view)
{
	if (view->output == NULL || !cow_view_visible(view, view->output)) {
		cow_decoration_hide(view);
		river_window_v1_hide(view->river_window);
		return;
	}

	river_window_v1_show(view->river_window);

	/*
	 * Container z-ordering is owned by the global stack pass
	 * (stack.c).  Container members share a layer and receive a
	 * shared raise_seq when any member is raised or lowered. Hidden
	 * tabs stay parked off-screen; each visible pane keeps its position.
	 */

	if (view->use_ssd) {
		bool active = (view == cow_wm.focused_view);
		struct cow_container *frame  = cow_container_window_owner(view);
		struct cow_view	     *focused =
		    cow_container_ancestor(cow_wm.focused_view, NULL);
		if (frame && focused && cow_view_container(focused) == frame)
			active = true;
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

	if (view->node == NULL) {
		view->node = river_window_v1_get_node(view->river_window);
		/* New nodes need ordering even when mapping does not raise them,
		 * notably container backing windows beneath their panes.
		 */
		cow_stack_mark_dirty();
	}

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

static void
view_reassign_output(struct cow_view *view, struct cow_output *output)
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
	cow_style_apply_for_event(view, false);
	cow_update_request(COW_UPDATE_STATUS);
}

void
cow_view_reassign_output(struct cow_view *view, struct cow_output *output)
{
	struct cow_container *c = cow_container_top_group(view);
	if (c == NULL) {
		view_reassign_output(view, output);
		return;
	}
	if (output == NULL)
		return;
	c->output = output;
	c->desk	  = output->current_desk;
	struct cow_view *member = NULL;
	while ((member = cow_container_next_view(c, member)) != NULL) {
		view_reassign_output(member, output);
		cow_view_sync_vxy_from_global(member);
	}
}

void
cow_view_update_output(struct cow_view *view)
{
	if (view->output == NULL)
		return;

	struct cow_output *target;
	if (cow_container_is_split(view)) {
		struct cow_box frame = cow_container_frame_at(view, view->x,
		    view->y);
		target = cow_output_for_box(&frame, view->output);
	} else {
		struct cow_box frame = cow_view_frame_box(view);
		target = cow_output_at(frame.x + frame.width / 2,
		    frame.y + frame.height / 2);
	}
	if (target != NULL && target != view->output) {
		cow_view_reassign_output(view, target);
		cow_view_clear_orphan(view);
	}
}

void
cow_view_set_pending_geometry(struct cow_view *view, int x, int y, int w, int h)
{
	cow_view_set_pending_geometry_anchored(view, x, y, w, h, false,
	    false);
}

void
cow_view_set_pending_geometry_anchored(struct cow_view *view, int x, int y,
    int w, int h, bool anchor_right, bool anchor_bottom)
{
	if (view->sd != COW_SHADE_NONE) {
		view->shade_saved_width = w;
		view->shade_saved_height = h;
	}
	view->pending_resize = true;
	view->pending_x	     = x;
	view->pending_y	     = y;
	view->pending_width  = w;
	view->pending_height = h;
	view->deferred_anchor_right = anchor_right;
	view->deferred_anchor_bottom = anchor_bottom;
	view->deferred_anchor_right_x = view->x + view->content_width;
	view->deferred_anchor_bottom_y = view->y + view->content_height;
	view->dec.dirty	     = true;
	cow_update_request(COW_UPDATE_MANAGE);
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

static int
view_floor_div(int n, int d)
{
	if (d <= 0)
		return 0;
	if (n >= 0)
		return n / d;
	return -((-n + d - 1) / d);
}

int
cow_view_page_col(const struct cow_view *view)
{
	if (view == NULL || view->output == NULL || cow_config == NULL ||
	    cow_config->page_cols <= 1)
		return 0;

	return view_floor_div(view->vx, cow_page_width(view->output));
}

int
cow_view_page_row(const struct cow_view *view)
{
	if (view == NULL || view->output == NULL || cow_config == NULL ||
	    cow_config->page_rows <= 1)
		return 0;

	return view_floor_div(view->vy, cow_page_height(view->output));
}

bool
cow_view_current_desk(const struct cow_view *view)
{
	if (view == NULL || view->output == NULL || view->desk == NULL ||
	    view->output->current_desk == NULL)
		return false;
	return view->desk == view->output->current_desk ||
	    (view->flags & COW_VIEW_STICKY_DESK_FLAG) != 0;
}

bool
cow_view_current_output(const struct cow_view *view)
{
	return view != NULL && view->output != NULL &&
	    view->output == cow_wm.current_output;
}

bool
cow_view_current_page(const struct cow_view *view)
{
	if (view == NULL || view->output == NULL || view->desk == NULL ||
	    view->output->current_desk == NULL)
		return false;

	if (!cow_view_current_desk(view))
		return false;
	if (view->flags & COW_VIEW_STICKY_PAGE_FLAG)
		return true;
	if (cow_config == NULL ||
	    (cow_config->page_cols <= 1 && cow_config->page_rows <= 1))
		return true;

	int w = cow_page_width(view->output);
	int h = cow_page_height(view->output);
	if (w <= 0 || h <= 0)
		return true;

	struct cow_desk *desk = view->output->current_desk;
	return cow_view_page_col(view) == view_floor_div(desk->page_vx, w) &&
	    cow_view_page_row(view) == view_floor_div(desk->page_vy, h);
}

void
cow_view_set_page_sticky(struct cow_view *view, bool sticky)
{
	if (view == NULL)
		return;

	bool currently = (view->flags & COW_VIEW_STICKY_PAGE_FLAG) != 0;
	if (currently == sticky)
		return;

	/*
	 * A page-sticky window tracks subsequent viewport movement.  Anchor it
	 * to the current page first so an off-page target does not retain an
	 * off-page virtual position while bypassing page visibility checks.
	 */
	if (sticky && view->output != NULL &&
	    view->output->current_desk != NULL) {
		cow_view_move_to_page(view, cow_page_col(view->output),
		    cow_page_row(view->output));
	}

	if (sticky)
		view->flags |= COW_VIEW_STICKY_PAGE_FLAG;
	else
		view->flags &= ~COW_VIEW_STICKY_PAGE_FLAG;
	cow_container_sync_flags(view, COW_VIEW_STICKY_PAGE_FLAG);
	view->dec.dirty = true;
	cow_update_request(COW_UPDATE_MANAGE | COW_UPDATE_STATUS);
}

bool
cow_view_circulatable(const struct cow_view *v)
{
	return cow_view_focusable(v) &&
	    !(v->flags & COW_VIEW_CIRCULATE_SKIP_FLAG);
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
	struct cow_container *c		  = cow_container_top_group(view);
	if (c != NULL) {
		struct cow_view *member = NULL;
		while ((member = cow_container_next_view(c, member)) != NULL) {
			if (member->layer == new_layer)
				continue;
			uint8_t prev   = member->layer;
			member->layer  = new_layer;
			any_changed    = true;

			char payload[64];

			/* FIXME: we can/should use cJSOJ here. */
			snprintf(payload, sizeof payload,
			    "{\"prev\":%u,\"current\":%u}", prev, new_layer);
			cow_rule_fire_json(COW_RULE_ON_LAYER_CHANGE, member,
			    member->desk, member->output, payload);
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
		cow_update_request(COW_UPDATE_MANAGE | COW_UPDATE_STATUS);
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
	if (!max && cow_container_is_split(view)) {
		struct cow_container *c = cow_view_container(view);
		c->maximized_h = c->maximized_v = false;
		struct cow_container_member *m;
		wl_list_for_each(m, &c->members, link)
		{
			struct cow_view *v = m->view;
			v->maximized = v->maximized_h = v->maximized_v = false;
			v->maximized_h_keep_geometry =
			    v->maximized_v_keep_geometry = false;
			v->maximized_h_ignore_reserved =
			    v->maximized_v_ignore_reserved = false;
			v->icon_inverted_button		   = -1;
			v->dec.dirty			   = true;
			cow_view_set_pending_maximized_hint(v, false);
		}
	}
	bool changed = (view->maximized != max);
	int maximize_button = max ? cow_view_maximize_button_index() : -1;

	/* Keep the per-axis flags in step with the unified one for
	 * call sites that don't model an axis -- drag-to-unmax,
	 * fullscreen toggle, state restore on session reload, etc.
	 * The cmd-window per-axis paths set maximized_h / maximized_v
	 * directly and then derive maximized from their OR. */
	view->maximized	  = max;
	view->maximized_h = max;
	view->maximized_v = max;
	view->maximized_h_ignore_reserved = false;
	view->maximized_v_ignore_reserved = false;
	view->maximized_h_keep_geometry = false;
	view->maximized_v_keep_geometry = false;
	if (view->icon_inverted_button != maximize_button) {
		view->icon_inverted_button = maximize_button;
		view->dec.dirty = true;
	}

	/*
	 * Notify status clients of the flip so per-window UI (the
	 * pager's maximized hint, status bars, etc.) update without
	 * having to wait for an unrelated status event.  The
	 * dimensions handler will request another update once the client
	 * acknowledges the resize; requests in one loop turn are coalesced.
	 */
	if (changed)
		cow_update_request(COW_UPDATE_STATUS);
}

void
cow_view_set_pending_maximized_hint(struct cow_view *view, bool maximized)
{
	if (view == NULL)
		return;

	view->pending_maximized_hint = true;
	view->pending_maximized_hint_value = maximized;
	cow_update_request(COW_UPDATE_MANAGE);
}

void
cow_view_inform_fullscreen_state(struct cow_view *view, bool fullscreen)
{
	if (view == NULL)
		return;

	view->pending_fullscreen_state = false;
	if (view->river_window != NULL &&
	    cow_wm.sequence == COW_WM_SEQUENCE_MANAGE) {
		if (fullscreen)
			river_window_v1_inform_fullscreen(view->river_window);
		else
			river_window_v1_inform_not_fullscreen(view->river_window);
		return;
	}

	view->pending_fullscreen_state = true;
	view->pending_fullscreen	     = fullscreen;
	cow_update_request(COW_UPDATE_MANAGE);
}

void
cow_view_set_tiled_edges(struct cow_view *view, uint32_t edges)
{
	if (view == NULL)
		return;

	if (view->tiled_edges == edges && !view->pending_tiled_edges)
		return;

	view->pending_tiled_edges	= true;
	view->pending_tiled_edges_value	= edges;
	cow_update_request(COW_UPDATE_MANAGE);
}

void
cow_view_clear_expanded_state(struct cow_view *view)
{
	if (view == NULL)
		return;
	if (cow_container_is_split(view)) {
		view->expanded = false;
		return;
	}
	if (!view->expanded &&
	    view->tiled_edges == RIVER_WINDOW_V1_EDGES_NONE &&
	    (!view->pending_tiled_edges ||
		view->pending_tiled_edges_value == RIVER_WINDOW_V1_EDGES_NONE))
		return;

	view->expanded = false;
	cow_view_set_tiled_edges(view, RIVER_WINDOW_V1_EDGES_NONE);
	if (!view->maximized)
		view->icon_inverted_button = -1;
	view->dec.dirty = true;
}

void
cow_view_save_expanded_geometry(struct cow_view *view)
{
	view->expanded_saved_x      = view->vx;
	view->expanded_saved_y      = view->vy;
	view->expanded_saved_width  = view->content_width;
	view->expanded_saved_height = view->content_height;
}

void
cow_view_restore_expanded_geometry(struct cow_view *view)
{
	/*
	 * expanded_saved_x/y are in virtual (vx/vy) coordinates.  Convert to
	 * global for the pending resize; it will re-sync vx/vy when applied.
	 */
	int gx = view->expanded_saved_x;
	int gy = view->expanded_saved_y;
	if (view->output != NULL && view->desk != NULL) {
		gx = view->output->x +
		    (view->expanded_saved_x - view->desk->page_vx);
		gy = view->output->y +
		    (view->expanded_saved_y - view->desk->page_vy);
	}

	cow_view_set_pending_geometry(view, gx, gy,
	    view->expanded_saved_width, view->expanded_saved_height);
	cow_view_clear_expanded_state(view);
}

struct cow_box
cow_view_frame_box(const struct cow_view *v)
{
	return cow_view_frame_box_at(v, v->x, v->y);
}

struct cow_box
cow_view_frame_box_at(const struct cow_view *v, int x, int y)
{
	int frame_w, frame_h, content_x, content_y;
	int cw, ch;
	cow_view_effective_content_size(v, &cw, &ch);
	cow_titlebar_frame_geometry(v, cw, ch, &frame_w, &frame_h,
	    &content_x, &content_y);
	return (struct cow_box) {
		.x	= x - content_x,
		.y	= y - content_y,
		.width	= frame_w,
		.height = frame_h,
	};
}

static void
view_maximized_areas(const struct cow_view *view, struct cow_box *ha,
    struct cow_box *va)
{
	struct cow_box full = {
	    .x = view->output->x,
	    .y = view->output->y,
	    .width = view->output->width,
	    .height = view->output->height,
	};
	*ha = view->maximized_h_ignore_reserved ? full : view->output->usable_area;
	*va = view->maximized_v_ignore_reserved ? full : view->output->usable_area;
	/* Work areas describe the current viewport. Both refitting and reflow
	 * must use the window's own page, including after state restoration.
	 */
	if (view->desk != NULL && !(view->flags & COW_VIEW_STICKY_PAGE_FLAG)) {
		int dx = cow_view_page_col(view) * cow_page_width(view->output) -
		    view->desk->page_vx;
		int dy = cow_view_page_row(view) * cow_page_height(view->output) -
		    view->desk->page_vy;
		ha->x += dx;
		ha->y += dy;
		va->x += dx;
		va->y += dy;
	}
}

static void
view_refit_maximized(struct cow_view *view)
{
	if (view == NULL || !view->maximized || view->output == NULL)
		return;
	if (cow_container_is_split(view))
		return;

	struct cow_box ha, va;
	view_maximized_areas(view, &ha, &va);
	int frame_w, frame_h, content_x, content_y;
	cow_titlebar_frame_geometry(view, view->content_width,
	    view->content_height, &frame_w, &frame_h, &content_x, &content_y);

	int new_x = view->x;
	int new_y = view->y;
	if (view->maximized_h && !view->maximized_h_keep_geometry)
		new_x = ha.x + content_x;
	if (view->maximized_v && !view->maximized_v_keep_geometry)
		new_y = va.y + content_y;

	if (view->maximized_h && !view->maximized_h_keep_geometry &&
	    frame_w <= ha.width) {
		int min_x = ha.x + content_x;
		int max_x = ha.x + ha.width - frame_w + content_x;
		if (new_x < min_x)
			new_x = min_x;
		else if (new_x > max_x)
			new_x = max_x;
	}
	if (view->maximized_v && !view->maximized_v_keep_geometry &&
	    frame_h <= va.height) {
		int min_y = va.y + content_y;
		int max_y = va.y + va.height - frame_h + content_y;
		if (new_y < min_y)
			new_y = min_y;
		else if (new_y > max_y)
			new_y = max_y;
	}

	if (new_x == view->x && new_y == view->y)
		return;

	view->x = new_x;
	view->y = new_y;
	cow_view_sync_vxy_from_global(view);
	cow_view_sync_container_pos(view);
	view->dec.dirty = true;
	cow_update_request(COW_UPDATE_MANAGE | COW_UPDATE_STATUS);
}

/* ================================================================
 * View helper functions (public API used by wm.c, cmd-*.c)
 * ================================================================ */

int
cow_view_maximize_button_index(void)
{
	for (int i = 0; i < COW_TITLEBAR_NUM_BUTTONS; i++) {
		for (int clicks = 1; clicks <= 2; clicks++) {
			const char *action = cow_dec_binding_lookup(
			    cow_dec_region_button(i), clicks);
			if (cow_dec_binding_can_maximize(action))
				return i;
		}
	}
	return -1;
}

void
cow_view_maximize(struct cow_view *view)
{
	struct cow_container *c = cow_view_container(view);
	if (c && c->frame_view) {
		cow_view_maximize(c->frame_view);
		return;
	}
	if (cow_container_is_split(view)) {
		cow_container_maximize(view, true, true, false, false, false);
		return;
	}
	if (view->output == NULL)
		return;

	cow_view_clear_expanded_state(view);

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
	int frame_w, frame_h, content_x, content_y;
	cow_titlebar_frame_geometry(view, view->content_width,
	    view->content_height, &frame_w, &frame_h, &content_x, &content_y);
	int frame_extra_w = frame_w - view->content_width;
	int frame_extra_h = frame_h - view->content_height;

	view->x = ua->x + content_x;
	view->y = ua->y + content_y;
	cow_view_sync_vxy_from_global(view);
	int w = ua->width - frame_extra_w;
	int h = ua->height - frame_extra_h;
	cow_clamp_dimensions(&w, &h);
	cow_view_set_pending_geometry(view, view->x, view->y, w, h);
	cow_view_set_pending_maximized_hint(view, true);
	cow_view_set_maximized(view, true);

	/*
	 * Sync the container with the maximized geometry.  Use the proposed
	 * w/h directly because the dimensions event hasn't arrived yet and
	 * view->content_width still holds the old value.
	 */
	cow_view_sync_container_geometry(view, view->x, view->y, w, h);

}

void
cow_view_reflow_maximized(struct cow_view *view)
{
	if (view != NULL && cow_container_is_split(view)) {
		cow_container_reflow(view);
		return;
	}
	if (view == NULL || !view->maximized || view->fullscreen ||
	    view->output == NULL)
		return;

	struct cow_box ha, va;
	view_maximized_areas(view, &ha, &va);
	int frame_w, frame_h, content_x, content_y;
	cow_titlebar_frame_geometry(view, view->content_width,
	    view->content_height, &frame_w, &frame_h, &content_x, &content_y);

	int x = view->x;
	int y = view->y;
	int w = view->content_width;
	int h = view->content_height;
	if (view->maximized_h && !view->maximized_h_keep_geometry) {
		x = ha.x + content_x;
		w = ha.width - (frame_w - view->content_width);
	}
	if (view->maximized_v && !view->maximized_v_keep_geometry) {
		y = va.y + content_y;
		h = va.height - (frame_h - view->content_height);
	}
	cow_clamp_dimensions(&w, &h);

	if (x == view->x && y == view->y && w == view->content_width &&
	    h == view->content_height)
		return;

	cow_view_set_pending_geometry(view, x, y, w, h);
	if (view->container_member != NULL)
		cow_view_sync_container_geometry(view, x, y, w, h);
}

void
cow_view_unmaximize(struct cow_view *view)
{
	struct cow_container *c = cow_view_container(view);
	if (c && c->frame_view) {
		cow_view_unmaximize(c->frame_view);
		return;
	}
	if (cow_container_is_split(view)) {
		cow_container_maximize(view, true, true, true, false, false);
		return;
	}
	cow_view_clear_expanded_state(view);

	/* saved_x/y hold virtual (vx/vy) coordinates saved by
	 * cow_view_maximize.  If maximize fired before the window was
	 * placed (e.g. Chrome's startup maximize_requested), saved_x/y
	 * are zero and must not be restored — clear placed so normal
	 * placement runs on the next render instead. */
	if (!view->placed) {
		/* Placement hasn't run yet; just drop the maximized state
		 * and let cow_view_apply_render place the window normally. */
		cow_view_set_maximized(view, false);
		cow_view_set_pending_geometry(view, view->x, view->y,
		    view->saved_width, view->saved_height);
		cow_view_set_pending_maximized_hint(view, false);
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
	cow_view_set_pending_geometry(view, view->x, view->y,
	    view->saved_width, view->saved_height);
	cow_view_set_pending_maximized_hint(view, false);
	cow_view_set_maximized(view, false);

	/* Restore container geometry to the saved (pre-maximize) values.
	 * Use view->x/y (already recomputed from vx/vy) as the global coord. */
	cow_view_sync_container_geometry(view, view->x, view->y,
	    view->saved_width, view->saved_height);

	view->dec.dirty = true;
}

void
cow_view_shaded_content_size(const struct cow_view *view, int direction,
    int *cw, int *ch)
{
	int sw = view->shade_saved_width;
	int sh = view->shade_saved_height;

	switch (direction) {
	case COW_SHADE_EAST:
	case COW_SHADE_WEST:
		*cw = 0;
		*ch = sh;
		break;
	case COW_SHADE_NW:
	case COW_SHADE_NE:
	case COW_SHADE_SW:
	case COW_SHADE_SE:
		*cw = 0;
		*ch = 0;
		break;
	case COW_SHADE_NORTH:
	case COW_SHADE_SOUTH:
	default:
		*cw = sw;
		*ch = 0;
		break;
	}

	if (!shade_uses_titlebar_axis(view, direction))
		return;

	struct cow_box tbr = cow_titlebar_squeezed_rect_for(view, sw, sh);
	if (cow_titlebar_is_vertical(view->cfg.titlebar.position)) {
		if (tbr.height > 0)
			*ch = tbr.height;
	} else if (tbr.width > 0) {
		*cw = tbr.width;
	}
}

void
cow_view_effective_content_size(const struct cow_view *view, int *width,
    int *height)
{
	*width = view->content_width;
	*height = view->content_height;
	if (cow_container_shaded_size(view, width, height))
		return;
	if (view->sd != COW_SHADE_NONE)
		cow_view_shaded_content_size(view, view->sd, width, height);
	struct cow_container_pane *pane = cow_view_pane(view);
	if (pane != NULL && !view->fullscreen) {
		if (pane->layout.collapsed == COW_CONTAINER_HORIZONTAL)
			*width = 0;
		if (pane->layout.collapsed == COW_CONTAINER_VERTICAL)
			*height = 0;
	}
}

static struct cow_box
view_frame_box_for_size(const struct cow_view *view, int x, int y, int cw,
    int ch)
{
	int frame_w, frame_h, content_x, content_y;

	cow_titlebar_frame_geometry(view, cw, ch, &frame_w, &frame_h,
	    &content_x, &content_y);
	return (struct cow_box) {
		.x	= x - content_x,
		.y	= y - content_y,
		.width	= frame_w,
		.height = frame_h,
	};
}

static void
view_place_for_shade_edge(struct cow_view *view, int direction,
    const struct cow_box *old_fb, int new_cw, int new_ch)
{
	int frame_w, frame_h, content_x, content_y;

	cow_titlebar_frame_geometry(view, new_cw, new_ch, &frame_w, &frame_h,
	    &content_x, &content_y);

	switch (direction) {
	case COW_SHADE_SOUTH:
	case COW_SHADE_SE:
	case COW_SHADE_SW:
		view->y = old_fb->y + old_fb->height - frame_h + content_y;
		break;
	case COW_SHADE_NORTH:
	case COW_SHADE_NE:
	case COW_SHADE_NW:
	default:
		view->y = old_fb->y + content_y;
		break;
	}

	switch (direction) {
	case COW_SHADE_EAST:
	case COW_SHADE_NE:
	case COW_SHADE_SE:
		view->x = old_fb->x + old_fb->width - frame_w + content_x;
		break;
	case COW_SHADE_WEST:
	case COW_SHADE_NW:
	case COW_SHADE_SW:
	default:
		view->x = old_fb->x + content_x;
		break;
	}

	if (shade_uses_titlebar_axis(view, direction)) {
		int old_cw, old_ch;

		if (view->sd != COW_SHADE_NONE) {
			old_cw = view->shade_saved_width;
			old_ch = view->shade_saved_height;
		} else {
			cow_view_shaded_content_size(view, direction, &old_cw,
			    &old_ch);
		}

		struct cow_box old_tbr =
		    cow_titlebar_squeezed_rect_for(view, old_cw, old_ch);
		struct cow_box new_tbr =
		    cow_titlebar_squeezed_rect_for(view, new_cw, new_ch);
		int new_frame_x = view->x - content_x;
		int new_frame_y = view->y - content_y;

		if (cow_titlebar_is_vertical(view->cfg.titlebar.position)) {
			int old_title_y = old_fb->y + old_tbr.y;
			int new_title_y = new_frame_y + new_tbr.y;
			view->y += old_title_y - new_title_y;
		} else {
			int old_title_x = old_fb->x + old_tbr.x;
			int new_title_x = new_frame_x + new_tbr.x;
			view->x += old_title_x - new_title_x;
		}
	}
	cow_view_sync_vxy_from_global(view);
}

void
cow_view_shade(struct cow_view *view, int direction)
{
	struct cow_container *c = cow_view_container(view);
	if (c && c->frame_view) {
		cow_view_shade(c->frame_view, direction);
		return;
	}
	if (cow_container_is_split(view)) {
		if (cow_view_container(view)->shaded) {
			cow_view_unshade(view);
			return;
		}
		if (cow_container_shade(view, true))
			cow_rule_fire(COW_RULE_ON_SHADE, view, view->desk,
			    view->output);
		return;
	}
	if (view->sd != COW_SHADE_NONE) {
		cow_view_unshade(view);
		return;
	}

	if (direction == 0)
		direction = COW_SHADE_NORTH;

	bool had_focus = (cow_wm.focused_view == view);
	struct cow_box old_fb = cow_view_frame_box(view);

	view->shade_saved_width	 = view->content_width;
	view->shade_saved_height = view->content_height;
	view->sd		 = direction;

	int shaded_cw, shaded_ch;
	cow_view_shaded_content_size(view, direction, &shaded_cw, &shaded_ch);
	view_place_for_shade_edge(view, direction, &old_fb, shaded_cw,
	    shaded_ch);

	/* Clip the window content to 1x1 pixel so it's effectively
	 * invisible.  0x0 disables clipping per the protocol.
	 * Applied during the render callback via pending_clip. */
	view->pending_clip = true;
	view->clip_x	   = 0;
	view->clip_y	   = 0;
	view->clip_w	   = 1;
	view->clip_h	   = 1;

	view->dec.dirty = true;
	if (had_focus)
		view->needs_focus = true;
	cow_update_request(COW_UPDATE_MANAGE | COW_UPDATE_STATUS);

	cow_rule_fire(COW_RULE_ON_SHADE, view, view->desk, view->output);
}

void
cow_view_unshade(struct cow_view *view)
{
	struct cow_container *c = cow_view_container(view);
	if (c && c->frame_view) {
		cow_view_unshade(c->frame_view);
		return;
	}
	if (cow_container_is_split(view) && cow_view_container(view)->shaded) {
		if (cow_container_shade(view, false))
			cow_rule_fire(COW_RULE_ON_UNSHADE, view, view->desk,
			    view->output);
		return;
	}
	if (view->sd == COW_SHADE_NONE)
		return;

	bool had_focus = (cow_wm.focused_view == view);
	int sw = view->shade_saved_width;
	int sh = view->shade_saved_height;
	int direction = view->sd;
	int shaded_cw, shaded_ch;
	cow_view_shaded_content_size(view, direction, &shaded_cw, &shaded_ch);
	struct cow_box old_fb = view_frame_box_for_size(view, view->x,
	    view->y, shaded_cw, shaded_ch);

	view->sd = COW_SHADE_NONE;

	view->content_width  = sw;
	view->content_height = sh;
	view_place_for_shade_edge(view, direction, &old_fb, sw, sh);

	/* Disable clipping (0 width/height = disabled per protocol). */
	view->pending_clip = true;
	view->clip_x	   = 0;
	view->clip_y	   = 0;
	view->clip_w	   = 0;
	view->clip_h	   = 0;

	view->dec.dirty		      = true;
	view->dec.last_content_width  = -1;
	view->dec.last_content_height = -1;
	if (had_focus)
		view->needs_focus = true;
	if (view->dec.buffer != NULL) {
		cow_shm_buffer_destroy(view->dec.buffer);
		view->dec.buffer = NULL;
	}

	cow_update_request(COW_UPDATE_MANAGE | COW_UPDATE_STATUS);

	cow_rule_fire(COW_RULE_ON_UNSHADE, view, view->desk, view->output);
}

void
cow_view_fullscreen(struct cow_view *view)
{
	if (view->output == NULL)
		return;
	cow_container_shade(view, false);
	struct cow_container *c = cow_view_container(view);
	if (c != NULL) {
		struct cow_container_member *m;
		wl_list_for_each(m, &c->members, link)
		{
			if (m->view != view && m->view->fullscreen)
				cow_view_exit_fullscreen(m->view);
		}
		cow_container_activate(c, view->container_member);
	}
	bool was_fullscreen = view->fullscreen;

	cow_view_clear_expanded_state(view);

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
	view->vx = view->desk != NULL ? view->desk->page_vx : 0;
	view->vy = view->desk != NULL ? view->desk->page_vy : 0;
	cow_view_set_pending_geometry(view, view->x, view->y,
	    view->output->width, view->output->height);
	cow_view_inform_fullscreen_state(view, true);
	view->fullscreen = true;
	view->use_ssd	 = false;
	view->pending_clip = true;
	view->clip_x = view->clip_y = view->clip_w = view->clip_h = 0;
	cow_update_request(COW_UPDATE_STATUS);
	if (!was_fullscreen)
		cow_rule_fire(COW_RULE_ON_FULLSCREEN, view, view->desk,
		    view->output);
}

void
cow_view_exit_fullscreen(struct cow_view *view)
{
	bool was_fullscreen = view->fullscreen;
	if (cow_container_is_split(view)) {
		view->fullscreen = false;
		cow_view_inform_fullscreen_state(view, false);
		struct cow_container *c = cow_view_container(view);
		cow_container_arrange(c, c->frame);
		if (was_fullscreen)
			cow_rule_fire(COW_RULE_ON_UNFULLSCREEN, view,
			    view->desk, view->output);
		return;
	}

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
	cow_view_set_pending_geometry(view, view->x, view->y,
	    view->saved_width, view->saved_height);
	cow_view_inform_fullscreen_state(view, false);
	view->fullscreen = false;
	view->use_ssd	 = true;
	cow_update_request(COW_UPDATE_STATUS);
	if (was_fullscreen)
		cow_rule_fire(COW_RULE_ON_UNFULLSCREEN, view, view->desk,
		    view->output);
}

/* ================================================================
 * Iconify (minimize) support
 * ================================================================ */

static void
icon_occupied_add(struct cow_icon_rect **occupied, size_t *count, int x, int y,
    int width, int height)
{
	*occupied = xrealloc(*occupied, *count + 1, sizeof(**occupied));
	(*occupied)[*count] = (struct cow_icon_rect) { x, y, width, height };
	(*count)++;
}

static bool
icon_position_available(const struct cow_box *area,
    const struct cow_icon_rect *occupied, size_t occupied_count, int x, int y,
    int width, int height)
{
	if (x < area->x || y < area->y ||
	    (int64_t)x + width > (int64_t)area->x + area->width ||
	    (int64_t)y + height > (int64_t)area->y + area->height)
		return false;
	for (size_t i = 0; i < occupied_count; i++) {
		const struct cow_icon_rect *o = &occupied[i];
		if ((int64_t)x < (int64_t)o->x + o->width &&
		    (int64_t)x + width > o->x &&
		    (int64_t)y < (int64_t)o->y + o->height &&
		    (int64_t)y + height > o->y)
			return false;
	}
	return true;
}

void
cow_view_invalidate_auto_icon_positions(struct cow_output *output)
{
	struct cow_view *view;
	wl_list_for_each(view, &cow_wm.views, wm_link) {
		if (view->output == output && !view->icon_manual_position)
			view->icon_auto_position_valid = false;
	}
}

bool
cow_view_has_icon(const struct cow_view *view)
{
	struct cow_container *c = cow_view_container(view);
	return !view->closed && cow_view_is_iconified(view) &&
	    view->cfg.icon_enabled && (c == NULL || c->frame_view == NULL) &&
	    (c == NULL || (c->active != NULL && c->active->view == view));
}

/* Compute collision-free automatic icon positions on an output. */
void
cow_view_layout_icons_on_output(struct cow_output *output)
{
	struct cow_icon_placement placement;
	if (cow_config != NULL)
		cow_icon_placement_resolve(cow_config, output, &placement);
	else
		cow_icon_placement_default(&placement);
	struct cow_box box;
	if (!cow_icon_box_resolve(&placement.box, &output->usable_area, &box))
		box = output->usable_area;
	struct cow_icon_rect *occupied = NULL;
	size_t occupied_count = 0;

	struct cow_view *view;
	/* Size every icon first and reserve manually positioned icons. */
	wl_list_for_each(view, &cow_wm.views, wm_link)
	{
		if (!cow_view_has_icon(view))
			continue;
		if (view->output != output)
			continue;

		int icon_w = view->cfg.icon_size;
		int icon_h = icon_w + view->cfg.icon_label_area;
		view->icon_width = icon_w;
		view->icon_height = icon_h;
		if (view->icon_manual_position)
			icon_occupied_add(&occupied, &occupied_count,
			    view->icon_x, view->icon_y, icon_w, icon_h);
	}
	if (!placement.compact) {
		wl_list_for_each(view, &cow_wm.views, wm_link) {
			if (!cow_view_has_icon(view) ||
			    view->output != output ||
			    view->icon_manual_position ||
			    !view->icon_auto_position_valid)
				continue;
			int icon_w = view->cfg.icon_size;
			int icon_h = icon_w + view->cfg.icon_label_area;
			if (!icon_position_available(&output->usable_area, occupied,
				occupied_count, view->icon_x, view->icon_y, icon_w,
				icon_h)) {
				view->icon_auto_position_valid = false;
				continue;
			}
			icon_occupied_add(&occupied, &occupied_count, view->icon_x,
			    view->icon_y, icon_w, icon_h);
		}
	}

	wl_list_for_each(view, &cow_wm.views, wm_link)
	{
		if (!cow_view_has_icon(view) || view->output != output ||
		    view->icon_manual_position ||
		    (!placement.compact && view->icon_auto_position_valid))
			continue;
		int icon_w = view->cfg.icon_size;
		int icon_h = icon_w + view->cfg.icon_label_area;
		int x, y;
		bool found = cow_icon_placement_find(&placement, &box, icon_w, icon_h,
		    occupied, occupied_count, &x, &y);
		if (!found && (box.x != output->usable_area.x ||
		    box.y != output->usable_area.y ||
		    box.width != output->usable_area.width ||
		    box.height != output->usable_area.height))
			found = cow_icon_placement_find(&placement, &output->usable_area,
			    icon_w, icon_h, occupied, occupied_count, &x, &y);
		if (!found) {
			x = placement.primary == COW_ICON_DIR_LEFT ||
			    placement.secondary == COW_ICON_DIR_LEFT ?
			    output->usable_area.x + output->usable_area.width - icon_w :
			    output->usable_area.x;
			y = placement.primary == COW_ICON_DIR_UP ||
			    placement.secondary == COW_ICON_DIR_UP ?
			    output->usable_area.y + output->usable_area.height - icon_h :
			    output->usable_area.y;
			cow_log_warn("icon placement exhausted (output=%s)",
			    output->name ? output->name : "unknown");
		}
		view->icon_x = x;
		view->icon_y = y;
		view->icon_auto_position_valid = true;
		icon_occupied_add(&occupied, &occupied_count, x, y, icon_w,
		    icon_h);
	}
	xfree(occupied);
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
	if (view->icon_node == NULL) {
		view->icon_node = river_shell_surface_v1_get_node(
		    view->icon_shell);
		if (view->icon_node != NULL)
			cow_stack_mark_dirty();
	}
	return true;
}

/* Allocate (or resize) the icon's SHM buffer.  Returns false on
 * allocation failure. */
static bool
icon_ensure_buffer(struct cow_view *view, int icon_w, int icon_h)
{
	if (view->icon_buffer != NULL &&
	    (view->icon_buffer->busy || view->icon_buffer->width != icon_w ||
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
	if (!cow_image_render_rect(path, p, box_x + pad, pad, size, size) &&
	    strcmp(path, COW_DATADIR "/icons/default.png") != 0)
		(void)cow_image_render_rect(COW_DATADIR "/icons/default.png", p,
		    box_x + pad, pad, size, size);
}

const char *
cow_view_icon_image(const struct cow_view *view)
{
	if (view == NULL || view->cfg.icon_image == NULL)
		return NULL;
	if (strcmp(view->cfg.icon_image, "auto") != 0)
		return view->cfg.icon_image;
	if (view->application_icon != NULL)
		return view->application_icon;
	return COW_DATADIR "/icons/default.png";
}

/* Render the icon's app_id label into the strip below the icon box. */
static void
icon_draw_label(struct cow_pixbuf *p, int icon_sz, const char *label,
    const float *fg, const struct cow_colour_triple *bc_t,
    enum cow_border_style border_style, enum cow_relief relief)
{
	int text_y = relief == COW_RELIEF_FLAT ? icon_sz : icon_sz + 2;
	int text_area_h = p->height - text_y;
	if (text_area_h <= 0)
		return;

	if (relief != COW_RELIEF_NONE) {
		cow_draw_fill_rect(p, 0, text_y, p->width, text_area_h,
		    bc_t->base);
		if (relief != COW_RELIEF_FLAT &&
		    border_style != COW_BORDER_STYLE_NONE) {
			uint32_t px_tl = relief == COW_RELIEF_SUNKEN ?
			    bc_t->dk : bc_t->lt;
			uint32_t px_br = relief == COW_RELIEF_SUNKEN ?
			    bc_t->lt : bc_t->dk;
			int bw = 3;
			for (int r = 0; r < bw && r < text_area_h / 2; r++) {
				cow_draw_hline(p, r, p->width - 1 - r,
				    text_y + r, px_tl);
				cow_draw_vline(p, r, text_y + r,
				    text_y + text_area_h - 1 - r, px_tl);
				cow_draw_hline(p, r, p->width - 1 - r,
				    text_y + text_area_h - 1 - r, px_br);
				cow_draw_vline(p, p->width - 1 - r, text_y + r,
				    text_y + text_area_h - 1 - r, px_br);
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
	int inset = relief != COW_RELIEF_FLAT && relief != COW_RELIEF_NONE &&
	    border_style != COW_BORDER_STYLE_NONE ? 3 : 0;
	int avail_w = p->width - 2 * inset;
	int avail_h = text_area_h - 2 * inset;
	if (avail_w <= 0 || avail_h <= 0)
		goto out;
	pango_layout_set_width(layout, avail_w * PANGO_SCALE);
	pango_layout_set_alignment(layout, PANGO_ALIGN_CENTER);
	pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
	PangoRectangle ink;
	pango_layout_get_pixel_extents(layout, &ink, NULL);
	int text_x = inset + (avail_w - ink.width) / 2 - ink.x;
	int text_y_offset = inset + (avail_h - ink.height) / 2 - ink.y;

	cairo_move_to(cr, text_x, text_y_offset);
	cairo_set_source_rgba(cr, fg[0], fg[1], fg[2], fg[3]);
	pango_cairo_show_layout(cr, layout);

out:
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
	view->icon_buffer->busy = true;
	wl_surface_attach(view->icon_surface, view->icon_buffer->buffer, 0, 0);
	wl_surface_damage_buffer(view->icon_surface, 0, 0, icon_w, icon_h);
	river_shell_surface_v1_sync_next_commit(view->icon_shell);
	wl_surface_commit(view->icon_surface);

	river_node_v1_set_position(view->icon_node, view->icon_x, view->icon_y);
}

void
cow_view_icon_set_hovered(struct cow_view *view, bool hovered)
{
	if (view == NULL || view->icon_hovered == hovered)
		return;
	view->icon_hovered = hovered;
	cow_update_request(COW_UPDATE_MANAGE);
}

static void
icon_colours(const struct cow_view *view, const float **background,
    const float **foreground)
{
	const struct cow_view_cfg *cfg = &view->cfg;
	*background = cfg->icon_background.set ?
	    cfg->icon_background.rgba : cfg->titlebar.inactive_colour;
	*foreground = cfg->icon_foreground.set ?
	    cfg->icon_foreground.rgba : cfg->titlebar.fg_inactive;
	if (view->icon_hovered) {
		*background = cfg->icon_active_background.set ?
		    cfg->icon_active_background.rgba : cfg->titlebar.active_colour;
		*foreground = cfg->icon_active_foreground.set ?
		    cfg->icon_active_foreground.rgba : cfg->titlebar.fg_active;
	}
}

void
cow_view_icon_render(struct cow_view *view)
{
	if (!cow_view_has_icon(view)) {
		cow_view_icon_destroy(view);
		return;
	}
	if (view->output == NULL)
		return;

	int icon_w = view->icon_width;
	int icon_h = view->icon_height;
	if (icon_w <= 0 || icon_h <= 0)
		return;
	bool needs_draw = view->icon_surface == NULL ||
	    view->icon_hovered != view->icon_rendered_hovered ||
	    view->icon_shell == NULL || view->icon_node == NULL ||
	    view->icon_buffer == NULL ||
	    view->icon_buffer->width != icon_w ||
	    view->icon_buffer->height != icon_h;

	if (!icon_ensure_surface(view))
		return;
	if (!needs_draw) {
		river_node_v1_set_position(view->icon_node, view->icon_x,
		    view->icon_y);
		return;
	}
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
	const float *background, *foreground;
	icon_colours(view, &background, &foreground);
	cow_colour_make_triple(&bc_t, background);

	int icon_sz = view->cfg.icon_size;

	icon_draw_box(&p, icon_sz, bc_t.base, bc_t.lt, bc_t.dk,
	    view->cfg.border_style);
	icon_draw_image(&p, icon_sz, cow_view_icon_image(view));

	char *title = cow_view_make_display_title(view, view->cfg.icon_format);

	icon_draw_label(&p, icon_sz, title,
	    foreground, &bc_t, view->cfg.border_style,
	    view->cfg.icon_title_relief);

	xfree(title);

	if (view->flags & COW_VIEW_STICKY_FLAG)
		icon_draw_sticky_stipple(&p, bc_t.base, bc_t.lt, bc_t.dk);
	icon_commit(view, icon_w, icon_h);
	view->icon_rendered_hovered = view->icon_hovered;
}

void
cow_view_icon_refresh(struct cow_view *view)
{
	if (view == NULL || view->closed)
		return;
	cow_view_icon_destroy(view);
	if (view->output != NULL)
		cow_view_layout_icons_on_output(view->output);
}

static void
cow_view_icon_destroy(struct cow_view *view)
{
	view->icon_hovered = false;
	if (view->icon_buffer != NULL) {
		cow_shm_buffer_destroy(view->icon_buffer);
		view->icon_buffer = NULL;
	}
	if (view->icon_node != NULL) {
		river_node_v1_destroy(view->icon_node);
		view->icon_node = NULL;
		cow_stack_mark_dirty();
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

static void
view_set_iconified(struct cow_view *view, bool iconified)
{
	if (iconified) {
		view->flags |= COW_VIEW_ICONIFIED_FLAG;
	} else {
		view->flags &= ~COW_VIEW_ICONIFIED_FLAG;
		if (!view->icon_manual_position)
			view->icon_auto_position_valid = false;
	}
	cow_view_icon_destroy(view);
}

static void
view_group_set_iconified(struct cow_view *view, bool iconified)
{
	struct cow_container *c = cow_container_top_group(view);
	if (c == NULL) {
		view_set_iconified(view, iconified);
		return;
	}
	struct cow_view *member = NULL;
	while ((member = cow_container_next_view(c, member)) != NULL)
		view_set_iconified(member, iconified);
}

void
cow_view_iconify(struct cow_view *view)
{
	if (cow_view_is_iconified(view))
		return;

	/* Hiding does not change geometry or overwrite maximize restore state.
	 */
	view_group_set_iconified(view, true);

	/* Layout all icons on this output */
	if (view->output != NULL)
		cow_view_layout_icons_on_output(view->output);

	/* All panes are hidden before choosing a replacement focus. */
	struct cow_view *focused = cow_wm.focused_view;
	struct cow_view *pending = cow_wm.pending_focus;
	if ((focused != NULL && cow_view_is_iconified(focused)) ||
	    (pending != NULL && cow_view_is_iconified(pending))) {
		struct cow_view *next = cow_view_walk(view, true,
		    cow_view_focusable);
		if (next != NULL && next != view) {
			cow_wm.pending_focus = next;
		} else {
			struct cow_seat *seat;

			cow_wm.pending_focus = NULL;
			if (focused != NULL && cow_view_is_iconified(focused)) {
				cow_wm_set_focused_view(NULL);
				wl_list_for_each(seat, &cow_wm.seats, wm_link)
				{
					cow_seat_forget_view(seat, focused);
				}
			}
		}
	}

	cow_update_request(COW_UPDATE_MANAGE | COW_UPDATE_STATUS);

	cow_rule_fire(COW_RULE_ON_ICONIFY, view, view->desk, view->output);
}

void
cow_view_deiconify(struct cow_view *view)
{
	if (!cow_view_is_iconified(view))
		return;

	view_group_set_iconified(view, false);
	if (view->container_member != NULL)
		cow_container_activate(cow_view_container(view),
		    view->container_member);

	/* Re-layout remaining icons on this output */
	if (view->output != NULL)
		cow_view_layout_icons_on_output(view->output);

	/* Focus the restored window */
	view->needs_raise    = true;
	cow_wm.pending_focus = view;
	cow_update_request(COW_UPDATE_MANAGE | COW_UPDATE_STATUS);

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

bool
cow_view_is_iconified(const struct cow_view *view)
{
	return (view->flags & COW_VIEW_ICONIFIED_FLAG) != 0;
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
cow_view_warp_to(const struct cow_view *view)
{
	int cx, cy;

	if (view == NULL)
		return;

	cow_view_centre(view, &cx, &cy);
	cow_wm_request_pointer_warp(cx, cy);
	cow_update_request(COW_UPDATE_MANAGE);
}

void
cow_view_warp_to_focused(void)
{
	const struct cow_view *view = cow_wm.pending_focus != NULL ?
	    cow_wm.pending_focus : cow_wm.focused_view;

	cow_view_warp_to(view);
}

void
cow_view_centre(const struct cow_view *v, int *cx, int *cy)
{
	struct cow_box fb = cow_view_frame_box(v);

	*cx = fb.x + fb.width / 2;
	*cy = fb.y + fb.height / 2;
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
	cow_container_sync_frame(view);
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
	cow_view_clear_expanded_state(view);
	view->icon_inverted_button = -1;
}

void
cow_view_json_add_identity(struct cJSON *obj, const struct cow_view *view)
{
	cJSON_AddStringToObject(obj, "id",
	    view->identifier ? view->identifier : "");
	cJSON_AddStringToObject(obj, "app_id",
	    view->app_id ? view->app_id : "");
	const char *icon = cow_view_icon_image(view);
	if (icon != NULL)
		cJSON_AddStringToObject(obj, "icon", icon);
	cJSON_AddStringToObject(obj, "title", view->title ? view->title : "");
	char *display_title = cow_decoration_display_title(view);
	cJSON_AddStringToObject(obj, "display_title",
	    display_title != NULL ? display_title : "");
	xfree(display_title);
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
