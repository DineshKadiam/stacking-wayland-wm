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

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cow/config.h"
#include "cow/icon_placement.h"
#include "cow/memory.h"
#include "cow/output.h"

void
cow_icon_placement_default(struct cow_icon_placement *p)
{
	memset(p, 0, sizeof(*p));
	p->box.type = COW_ICON_BOX_DEFAULT;
	p->primary = COW_ICON_DIR_RIGHT;
	p->secondary = COW_ICON_DIR_UP;
	/* Zero means icon dimensions plus the traditional 8px gap. */
	p->grid_x = 0;
	p->grid_y = 0;
	p->compact = true;
}

static bool
parse_positive(const char **s, int *value)
{
	char *end;
	long n = strtol(*s, &end, 10);
	if (end == *s || n <= 0 || n > INT_MAX)
		return false;
	*value = (int)n;
	*s = end;
	return true;
}

static bool
parse_offset(const char **s, int *value, bool *far_edge)
{
	if (**s != '+' && **s != '-')
		return false;
	*far_edge = **s == '-';
	(*s)++;
	char *end;
	long n = strtol(*s, &end, 10);
	if (end == *s || n < 0 || n > INT_MAX)
		return false;
	*value = (int)n;
	*s = end;
	return true;
}

bool
cow_icon_box_parse(const char *value, struct cow_icon_box_config *box)
{
	if (strcmp(value, "full") == 0) {
		memset(box, 0, sizeof(*box));
		box->type = COW_ICON_BOX_FULL;
		return true;
	}
	struct cow_icon_box_config parsed = { .type = COW_ICON_BOX_GEOMETRY };
	const char *p = value;
	if (!parse_positive(&p, &parsed.width) || *p++ != 'x' ||
	    !parse_positive(&p, &parsed.height) ||
	    !parse_offset(&p, &parsed.x, &parsed.from_right) ||
	    !parse_offset(&p, &parsed.y, &parsed.from_bottom) || *p != '\0')
		return false;
	*box = parsed;
	return true;
}

static bool
parse_direction(const char *value, enum cow_icon_direction *direction)
{
	if (strcmp(value, "right") == 0)
		*direction = COW_ICON_DIR_RIGHT;
	else if (strcmp(value, "left") == 0)
		*direction = COW_ICON_DIR_LEFT;
	else if (strcmp(value, "down") == 0)
		*direction = COW_ICON_DIR_DOWN;
	else if (strcmp(value, "up") == 0)
		*direction = COW_ICON_DIR_UP;
	else
		return false;
	return true;
}

bool
cow_icon_fill_parse(const char *first, const char *second,
    enum cow_icon_direction *primary, enum cow_icon_direction *secondary)
{
	if (!parse_direction(first, primary) ||
	    !parse_direction(second, secondary))
		return false;
	bool ph = *primary == COW_ICON_DIR_LEFT ||
	    *primary == COW_ICON_DIR_RIGHT;
	bool sh = *secondary == COW_ICON_DIR_LEFT ||
	    *secondary == COW_ICON_DIR_RIGHT;
	return ph != sh;
}

bool
cow_icon_box_resolve(const struct cow_icon_box_config *cfg,
    const struct cow_box *usable, struct cow_box *box)
{
	if (cfg->type == COW_ICON_BOX_DEFAULT) {
		*box = *usable;
		box->x += 8;
		box->y += 8;
		box->width -= 16;
		box->height -= 16;
	} else if (cfg->type == COW_ICON_BOX_FULL) {
		*box = *usable;
	} else {
		int64_t usable_right = (int64_t)usable->x + usable->width;
		int64_t usable_bottom = (int64_t)usable->y + usable->height;
		int64_t left = cfg->from_right ?
		    usable_right - cfg->width - cfg->x :
		    (int64_t)usable->x + cfg->x;
		int64_t top = cfg->from_bottom ?
		    usable_bottom - cfg->height - cfg->y :
		    (int64_t)usable->y + cfg->y;
		int64_t right = left + cfg->width;
		int64_t bottom = top + cfg->height;
		left = left < usable->x ? usable->x : left;
		top = top < usable->y ? usable->y : top;
		right = right > usable_right ? usable_right : right;
		bottom = bottom > usable_bottom ? usable_bottom : bottom;
		if (right <= left || bottom <= top) {
			memset(box, 0, sizeof(*box));
			return false;
		}
		box->x = (int)left;
		box->y = (int)top;
		box->width = (int)(right - left);
		box->height = (int)(bottom - top);
	}
	return box->width > 0 && box->height > 0;
}

static bool
rect_overlaps(int x, int y, int width, int height,
    const struct cow_icon_rect *occupied, size_t count)
{
	for (size_t i = 0; i < count; i++) {
		const struct cow_icon_rect *o = &occupied[i];
		if ((int64_t)x < (int64_t)o->x + o->width &&
		    (int64_t)x + width > o->x &&
		    (int64_t)y < (int64_t)o->y + o->height &&
		    (int64_t)y + height > o->y)
			return true;
	}
	return false;
}

bool
cow_icon_placement_find(const struct cow_icon_placement *placement,
    const struct cow_box *box, int width, int height,
    const struct cow_icon_rect *occupied, size_t count, int *x_out, int *y_out)
{
	int64_t gx = placement->grid_x > 0 ? placement->grid_x :
	    (int64_t)width + 8;
	int64_t gy = placement->grid_y > 0 ? placement->grid_y :
	    (int64_t)height + 8;
	bool primary_h = placement->primary == COW_ICON_DIR_LEFT ||
	    placement->primary == COW_ICON_DIR_RIGHT;
	int64_t px_start = placement->primary == COW_ICON_DIR_LEFT ?
	    (int64_t)box->x + box->width - width : box->x;
	int64_t py_start = placement->primary == COW_ICON_DIR_UP ?
	    (int64_t)box->y + box->height - height : box->y;
	int64_t sx_start = placement->secondary == COW_ICON_DIR_LEFT ?
	    (int64_t)box->x + box->width - width : box->x;
	int64_t sy_start = placement->secondary == COW_ICON_DIR_UP ?
	    (int64_t)box->y + box->height - height : box->y;
	int pd = (placement->primary == COW_ICON_DIR_LEFT ||
	    placement->primary == COW_ICON_DIR_UP) ? -1 : 1;
	int sd = (placement->secondary == COW_ICON_DIR_LEFT ||
	    placement->secondary == COW_ICON_DIR_UP) ? -1 : 1;

	if (width > box->width || height > box->height)
		return false;
	if (primary_h) {
		for (int64_t y = sy_start; y >= box->y &&
		    y + height <= (int64_t)box->y + box->height;
		    y += sd * gy) {
			for (int64_t x = px_start; x >= box->x &&
			    x + width <= (int64_t)box->x + box->width;
			    x += pd * gx) {
				if (!rect_overlaps(x, y, width, height,
					occupied, count)) {
					*x_out = (int)x;
					*y_out = (int)y;
					return true;
				}
			}
		}
	} else {
		for (int64_t x = sx_start; x >= box->x &&
		    x + width <= (int64_t)box->x + box->width;
		    x += sd * gx) {
			for (int64_t y = py_start; y >= box->y &&
			    y + height <= (int64_t)box->y + box->height;
			    y += pd * gy) {
				if (!rect_overlaps(x, y, width, height,
					occupied, count)) {
					*x_out = (int)x;
					*y_out = (int)y;
					return true;
				}
			}
		}
	}
	return false;
}

void
cow_icon_placement_config_init(struct cow_config *config)
{
	cow_icon_placement_default(&config->icon_placement);
	wl_list_init(&config->icon_placement_overrides);
}

void
cow_icon_placement_config_fini(struct cow_config *config)
{
	struct cow_icon_placement_override *o, *tmp;
	wl_list_for_each_safe(o, tmp, &config->icon_placement_overrides, link) {
		wl_list_remove(&o->link);
		xfree(o->output);
		xfree(o);
	}
}

struct cow_icon_placement_override *
cow_icon_placement_override_get(struct cow_config *config, const char *output,
    bool create)
{
	struct cow_icon_placement_override *o;
	wl_list_for_each(o, &config->icon_placement_overrides, link) {
		if (strcmp(o->output, output) == 0)
			return o;
	}
	if (!create)
		return NULL;
	o = xcalloc(1, sizeof(*o));
	o->output = xstrdup(output);
	wl_list_insert(config->icon_placement_overrides.prev, &o->link);
	return o;
}

void
cow_icon_placement_override_remove(struct cow_config *config,
    const char *output)
{
	struct cow_icon_placement_override *o =
	    cow_icon_placement_override_get(config, output, false);
	if (o == NULL)
		return;
	wl_list_remove(&o->link);
	xfree(o->output);
	xfree(o);
}

static const struct cow_icon_placement_override *
find_override(const struct cow_config *config, const struct cow_output *output)
{
	struct cow_icon_placement_override *o;
	if (output->name != NULL) {
		wl_list_for_each(o, &config->icon_placement_overrides, link)
			if (strcmp(o->output, output->name) == 0)
				return o;
	}
	char number[32];
	snprintf(number, sizeof(number), "@%d", output->number);
	wl_list_for_each(o, &config->icon_placement_overrides, link)
		if (strcmp(o->output, number) == 0)
			return o;
	return NULL;
}

void
cow_icon_placement_resolve(const struct cow_config *config,
    const struct cow_output *output, struct cow_icon_placement *placement)
{
	*placement = config->icon_placement;
	const struct cow_icon_placement_override *o =
	    find_override(config, output);
	if (o == NULL)
		return;
	if (o->fields & COW_ICON_PLACEMENT_BOX)
		placement->box = o->placement.box;
	if (o->fields & COW_ICON_PLACEMENT_FILL) {
		placement->primary = o->placement.primary;
		placement->secondary = o->placement.secondary;
	}
	if (o->fields & COW_ICON_PLACEMENT_GRID) {
		placement->grid_x = o->placement.grid_x;
		placement->grid_y = o->placement.grid_y;
	}
	if (o->fields & COW_ICON_PLACEMENT_COMPACT)
		placement->compact = o->placement.compact;
}
