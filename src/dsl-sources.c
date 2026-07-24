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
#include "cow/config.h"
#include "cow/cmd.h"
#include "cow/titlebar.h"
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

static void
rec_set_box(struct dsl_value *rec, const char *key, const struct cow_box *box)
{
	struct dsl_value *v = dsl_value_record();

	rec_set_number(v, "x", box->x);
	rec_set_number(v, "y", box->y);
	rec_set_number(v, "width", box->width);
	rec_set_number(v, "height", box->height);
	dsl_record_set(rec, key, v);
	dsl_value_unref(v);
}

struct dsl_value *
dsl_snapshot_view(const struct cow_view *view)
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
	rec_set_number(r, "page_col", cow_view_page_col(view));
	rec_set_number(r, "page_row", cow_view_page_row(view));
	rec_set_bool(r, "current_desk", cow_view_current_desk(view));
	rec_set_bool(r, "current_output", cow_view_current_output(view));
	rec_set_bool(r, "current_page", cow_view_current_page(view));
	rec_set_bool(r, "focused", view == cow_wm.focused_view);
	rec_set_bool(r, "maximized", view->maximized);
	rec_set_bool(r, "shaded", view->sd != COW_SHADE_NONE);
	rec_set_bool(r, "iconified", cow_view_is_iconified(view));
	rec_set_bool(r, "circulate_skip",
	    (view->flags & COW_VIEW_CIRCULATE_SKIP_FLAG) != 0);
	rec_set_number(r, "raise_seq", (double)view->raise_seq);
	rec_set_number(r, "layer", (double)view->layer);
	rec_set_bool(r, "sticky_desk",
	    (view->flags & COW_VIEW_STICKY_DESK_FLAG) != 0);
	rec_set_bool(r, "sticky_page",
	    (view->flags & COW_VIEW_STICKY_PAGE_FLAG) != 0);

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
	rec_set_number(r, "title_thickness", view->dec.title_thickness);
	rec_set_string(r, "titlebar_position",
	    cow_titlebar_position_name(view->cfg.titlebar.position));
	rec_set_number(r, "border_width", view->dec.border_width);

	struct cow_titlebar_squeeze sq;
	cow_titlebar_effective_squeeze(view, &sq);
	struct dsl_value *sqv = dsl_value_record();
	rec_set_string(sqv, "mode",
	    sq.mode == COW_SQUEEZE_NATURAL ? "natural" :
	    sq.mode == COW_SQUEEZE_FIXED ? "fixed" : "none");
	rec_set_string(sqv, "justify",
	    sq.justify == COW_JUST_CENTRE ? "centre" :
	    sq.justify == COW_JUST_RIGHT ? "right" : "left");
	rec_set_number(sqv, "width", sq.width);
	rec_set_number(sqv, "offset", sq.offset);
	rec_set_number(sqv, "min_width", sq.min_width);
	dsl_record_set(r, "squeeze", sqv);
	dsl_value_unref(sqv);

	struct cow_box tbr = cow_titlebar_squeezed_rect(view);
	rec_set_box(r, "squeezed_rect", &tbr);
	return (r);
}

static struct dsl_value *
snapshot_desk(const struct cow_desk *desk)
{
	struct dsl_value *r = dsl_value_record();
	rec_set_number(r, "nr", desk->nr);
	rec_set_string(r, "name", desk->name);
	rec_set_string(r, "output",
	    (desk->output && desk->output->name) ? desk->output->name : "");
	rec_set_number(r, "output_number",
	    desk->output ? desk->output->number : 0);
	rec_set_bool(r, "current",
	    desk->output && desk->output->current_desk == desk);
	rec_set_bool(r, "active",
	    desk->output && desk->output->current_desk == desk);
	rec_set_bool(r, "collected",
	    desk->output != NULL &&
	    (desk->output->collected_desks & (1u << desk->nr)) != 0);
	rec_set_number(r, "page_vx", desk->page_vx);
	rec_set_number(r, "page_vy", desk->page_vy);
	int pw	  = desk->output != NULL ? desk->output->usable_area.width : 0;
	int ph	  = desk->output != NULL ? desk->output->usable_area.height : 0;
	int pcols = cow_config ? cow_config->page_cols :
				 COW_CFG_DEFAULT_PAGE_COLS;
	int prows = cow_config ? cow_config->page_rows :
				 COW_CFG_DEFAULT_PAGE_ROWS;
	rec_set_number(r, "page_cols", pcols);
	rec_set_number(r, "page_rows", prows);
	rec_set_number(r, "page_col",
	    (pw > 0 && pcols > 1) ? desk->page_vx / pw : 0);
	rec_set_number(r, "page_row",
	    (ph > 0 && prows > 1) ? desk->page_vy / ph : 0);

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
	rec_set_number(r, "nr", out->number);
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
	rec_set_string(r, "current_desk_name",
	    out->current_desk != NULL ? out->current_desk->name : "");
	int pcols = cow_config ? cow_config->page_cols :
				 COW_CFG_DEFAULT_PAGE_COLS;
	int prows = cow_config ? cow_config->page_rows :
				 COW_CFG_DEFAULT_PAGE_ROWS;
	int pw = out->usable_area.width;
	int ph = out->usable_area.height;
	rec_set_number(r, "page_cols", pcols);
	rec_set_number(r, "page_rows", prows);
	rec_set_number(r, "page_col",
	    (out->current_desk != NULL && pw > 0 && pcols > 1) ?
	    out->current_desk->page_vx / pw : 0);
	rec_set_number(r, "page_row",
	    (out->current_desk != NULL && ph > 0 && prows > 1) ?
	    out->current_desk->page_vy / ph : 0);
	rec_set_number(r, "page_vx",
	    out->current_desk != NULL ? out->current_desk->page_vx : 0);
	rec_set_number(r, "page_vy",
	    out->current_desk != NULL ? out->current_desk->page_vy : 0);
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
	if (rule->desk_name != NULL)
		rec_set_string(r, "desk", rule->desk_name);
	else
		rec_set_number(r, "desk", rule->desk_nr);
	rec_set_string(r, "match",
	    rule->match_spec != NULL ? rule->match_spec : "");
	if (rule->cmdlist != NULL) {
		char buf[1024];
		buf[0] = '\0';
		cmd_list_print(rule->cmdlist, buf, sizeof(buf));
		rec_set_string(r, "body", buf);
	} else
		rec_set_string(r, "body", "");
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
			struct dsl_value *r = dsl_snapshot_view(vw);
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
