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
#include "cow/state.h"

#include <cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cow/compat.h"
#include "cow/memory.h"
#include "cow/output.h"
#include "cow/desk_ops.h"
#include "cow/view.h"
#include "cow/wm.h"

#include "river-window-management-v1-client-protocol.h"

const char *
cow_state_path(void)
{
	static char path[4096];
	const char *runtime = getenv("XDG_RUNTIME_DIR");
	if (runtime != NULL)
		snprintf(path, sizeof(path), "%s/cow-state.json", runtime);
	else
		snprintf(path, sizeof(path), "/tmp/cow-state-%d.json",
		    (int)getpid());
	return path;
}

bool
cow_state_save(void)
{
	const char *path = cow_state_path();

	cJSON *root = cJSON_CreateObject();

	/* Save per-output current sheet */
	cJSON		  *outputs = cJSON_AddArrayToObject(root, "outputs");
	struct cow_output *output;
	wl_list_for_each(output, &cow_wm.outputs, wm_link)
	{
		cJSON *o = cJSON_CreateObject();
		cJSON_AddNumberToObject(o, "x", output->x);
		cJSON_AddNumberToObject(o, "y", output->y);
		cJSON_AddNumberToObject(o, "current_desk",
		    output->current_desk->nr);

		cJSON_AddItemToArray(outputs, o);
	}

	/* Save focused view identifier */
	if (cow_wm.focused_view != NULL &&
	    cow_wm.focused_view->identifier != NULL)
		cJSON_AddStringToObject(root, "focused",
		    cow_wm.focused_view->identifier);

	/* Save per-view state */
	cJSON		*views	 = cJSON_AddArrayToObject(root, "views");
	int		 z_order = 0;
	struct cow_view *view;
	wl_list_for_each(view, &cow_wm.views, wm_link)
	{
		if (view->closed || view->identifier == NULL)
			continue;

		cJSON *v = cJSON_CreateObject();
		cJSON_AddStringToObject(v, "id", view->identifier);
		cJSON_AddNumberToObject(v, "x", view->x);
		cJSON_AddNumberToObject(v, "y", view->y);
		cJSON_AddNumberToObject(v, "width", view->content_width);
		cJSON_AddNumberToObject(v, "height", view->content_height);
		cJSON_AddNumberToObject(v, "flags", view->flags);
		cJSON_AddNumberToObject(v, "z_order", z_order++);

		if (view->desk != NULL)
			cJSON_AddNumberToObject(v, "desk", view->desk->nr);
		if (view->output != NULL) {
			cJSON_AddNumberToObject(v, "output_x", view->output->x);
			cJSON_AddNumberToObject(v, "output_y", view->output->y);
		}

		cJSON_AddBoolToObject(v, "maximized", view->maximized);
		if (view->maximized) {
			cJSON_AddNumberToObject(v, "saved_x", view->saved_x);
			cJSON_AddNumberToObject(v, "saved_y", view->saved_y);
			cJSON_AddNumberToObject(v, "saved_width",
			    view->saved_width);
			cJSON_AddNumberToObject(v, "saved_height",
			    view->saved_height);
		}

		cJSON_AddNumberToObject(v, "shade_direction", view->sd);
		if (view->sd != COW_SHADE_NONE) {
			cJSON_AddNumberToObject(v, "shade_saved_width",
			    view->shade_saved_width);
			cJSON_AddNumberToObject(v, "shade_saved_height",
			    view->shade_saved_height);
		}

		cJSON_AddItemToArray(views, v);
	}

	char *json = cJSON_Print(root);
	cJSON_Delete(root);

	if (json == NULL) {
		fprintf(stderr, "cow: state save: cJSON_Print failed\n");
		return false;
	}

	FILE *f = fopen(path, "w");
	if (f == NULL) {
		fprintf(stderr, "cow: state save: cannot open %s\n", path);
		cJSON_free(json);
		return false;
	}

	fputs(json, f);
	fclose(f);
	cJSON_free(json);

	fprintf(stderr, "cow: state saved to %s\n", path);
	return true;
}

/*
 * Find an output by its position (used to match outputs across restarts).
 */
static struct cow_output *
find_output_by_position(int x, int y)
{
	struct cow_output *output;
	wl_list_for_each(output, &cow_wm.outputs, wm_link)
	{
		if (output->x == x && output->y == y)
			return output;
	}
	return NULL;
}

bool
cow_state_restore(void)
{
	const char *path = cow_state_path();

	if (access(path, R_OK) != 0)
		return false;

	/* Read entire file */
	FILE *f = fopen(path, "r");
	if (f == NULL)
		return false;

	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	fseek(f, 0, SEEK_SET);

	if (size <= 0) {
		fclose(f);
		return false;
	}

	char *buf = xmalloc(size + 1);
	fread(buf, 1, size, f);
	buf[size] = '\0';
	fclose(f);

	cJSON *root = cJSON_Parse(buf);
	xfree(buf);

	if (root == NULL) {
		fprintf(stderr, "cow: state restore: parse error\n");
		return false;
	}

	bool restored = false;

	/* Restore per-output current sheet */
	cJSON *outputs = cJSON_GetObjectItem(root, "outputs");
	if (cJSON_IsArray(outputs)) {
		cJSON *out_obj;
		cJSON_ArrayForEach(out_obj, outputs)
		{
			cJSON *ox = cJSON_GetObjectItem(out_obj, "x");
			cJSON *oy = cJSON_GetObjectItem(out_obj, "y");
			if (!cJSON_IsNumber(ox) || !cJSON_IsNumber(oy))
				continue;

			struct cow_output *output = find_output_by_position(
			    (int)ox->valuedouble, (int)oy->valuedouble);
			if (output == NULL)
				continue;

			cJSON *cs = cJSON_GetObjectItem(out_obj,
			    "current_desk");
			if (cJSON_IsNumber(cs)) {
				int nr = (int)cs->valuedouble;
				if (nr >= 0 && nr < COW_NR_OF_DESKS)
					output->current_desk =
					    &output->desks[nr];
			}
		}
	}

	/* Restore per-view state */
	cJSON *views_arr = cJSON_GetObjectItem(root, "views");
	if (cJSON_IsArray(views_arr)) {
		struct cow_view *view;
		wl_list_for_each(view, &cow_wm.views, wm_link)
		{
			if (view->identifier == NULL || view->closed)
				continue;

			/* Find matching state entry */
			cJSON *vs = NULL;
			cJSON *v_obj;
			cJSON_ArrayForEach(v_obj, views_arr)
			{
				cJSON *vid = cJSON_GetObjectItem(v_obj, "id");
				if (cJSON_IsString(vid) &&
				    strcmp(vid->valuestring,
					view->identifier) == 0) {
					vs = v_obj;
					break;
				}
			}
			if (vs == NULL)
				continue;

			/* Position and dimensions -- use set_pending_geometry
			 * so both are pushed to River in the next manage cycle.
			 */
			cJSON *vx = cJSON_GetObjectItem(vs, "x");
			cJSON *vy = cJSON_GetObjectItem(vs, "y");
			cJSON *vw = cJSON_GetObjectItem(vs, "width");
			cJSON *vh = cJSON_GetObjectItem(vs, "height");
			if (cJSON_IsNumber(vx) && cJSON_IsNumber(vy) &&
			    cJSON_IsNumber(vw) && cJSON_IsNumber(vh)) {
				int x = (int)vx->valuedouble;
				int y = (int)vy->valuedouble;
				int w = (int)vw->valuedouble;
				int h = (int)vh->valuedouble;
				if (w > 0 && h > 0)
					cow_view_set_pending_geometry(view, x,
					    y, w, h);
			}

			/* Flags */
			cJSON *vf = cJSON_GetObjectItem(vs, "flags");
			if (cJSON_IsNumber(vf))
				view->flags = (uint32_t)vf->valuedouble;

			/* Sheet assignment */
			cJSON *vdesk = cJSON_GetObjectItem(vs, "desk");
			if (cJSON_IsNumber(vdesk) && view->output != NULL) {
				int nr = (int)vdesk->valuedouble;
				if (nr >= 0 && nr < COW_NR_OF_DESKS) {
					wl_list_remove(&view->desk_views);
					view->desk = &view->output->desks[nr];
					wl_list_insert(&view->desk->views,
					    &view->desk_views);
				}
			}

			/* Output matching */
			cJSON *out_x = cJSON_GetObjectItem(vs, "output_x");
			cJSON *out_y = cJSON_GetObjectItem(vs, "output_y");
			if (cJSON_IsNumber(out_x) && cJSON_IsNumber(out_y)) {
				struct cow_output *target =
				    find_output_by_position(
					(int)out_x->valuedouble,
					(int)out_y->valuedouble);
				if (target != NULL && target != view->output) {
					view->output = target;
					if (view->desk != NULL) {
						int nr = view->desk->nr;
						wl_list_remove(
						    &view->desk_views);
						view->desk =
						    &target->desks[nr];
						wl_list_insert(
						    &view->desk->views,
						    &view->desk_views);
					}
				}
			}

			/* Maximize state */
			cJSON *vmax = cJSON_GetObjectItem(vs, "maximized");
			if (cJSON_IsTrue(vmax)) {
				cJSON *sx = cJSON_GetObjectItem(vs, "saved_x");
				cJSON *sy = cJSON_GetObjectItem(vs, "saved_y");
				cJSON *sw = cJSON_GetObjectItem(vs,
				    "saved_width");
				cJSON *sh = cJSON_GetObjectItem(vs,
				    "saved_height");
				if (cJSON_IsNumber(sx) && cJSON_IsNumber(sy) &&
				    cJSON_IsNumber(sw) && cJSON_IsNumber(sh)) {
					view->saved_x = (int)sx->valuedouble;
					view->saved_y = (int)sy->valuedouble;
					view->saved_width = (int)
								sw->valuedouble;
					view->saved_height =
					    (int)sh->valuedouble;
					view->maximized = true;
				}
			}

			/* Shade state */
			cJSON *vshade = cJSON_GetObjectItem(vs,
			    "shade_direction");
			if (cJSON_IsNumber(vshade)) {
				int dir = (int)vshade->valuedouble;
				if (dir > COW_SHADE_NONE &&
				    dir <= COW_SHADE_SE) {
					cJSON *ssw = cJSON_GetObjectItem(vs,
					    "shade_saved_width");
					cJSON *ssh = cJSON_GetObjectItem(vs,
					    "shade_saved_height");
					if (cJSON_IsNumber(ssw) &&
					    cJSON_IsNumber(ssh)) {
						view->sd = dir;
						view->shade_saved_width =
						    (int)ssw->valuedouble;
						view->shade_saved_height =
						    (int)ssh->valuedouble;
						river_window_v1_propose_dimensions(
						    view->river_window, 1, 1);
					}
				}
			}

			view->configured = true;
			restored	 = true;
		}
	}

	/* Restore focus */
	cJSON *focused = cJSON_GetObjectItem(root, "focused");
	if (cJSON_IsString(focused)) {
		struct cow_view *view;
		wl_list_for_each(view, &cow_wm.views, wm_link)
		{
			if (view->identifier != NULL && !view->closed &&
			    strcmp(view->identifier, focused->valuestring) ==
				0) {
				cow_wm.focused_view = view;
				view->needs_raise   = true;
				break;
			}
		}
	}

	cJSON_Delete(root);

	/* Remove the state file after restore -- it's transient */
	unlink(path);

	if (restored)
		fprintf(stderr, "cow: state restored from %s\n", path);

	return restored;
}
