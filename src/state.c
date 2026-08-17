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
#include "cow/log.h"

#include <cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cow/compat.h"
#include "cow/config.h"
#include "cow/desk_ops.h"
#include "cow/json_util.h"
#include "cow/memory.h"
#include "cow/output.h"
#include "cow/stack.h"
#include "cow/view.h"
#include "cow/wm.h"

/*
 * Return the view's bottom-to-top position within its effective layer.
 * Store a compact rank rather than raise_seq itself: raise_seq is a uint64_t
 * near UINT64_MAX / 2 and cannot be represented exactly by cJSON's double.
 */
static int
view_stack_order(const struct cow_view *view)
{
	int		 order = 0;
	struct cow_view *other;

	wl_list_for_each(other, &cow_wm.views, wm_link)
	{
		if (other->closed || other == view)
			continue;
		if (cow_view_effective_layer(other) !=
		    cow_view_effective_layer(view))
			continue;
		if (other->raise_seq < view->raise_seq)
			order++;
	}

	return order;
}

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
		cJSON_AddBoolToObject(o, "user_named",
		    output->desk_set.user_named);
		cJSON *desk_pages = cJSON_AddArrayToObject(o, "desk_pages");
		cJSON *desk_names = cJSON_AddArrayToObject(o, "desk_names");
		struct cow_desk *dk;
		wl_list_for_each(dk, &output->desk_set.desks, set_link)
		{
			cJSON *dp = cJSON_CreateObject();
			cJSON_AddNumberToObject(dp, "page_col",
			    cow_page_width(output) > 0 ?
				dk->page_vx / cow_page_width(output) :
				0);
			cJSON_AddNumberToObject(dp, "page_row",
			    cow_page_height(output) > 0 ?
				dk->page_vy / cow_page_height(output) :
				0);
			cJSON_AddItemToArray(desk_pages, dp);

			cJSON *dn = cJSON_CreateObject();
			cJSON_AddNumberToObject(dn, "nr", dk->nr);
			cJSON_AddStringToObject(dn, "name",
			    dk->name != NULL ? dk->name : "");
			cJSON_AddItemToArray(desk_names, dn);
		}

		cJSON_AddItemToArray(outputs, o);
	}

	/* Save focused view identifier */
	if (cow_wm.focused_view != NULL &&
	    cow_wm.focused_view->identifier != NULL)
		cJSON_AddStringToObject(root, "focused",
		    cow_wm.focused_view->identifier);

	/* Save per-view state */
	cJSON		*views	 = cJSON_AddArrayToObject(root, "views");
	struct cow_view *view;
	wl_list_for_each(view, &cow_wm.views, wm_link)
	{
		if (view->closed || view->identifier == NULL)
			continue;

		cJSON *v = cJSON_CreateObject();
		cJSON_AddStringToObject(v, "id", view->identifier);
		cJSON_AddNumberToObject(v, "x", view->x);
		cJSON_AddNumberToObject(v, "y", view->y);
		cJSON_AddNumberToObject(v, "vx", view->vx);
		cJSON_AddNumberToObject(v, "vy", view->vy);
		cJSON_AddNumberToObject(v, "width", view->content_width);
		cJSON_AddNumberToObject(v, "height", view->content_height);
		cJSON_AddNumberToObject(v, "flags", view->flags);
		cJSON_AddNumberToObject(v, "layer", view->layer);
		cJSON_AddNumberToObject(v, "z_order",
		    view_stack_order(view));

		if (view->desk != NULL)
			cJSON_AddNumberToObject(v, "desk", view->desk->nr);
		if (view->output != NULL) {
			cJSON_AddNumberToObject(v, "output_x", view->output->x);
			cJSON_AddNumberToObject(v, "output_y", view->output->y);
		}

		cJSON_AddBoolToObject(v, "maximized", view->maximized);
		/* Per-axis bits (for window-maximize -h / -v).  Older
		 * state files without these keys restore as both-axis
		 * maximised via the maximized key alone -- see the
		 * cow_view_set_maximized fallback below. */
		cJSON_AddBoolToObject(v, "maximized_h", view->maximized_h);
		cJSON_AddBoolToObject(v, "maximized_v", view->maximized_v);
		cJSON_AddBoolToObject(v, "maximized_h_ignore_reserved",
		    view->maximized_h_ignore_reserved);
		cJSON_AddBoolToObject(v, "maximized_v_ignore_reserved",
		    view->maximized_v_ignore_reserved);
		cJSON_AddBoolToObject(v, "maximized_h_keep_geometry",
		    view->maximized_h_keep_geometry);
		cJSON_AddBoolToObject(v, "maximized_v_keep_geometry",
		    view->maximized_v_keep_geometry);
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

		/* Container membership */
		if (view->container_member != NULL) {
			struct cow_container *ctr =
			    view->container_member->container;
			/* Use the first member's identifier as the container
			 * ID; stable across restarts as long as the first
			 * member reconnects. */
			struct cow_container_member *first =
			    wl_container_of(ctr->members.next, first, link);
			if (first->view->identifier != NULL)
				cJSON_AddStringToObject(v, "container_id",
				    first->view->identifier);
			cJSON_AddBoolToObject(v, "container_active",
			    ctr->active == view->container_member);
		}

		cJSON_AddItemToArray(views, v);
	}

	char *json = cJSON_Print(root);
	cJSON_Delete(root);

	if (json == NULL) {
		cow_log_error("state save: cJSON_Print failed");
		return false;
	}

	FILE *f = fopen(path, "w");
	if (f == NULL) {
		cow_log_error("state save: cannot open %s", path);
		cJSON_free(json);
		return false;
	}

	fputs(json, f);
	fclose(f);
	cJSON_free(json);

	cow_log_debug("state saved to %s", path);
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

	char  *buf    = xmalloc(size + 1);
	size_t n_read = fread(buf, 1, size, f);
	fclose(f);
	if ((long)n_read != size) {
		cow_log_error("state restore: short read (%zu of %ld bytes)",
		    n_read, size);
		xfree(buf);
		return false;
	}
	buf[size] = '\0';

	cJSON *root = cJSON_Parse(buf);
	xfree(buf);

	if (root == NULL) {
		cow_log_error("state restore: parse error");
		return false;
	}

	bool restored = false;

	/* Restore per-output current sheet */
	cJSON *outputs = cJSON_GetObjectItem(root, "outputs");
	if (cJSON_IsArray(outputs)) {
		cJSON *out_obj;
		cJSON_ArrayForEach(out_obj, outputs)
		{
			long long ox, oy;
			if (!cow_json_get_int(out_obj, "x", &ox) ||
			    !cow_json_get_int(out_obj, "y", &oy))
				continue;

			struct cow_output *output =
			    find_output_by_position((int)ox, (int)oy);
			if (output == NULL)
				continue;

			cJSON *names = cJSON_GetObjectItem(out_obj,
			    "desk_names");
			bool user_named = false;
			(void)cow_json_get_bool(out_obj, "user_named",
			    &user_named);
			if (user_named && cJSON_IsArray(names)) {
				cow_desk_set_fini(&output->desk_set);
				cow_desk_set_init(&output->desk_set);
				cJSON *dn;
				cJSON_ArrayForEach(dn, names)
				{
					long long nr;
					const char *nm;
					if (!cow_json_get_int(dn, "nr", &nr))
						continue;
					if (!cow_json_get_string(dn, "name",
						&nm))
						nm = "";
					while (output->desk_set.count <=
					    (uint8_t)nr)
						(void)cow_desk_set_append_default(
						    output);
					struct cow_desk *dk = cow_desk_by_nr(
					    output, (uint8_t)nr);
					if (dk != NULL && nm[0] != '\0') {
						xstrset(&dk->name, nm);
					}
				}
				output->desk_set.user_named = true;
				output->current_desk = cow_desk_first(output);
			}

			long long cur_desk;
			if (cow_json_get_int(out_obj, "current_desk",
				&cur_desk)) {
				struct cow_desk *cd = cow_desk_by_nr(output,
				    (uint8_t)cur_desk);
				if (cd != NULL)
					output->current_desk = cd;
			}
			cJSON *desk_pages = cJSON_GetObjectItem(out_obj,
			    "desk_pages");
			if (cJSON_IsArray(desk_pages)) {
				int    w = cow_page_width(output);
				int    h = cow_page_height(output);
				int    i = 0;
				cJSON *dp;
				cJSON_ArrayForEach(dp, desk_pages)
				{
					struct cow_desk *dk = cow_desk_by_nr(
					    output, (uint8_t)i);
					if (dk == NULL)
						break;
					long long pc, pr;
					if (cow_json_get_int(dp, "page_col",
						&pc) &&
					    w > 0)
						dk->page_vx = (int)pc * w;
					if (cow_json_get_int(dp, "page_row",
						&pr) &&
					    h > 0)
						dk->page_vy = (int)pr * h;
					i++;
				}
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
				const char *vid;
				if (cow_json_get_string(v_obj, "id", &vid) &&
				    strcmp(vid, view->identifier) == 0) {
					vs = v_obj;
					break;
				}
			}
			if (vs == NULL)
				continue;

			/*
			 * River re-announces existing windows after exec.  They pass
			 * through cow_view_configure() before state restoration, which
			 * marks them for the focus and raise normally given to a new
			 * window.  Preserve the old stack and restore only the saved
			 * focus target instead.
			 */
			view->needs_focus = false;
			view->needs_raise = false;

			long long layer;
			int max_layer = cow_config != NULL ? cow_config->max_layer :
			    COW_CFG_MAX_LAYER;
			if (cow_json_get_int(vs, "layer", &layer) && layer >= 0)
				view->layer = (uint8_t)(layer > max_layer ? max_layer :
				    layer);

			long long z_order;
			if (cow_json_get_int(vs, "z_order", &z_order) &&
			    z_order >= 0)
				view->raise_seq = (uint64_t)z_order;

			/* Output matching — must happen before position
			 * restore so that vx/vy are computed against the
			 * correct output origin and page viewport. */
			long long out_x, out_y;
			if (cow_json_get_int(vs, "output_x", &out_x) &&
			    cow_json_get_int(vs, "output_y", &out_y)) {
				struct cow_output *target =
				    find_output_by_position((int)out_x,
					(int)out_y);
				if (target != NULL && target != view->output) {
					view->output = target;
					if (view->desk != NULL) {
						int nr = view->desk->nr;
						wl_list_remove(
						    &view->desk_views);
						struct cow_desk *td =
						    cow_desk_by_nr(target,
							(uint8_t)nr);
						if (td == NULL)
							td = cow_desk_first(
							    target);
						view->desk = td;
						if (td != NULL)
							wl_list_insert(
							    &td->views,
							    &view->desk_views);
					}
				}
			}

			/* Flags */
			long long vf;
			if (cow_json_get_int(vs, "flags", &vf))
				view->flags = (uint32_t)vf;

			/* Sheet assignment — must happen before position
			 * restore so pending_x/y use the correct page_vx/vy. */
			long long vdesk;
			if (cow_json_get_int(vs, "desk", &vdesk) &&
			    view->output != NULL) {
				struct cow_desk *td = cow_desk_by_nr(
				    view->output, (uint8_t)vdesk);
				if (td != NULL) {
					wl_list_remove(&view->desk_views);
					view->desk = td;
					wl_list_insert(&td->views,
					    &view->desk_views);
				}
			}

			/* Position and dimensions — now that output and desk
			 * are correct, compute pending_x/y from the saved
			 * virtual position against the right page viewport. */
			long long w, h, vx_j, vy_j, vx, vy;
			bool	  have_w  = cow_json_get_int(vs, "width", &w);
			bool	  have_h  = cow_json_get_int(vs, "height", &h);
			bool	  have_vx = cow_json_get_int(vs, "vx", &vx_j);
			bool	  have_vy = cow_json_get_int(vs, "vy", &vy_j);
			bool	  have_x  = cow_json_get_int(vs, "x", &vx);
			bool	  have_y  = cow_json_get_int(vs, "y", &vy);
			if (have_w && have_h) {
				if (have_vx)
					view->vx = (int)vx_j;
				else if (have_x && view->output != NULL &&
				    view->desk != NULL)
					view->vx = ((int)vx - view->output->x) +
					    view->desk->page_vx;
				else if (have_x && view->output != NULL)
					view->vx = (int)vx - view->output->x;
				if (have_vy)
					view->vy = (int)vy_j;
				else if (have_y && view->output != NULL &&
				    view->desk != NULL)
					view->vy = ((int)vy - view->output->y) +
					    view->desk->page_vy;
				else if (have_y && view->output != NULL)
					view->vy = (int)vy - view->output->y;

				if (w > 0 && h > 0 && view->output != NULL &&
				    view->desk != NULL) {
					int px = view->output->x +
					    (view->vx - view->desk->page_vx);
					int py = view->output->y +
					    (view->vy - view->desk->page_vy);
					cow_view_set_pending_geometry(view, px,
					    py, (int)w, (int)h);
					view->placed = true;
				}
			}

			/* Maximize state */
			bool vmax = false;
			if (cow_json_get_bool(vs, "maximized", &vmax) && vmax) {
				long long sx, sy, sw, sh;
				if (cow_json_get_int(vs, "saved_x", &sx) &&
				    cow_json_get_int(vs, "saved_y", &sy) &&
				    cow_json_get_int(vs, "saved_width", &sw) &&
				    cow_json_get_int(vs, "saved_height", &sh)) {
					view->saved_x	   = (int)sx;
					view->saved_y	   = (int)sy;
					view->saved_width  = (int)sw;
					view->saved_height = (int)sh;
					/*
					 * Honour per-axis bits when the
					 * state file has them; default to
					 * both-axis maxed otherwise so
					 * pre-split state files keep their
					 * behaviour.  cow_view_set_maximized
					 * stamps both axes; refine after.
					 */
					cow_view_set_maximized(view, true);
					bool mh = true, mv = true;
					cow_json_get_bool(vs, "maximized_h",
					    &mh);
					cow_json_get_bool(vs, "maximized_v",
					    &mv);
					view->maximized_h = mh;
					view->maximized_v = mv;
					view->maximized	  = mh || mv;
					cow_json_get_bool(vs,
					    "maximized_h_ignore_reserved",
					    &view->maximized_h_ignore_reserved);
					cow_json_get_bool(vs,
					    "maximized_v_ignore_reserved",
					    &view->maximized_v_ignore_reserved);
					cow_json_get_bool(vs,
					    "maximized_h_keep_geometry",
					    &view->maximized_h_keep_geometry);
					cow_json_get_bool(vs,
					    "maximized_v_keep_geometry",
					    &view->maximized_v_keep_geometry);
				}
			}

			/* Shade state */
			long long vshade;
			if (cow_json_get_int(vs, "shade_direction", &vshade)) {
				if (vshade > COW_SHADE_NONE &&
				    vshade <= COW_SHADE_SE) {
					long long ssw, ssh;
					if (cow_json_get_int(vs,
						"shade_saved_width", &ssw) &&
					    cow_json_get_int(vs,
						"shade_saved_height", &ssh)) {
						view->sd = (int)vshade;
						view->shade_saved_width = (int)
						    ssw;
						view->shade_saved_height = (int)
						    ssh;
						/* Shading keeps the client's real size and
						 * hides its content with a clip box.  Restore
						 * that clip instead of resizing the client;
						 * the saved geometry above remains the size to
						 * expose when the view is unshaded. */
						view->pending_clip = true;
						view->clip_x = view->clip_y = 0;
						view->clip_w = view->clip_h = 1;
						view->dec.dirty = true;
					}
				}
			}

			view->configured = true;
			restored	 = true;
		}
	}

	/* Restore container membership.
	 * Second pass: after all views are placed, group those that share a
	 * container_id.  The view whose identifier equals the container_id
	 * is used as the "target" (drop-point) when creating the container. */
	{
		cJSON *va2 = cJSON_GetObjectItem(root, "views");
		cJSON *vo2;
		cJSON_ArrayForEach(vo2, va2)
		{
			cJSON *vid2 = cJSON_GetObjectItem(vo2, "id");
			cJSON *vcid = cJSON_GetObjectItem(vo2, "container_id");
			if (!cJSON_IsString(vid2) || !cJSON_IsString(vcid))
				continue;

			/* Find this view */
			struct cow_view *vv = NULL;
			struct cow_view *sv;
			wl_list_for_each(sv, &cow_wm.views, wm_link)
			{
				if (sv->identifier != NULL &&
				    strcmp(sv->identifier, vid2->valuestring) ==
					0) {
					vv = sv;
					break;
				}
			}
			if (vv == NULL || vv->container_member != NULL)
				continue; /* already grouped or not found */

			/* Find the container anchor (the view whose id ==
			 * container_id) */
			struct cow_view *anchor = NULL;
			wl_list_for_each(sv, &cow_wm.views, wm_link)
			{
				if (sv->identifier != NULL &&
				    strcmp(sv->identifier, vcid->valuestring) ==
					0) {
					anchor = sv;
					break;
				}
			}
			if (anchor == NULL || anchor == vv)
				continue;

			/* Group: anchor is the primary (drop target) */
			struct cow_container *ctr = cow_view_container(anchor);
			if (ctr != NULL) {
				cow_container_add(ctr, vv);
			} else {
				ctr = cow_container_create(anchor, vv);
			}

			/* Restore active tab */
			cJSON *vca = cJSON_GetObjectItem(vo2,
			    "container_active");
			if (cJSON_IsTrue(vca) && vv->container_member != NULL)
				cow_container_activate(ctr,
				    vv->container_member);
		}
	}

	/* Apply the restored order before the next render pass. */
	if (restored)
		cow_stack_mark_dirty();

	/* Restore focus through the seat so all focus bookkeeping agrees. */
	const char *focused_id = NULL;
	if (cow_json_get_string(root, "focused", &focused_id)) {
		struct cow_view *view = cow_view_find_by_identifier(focused_id);
		if (view != NULL)
			cow_wm.pending_focus = view;
	}

	cJSON_Delete(root);

	/* Remove the state file after restore -- it's transient */
	unlink(path);

	if (restored)
		cow_log_info("state restored from %s", path);

	return restored;
}
