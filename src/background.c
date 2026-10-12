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
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <cJSON.h>
#include "cow/background.h"
#include "cow/cmd.h"
#include "cow/colour.h"
#include "cow/config.h"
#include "cow/desk_ops.h"
#include "cow/draw.h"
#include "cow/image.h"
#include "cow/image_cache.h"
#include "cow/memory.h"
#include "cow/output.h"
#include "cow/output_selector.h"
#include "cow/policy.h"
#include "cow/update.h"
#include "cow/util.h"
#include "cow/wm.h"

struct background {
	struct wl_list link;
	struct cow_policy scope;
	int col, row;
	float colour[4];
	bool is_image;
	char *image;
};

static void
background_free(struct background *bg)
{
	cow_policy_fini(&bg->scope);
	xfree(bg->image);
	xfree(bg);
}

void
cow_background_fini(struct cow_config *config)
{
	struct background *bg, *tmp;
	wl_list_for_each_safe(bg, tmp, &config->page_backgrounds, link) {
		wl_list_remove(&bg->link);
		background_free(bg);
	}
}

static const struct background *
background_find(const struct cow_output *output, const struct cow_desk *desk,
    int col, int row, bool image, int best)
{
	const struct background *found = NULL;
	if (cow_config == NULL)
		return NULL;
	struct background *bg;
	wl_list_for_each(bg, &cow_config->page_backgrounds, link) {
		if (bg->is_image != image)
			continue;
		struct cow_policy scope = bg->scope;
		/* Numeric output selectors remain usable before outputs exist. */
		if (scope.output_name != NULL && scope.output_name[0] == '@') {
			if (cow_output_find_by_spec(scope.output_name) != output)
				continue;
			scope.output_name = NULL;
		}
		if (!cow_policy_scope_matches(&scope, desk, output) ||
		    (bg->col >= 0 && (bg->col != col || bg->row != row)))
			continue;
		int rank = (bg->col >= 0 ? 4 : 0) +
		    (scope.desk_nr >= 0 || scope.desk_name != NULL ? 2 : 0) +
		    (bg->scope.output_name != NULL ? 1 : 0);
		if (rank < best)
			continue;
		best = rank;
		found = bg;
	}
	return found;
}

bool
cow_background_colour(const struct cow_output *output, const struct cow_desk *desk,
    int col, int row, float colour[4])
{
	const struct background *bg = background_find(output, desk, col, row,
	    false, output->has_bg_colour ? 1 : -1);
	const float *value = bg != NULL ? bg->colour :
	    output->has_bg_colour ? output->bg_colour :
	    cow_config != NULL && cow_config->has_output_bg_colour ?
	    cow_config->output_bg_colour : NULL;
	if (value == NULL)
		return false;
	memcpy(colour, value, sizeof(float) * 4);
	return true;
}

static const char *
background_image(const struct cow_output *output, const struct cow_desk *desk,
    int col, int row)
{
	const struct background *bg = background_find(output, desk, col, row,
	    true, output->has_bg_image ? 1 : -1);
	return bg != NULL ? bg->image : output->has_bg_image ? output->bg_image :
	    cow_config != NULL && cow_config->has_output_bg_image ?
	    cow_config->output_bg_image : NULL;
}

void
cow_background_paint(struct cow_output *output, struct cow_pixbuf *p)
{
	const struct cow_desk *desk = output->current_desk;
	int pw = cow_page_width(output), ph = cow_page_height(output);
	if (pw <= 0 || ph <= 0)
		return;
	int vx = desk != NULL ? desk->page_vx : 0;
	int vy = desk != NULL ? desk->page_vy : 0;
	int dx = (vx % pw + pw) % pw, dy = (vy % ph + ph) % ph;
	cairo_surface_t *surface = cairo_image_surface_create_for_data(
	    (unsigned char *)p->pixels, CAIRO_FORMAT_ARGB32,
	    p->width, p->height, p->stride_px * 4);
	cairo_t *cr = cairo_create(surface);
	for (int y = -dy, row = vy / ph - (vy % ph < 0); y < p->height; y += ph, row++) {
		for (int x = -dx, col = vx / pw - (vx % pw < 0); x < p->width; x += pw, col++) {
			float colour[4];
			if (cow_background_colour(output, desk, col, row, colour)) {
				cairo_set_source_rgba(cr, colour[0], colour[1], colour[2], colour[3]);
				cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
				cairo_rectangle(cr, x, y, pw, ph);
				cairo_fill(cr);
				cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
			}
			cow_image_cache_tile(&output->bg_images,
			    background_image(output, desk, col, row), cr,
			    x, y, pw, ph, 1, 1);
		}
	}
	cairo_destroy(cr);
	cairo_surface_destroy(surface);
}

static void
snapshot_page(cJSON *pages, const struct cow_output *output,
    const struct cow_desk *desk, int col, int row)
{
	float colour[4];
	bool has_colour = cow_background_colour(output, desk, col, row, colour);
	const char *image = background_image(output, desk, col, row);
	char value[16];
	cJSON *page = cJSON_CreateObject();
	cJSON_AddNumberToObject(page, "desk", desk->nr);
	cJSON_AddNumberToObject(page, "col", col);
	cJSON_AddNumberToObject(page, "row", row);
	if (has_colour) {
		cow_colour_format(value, sizeof(value), colour);
		cJSON_AddStringToObject(page, "background", value);
	}
	if (image != NULL)
		cJSON_AddStringToObject(page, "background_image", image);
	cJSON_AddItemToArray(pages, page);
}

struct cJSON *
cow_background_snapshot(const struct cow_output *output)
{
	cJSON *pages = cJSON_CreateArray();
	if (cow_config == NULL || wl_list_empty(&cow_config->page_backgrounds))
		return pages;
	struct cow_desk *desk;
	wl_list_for_each(desk, &output->desk_set.desks, set_link) {
		snapshot_page(pages, output, desk, -1, -1);
		struct background *bg;
		wl_list_for_each(bg, &cow_config->page_backgrounds, link) {
			if (bg->col < 0 || bg->col >= cow_config->page_cols ||
			    bg->row >= cow_config->page_rows)
				continue;
			bool duplicate = false;
			cJSON *page;
			cJSON_ArrayForEach(page, pages) {
				if (cJSON_GetObjectItem(page, "desk")->valueint == desk->nr &&
				    cJSON_GetObjectItem(page, "col")->valueint == bg->col &&
				    cJSON_GetObjectItem(page, "row")->valueint == bg->row) {
					duplicate = true;
					break;
				}
			}
			if (!duplicate)
				snapshot_page(pages, output, desk, bg->col, bg->row);
		}
	}
	return pages;
}

static enum cmd_retval
background_exec(struct cmd *cmd, struct cmd_q *q)
{
	struct args *args = cmd->args;
	bool image = strcmp(args->argv[0], "image") == 0;
	if (!image && strcmp(args->argv[0], "colour") != 0) {
		cmdq_error(q, "background: expected colour or image");
		return CMD_RETURN_ERROR;
	}
	struct background *bg = xcalloc(1, sizeof(*bg));
	bg->is_image = image;
	cow_policy_init(&bg->scope);
	bg->col = bg->row = -1;
	const char *target = args_get(args, 't');
	char *cause = NULL;
	if (target != NULL && !cow_policy_parse_scope(target, &bg->scope, &cause)) {
		cmdq_error(q, "background: %s", cause);
		xfree(cause);
		background_free(bg);
		return CMD_RETURN_ERROR;
	}
	bg->scope.name = xstrdup(target != NULL ? target : "");
	if (bg->scope.output_name != NULL && bg->scope.output_name[0] == '@' &&
	    !cow_output_selector_is_number(bg->scope.output_name)) {
		cmdq_error(q, "background: use an output name or @N");
		background_free(bg);
		return CMD_RETURN_ERROR;
	}
	const char *page = args_get(args, 'P');
	if (page != NULL) {
		long long col, row;
		if (strchr(page, 'p') != NULL || strchr(page, '-') != NULL ||
		    !cmd_parse_page_spec(q, "background", page, 0, 0, 99, 99, &col, &row)) {
			cmdq_error(q, "background: -P requires absolute col,row");
			background_free(bg);
			return CMD_RETURN_ERROR;
		}
		bg->col = col;
		bg->row = row;
	}
	bool inherit = strcmp(args->argv[1], "inherit") == 0;
	if (!inherit && image) {
		char *dir = cmd->file != NULL ? cow_path_dirname(cmd->file) : NULL;
		bg->image = strcmp(args->argv[1], "none") == 0 ? xstrdup("none") :
		    cow_path_resolve(dir, args->argv[1]);
		xfree(dir);
		/* Modules need an absolute filename, independent of their cwd. */
		if (bg->image != NULL && bg->image[0] != '/' &&
		    strcmp(bg->image, "none") != 0) {
			char *cwd = getcwd(NULL, 0);
			char *absolute = cwd != NULL ? cow_path_resolve(cwd, bg->image) : NULL;
			xfree(cwd);
			xfree(bg->image);
			bg->image = absolute;
		}
		if (bg->image == NULL || (strcmp(bg->image, "none") != 0 &&
		    !cow_image_validate(bg->image, &cause))) {
			cmdq_error(q, "background: %s", cause != NULL ? cause : "invalid image path");
			xfree(cause);
			background_free(bg);
			return CMD_RETURN_ERROR;
		}
	} else if (!inherit && !cow_colour_parse(args->argv[1], bg->colour)) {
		cmdq_error(q, "background: invalid colour '%s'", args->argv[1]);
		background_free(bg);
		return CMD_RETURN_ERROR;
	}
	struct background *old, *tmp;
	wl_list_for_each_safe(old, tmp, &cow_config->page_backgrounds, link) {
		if (strcmp(old->scope.name, bg->scope.name) == 0 &&
		    old->col == bg->col && old->row == bg->row && old->is_image == image) {
			wl_list_remove(&old->link);
			background_free(old);
		}
	}
	if (inherit)
		background_free(bg);
	else
		wl_list_insert(cow_config->page_backgrounds.prev, &bg->link);
	struct cow_output *output;
	wl_list_for_each(output, &cow_wm.outputs, wm_link)
		output->bg_dirty = true;
	cow_update_request(COW_UPDATE_MANAGE | COW_UPDATE_STATUS);
	return CMD_RETURN_NORMAL;
}

struct cmd_entry cmd_background_entry = {
	"background", "t:P:", 2, 2,
	"[-t output[:desk]] [-P col,row] colour|image VALUE|inherit",
	background_exec, CMD_OWN_TS | CMD_CONFIG,
};
