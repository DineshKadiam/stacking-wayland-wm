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

/*
 * DSL source adapters.
 *
 * Each adapter walks live wm state once and returns a snapshot list
 * of dsl_value records.  Record schemas mirror the corresponding
 * `show <topic>` JSON output so users can rely on a single set of
 * field names regardless of whether they're scripting in-process or
 * consuming JSON externally.
 *
 * Snapshots are point-in-time: a for-loop body that closes a window
 * still iterates the original list; commands operating on a stale
 * id fail at execution time with cow's usual not-found error.
 */

#include <string.h>

#include "cow/output.h"
#include "cow/rule.h"
#include "cow/view.h"
#include "cow/wm.h"

#include "dsl-sources.h"
#include "dsl-value.h"

/* Helper: set a string field, defaulting to empty when src is NULL. */
static void
rec_set_string(struct dsl_value *rec, const char *key, const char *s)
{
	struct dsl_value *v = dsl_value_string(s != NULL ? s : "");
	dsl_record_set(rec, key, v);
	dsl_value_unref(v);
}

static void
rec_set_number(struct dsl_value *rec, const char *key, double n)
{
	struct dsl_value *v = dsl_value_number(n);
	dsl_record_set(rec, key, v);
	dsl_value_unref(v);
}

static void
rec_set_bool(struct dsl_value *rec, const char *key, bool b)
{
	struct dsl_value *v = dsl_value_bool(b);
	dsl_record_set(rec, key, v);
	dsl_value_unref(v);
}

static struct dsl_value *
snapshot_view(const struct cow_view *view)
{
	struct dsl_value *r = dsl_value_record();

	rec_set_string(r, "id", view->identifier);
	rec_set_string(r, "app_id", view->app_id);
	rec_set_string(r, "title", view->title);
	rec_set_number(r, "desk", view->desk ? view->desk->nr : -1);
	rec_set_string(r, "output",
	    (view->output && view->output->name) ? view->output->name : "");
	rec_set_number(r, "output_number",
	    view->output ? view->output->number : 0);
	rec_set_number(r, "x", view->x);
	rec_set_number(r, "y", view->y);
	rec_set_number(r, "vx", view->vx);
	rec_set_number(r, "vy", view->vy);
	rec_set_number(r, "width", view->content_width);
	rec_set_number(r, "height", view->content_height);
	rec_set_bool(r, "focused", view == cow_wm.focused_view);
	rec_set_bool(r, "maximized", view->maximized);
	rec_set_bool(r, "shaded", view->sd != COW_SHADE_NONE);
	rec_set_bool(r, "iconified", cow_view_is_iconified(view));
	rec_set_number(r, "raise_seq", (double)view->raise_seq);
	rec_set_number(r, "layer", (double)view->layer);

	if (view->container_member != NULL) {
		struct cow_container *ctr = view->container_member->container;
		struct cow_container_member *first;
		first = wl_container_of(ctr->members.next, first, link);
		if (first->view != NULL && first->view->identifier != NULL)
			rec_set_string(r, "container_id",
			    first->view->identifier);
		rec_set_bool(r, "container_active",
		    ctr->active == view->container_member);
	}

	rec_set_bool(r, "use_ssd", view->use_ssd);
	return (r);
}

static struct dsl_value *
snapshot_desk(const struct cow_desk *desk)
{
	struct dsl_value *r = dsl_value_record();
	rec_set_number(r, "nr", desk->nr);
	rec_set_string(r, "output",
	    (desk->output && desk->output->name) ? desk->output->name : "");
	rec_set_number(r, "output_number",
	    desk->output ? desk->output->number : 0);
	rec_set_bool(r, "current",
	    desk->output && desk->output->current_desk == desk);
	rec_set_number(r, "page_vx", desk->page_vx);
	rec_set_number(r, "page_vy", desk->page_vy);

	/* Count views on this desk. */
	size_t		 n = 0;
	struct cow_view *vw;
	wl_list_for_each(vw, &desk->views, desk_views)
	{
		if (!vw->closed)
			n++;
	}
	rec_set_number(r, "window_count", (double)n);

	return (r);
}

static struct dsl_value *
snapshot_output(const struct cow_output *out)
{
	struct dsl_value *r = dsl_value_record();
	rec_set_string(r, "name", out->name);
	rec_set_number(r, "number", out->number);
	rec_set_number(r, "x", out->x);
	rec_set_number(r, "y", out->y);
	rec_set_number(r, "width", out->width);
	rec_set_number(r, "height", out->height);
	rec_set_number(r, "usable_x", out->usable_area.x);
	rec_set_number(r, "usable_y", out->usable_area.y);
	rec_set_number(r, "usable_width", out->usable_area.width);
	rec_set_number(r, "usable_height", out->usable_area.height);
	rec_set_bool(r, "current", out == cow_wm.current_output);
	rec_set_number(r, "current_desk",
	    out->current_desk ? out->current_desk->nr : -1);
	return (r);
}

static struct dsl_value *
snapshot_rule(const struct cow_rule *rule)
{
	struct dsl_value *r = dsl_value_record();
	rec_set_string(r, "name", rule->name);
	rec_set_string(r, "type", cow_rule_type_name(rule->type));
	rec_set_bool(r, "global", rule->is_global);
	rec_set_string(r, "output",
	    rule->output_name != NULL ? rule->output_name : "");
	rec_set_number(r, "desk", rule->desk_nr);
	rec_set_string(r, "match",
	    rule->match_spec != NULL ? rule->match_spec : "");
	return (r);
}

struct dsl_value *
dsl_source_snapshot(enum dsl_source src)
{
	struct dsl_value *list = dsl_value_list();

	switch (src) {
	case DSL_SRC_WINDOWS: {
		struct cow_view *vw;
		wl_list_for_each(vw, &cow_wm.views, wm_link)
		{
			if (vw->closed)
				continue;
			struct dsl_value *r = snapshot_view(vw);
			dsl_list_append(list, r);
			dsl_value_unref(r);
		}
		break;
	}
	case DSL_SRC_DESKS: {
		struct cow_output *out;
		wl_list_for_each(out, &cow_wm.outputs, wm_link)
		{
			struct cow_desk *desk;
			wl_list_for_each(desk, &out->desk_set.desks, set_link)
			{
				struct dsl_value *r = snapshot_desk(desk);
				dsl_list_append(list, r);
				dsl_value_unref(r);
			}
		}
		break;
	}
	case DSL_SRC_OUTPUTS: {
		struct cow_output *out;
		wl_list_for_each(out, &cow_wm.outputs, wm_link)
		{
			struct dsl_value *r = snapshot_output(out);
			dsl_list_append(list, r);
			dsl_value_unref(r);
		}
		break;
	}
	case DSL_SRC_RULES: {
		const struct wl_list *head = cow_rules_head();
		if (head != NULL) {
			struct cow_rule *rule;
			wl_list_for_each(rule, head, link)
			{
				struct dsl_value *r = snapshot_rule(rule);
				dsl_list_append(list, r);
				dsl_value_unref(r);
			}
		}
		break;
	}
	}

	return (list);
}
