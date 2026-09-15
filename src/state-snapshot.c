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
 * Canonical state snapshot adapters.
 *
 * Each adapter walks live wm state once and returns a snapshot list
 * of state_value records. JSON queries and the DSL consume these same
 * records so their public field sets cannot drift apart.
 *
 * Snapshots are point-in-time: a for-loop body that closes a window
 * still iterates the original list; commands operating on a stale
 * id fail at execution time with cow's usual not-found error.
 */

#include <string.h>

#include "cow/alias.h"
#include "cow/cmd.h"
#include "cow/colour.h"
#include "cow/config.h"
#include "cow/decoration.h"
#include "cow/memory.h"
#include "cow/menu.h"
#include "cow/output.h"
#include "cow/rule.h"
#include "cow/selection.h"
#include "cow/style.h"
#include "cow/titlebar.h"
#include "cow/view.h"
#include "cow/wm.h"

#include "decor-profile.h"
#include "state-snapshot.h"

/* Helper: set a string field, defaulting to empty when src is NULL. */
static void
rec_set_string(struct state_value *rec, const char *key, const char *s)
{
	struct state_value *v = state_value_string(s != NULL ? s : "");
	state_record_set(rec, key, v);
	state_value_unref(v);
}

static void
rec_set_number(struct state_value *rec, const char *key, double n)
{
	struct state_value *v = state_value_number(n);
	state_record_set(rec, key, v);
	state_value_unref(v);
}

static void
rec_set_bool(struct state_value *rec, const char *key, bool b)
{
	struct state_value *v = state_value_bool(b);
	state_record_set(rec, key, v);
	state_value_unref(v);
}

static void
rec_set_colour(struct state_value *rec, const char *key, const float *colour)
{
	char value[16];

	cow_colour_format(value, sizeof(value), colour);
	rec_set_string(rec, key, value);
}

static struct state_value *
gradient_snapshot(const struct cow_gradient *gradient)
{
	struct state_value *record = state_value_record();
	const char *mode = gradient->mode == COW_GRADIENT_H ? "horizontal" :
	    gradient->mode == COW_GRADIENT_V ? "vertical" : "none";

	rec_set_string(record, "mode", mode);
	rec_set_colour(record, "from", gradient->from);
	rec_set_colour(record, "to", gradient->to);
	return (record);
}

static void
rec_set_box(struct state_value *rec, const char *key, const struct cow_box *box)
{
	struct state_value *v = state_value_record();

	rec_set_number(v, "x", box->x);
	rec_set_number(v, "y", box->y);
	rec_set_number(v, "width", box->width);
	rec_set_number(v, "height", box->height);
	state_record_set(rec, key, v);
	state_value_unref(v);
}

struct state_value *
state_snapshot_view(const struct cow_view *view)
{
	struct state_value *r = state_value_record();

	rec_set_string(r, "id", view->identifier);
	rec_set_string(r, "app_id", view->app_id);
	rec_set_string(r, "icon", cow_view_icon_image(view));
	rec_set_string(r, "title", view->title);
	rec_set_string(r, "keyboard_layout", view->keyboard_layout);
	char *display_title = cow_decoration_display_title(view);
	rec_set_string(r, "display_title", display_title);
	xfree(display_title);
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
	struct cow_box frame = cow_view_frame_box(view);
	rec_set_number(r, "frame_x", frame.x);
	rec_set_number(r, "frame_y", frame.y);
	rec_set_number(r, "frame_width", frame.width);
	rec_set_number(r, "frame_height", frame.height);
	rec_set_number(r, "icon_x", view->icon_x);
	rec_set_number(r, "icon_y", view->icon_y);
	rec_set_number(r, "icon_vx",
	    view->icon_x - (view->output ? view->output->x : 0) +
	    (view->desk ? view->desk->page_vx : 0));
	rec_set_number(r, "icon_vy",
	    view->icon_y - (view->output ? view->output->y : 0) +
	    (view->desk ? view->desk->page_vy : 0));
	rec_set_number(r, "icon_width", view->icon_width);
	rec_set_number(r, "icon_height", view->icon_height);
	rec_set_number(r, "page_col", cow_view_page_col(view));
	rec_set_number(r, "page_row", cow_view_page_row(view));
	rec_set_bool(r, "current_desk", cow_view_current_desk(view));
	rec_set_bool(r, "current_output", cow_view_current_output(view));
	rec_set_bool(r, "current_page", cow_view_current_page(view));
	rec_set_bool(r, "focused", view == cow_wm_effective_focus());
	rec_set_bool(r, "selected", cow_selection_has(view, NULL));
	struct state_value *selections = state_value_list();
	struct cow_selection_entry *selection;
	wl_list_for_each(selection, &view->selections, link)
	{
		struct state_value *name = state_value_string(selection->name);
		state_list_append(selections, name);
		state_value_unref(name);
	}
	state_record_set(r, "selections", selections);
	state_value_unref(selections);
	rec_set_bool(r, "maximized", view->maximized);
	rec_set_bool(r, "fullscreen", view->fullscreen);
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

	rec_set_string(r, "container_id", "");
	rec_set_bool(r, "container_active", false);
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
	rec_set_number(r, "min_width", view->min_width);
	rec_set_number(r, "min_height", view->min_height);
	rec_set_number(r, "max_width", view->max_width);
	rec_set_number(r, "max_height", view->max_height);
	rec_set_number(r, "configured_border_width", view->cfg.border);
	rec_set_bool(r, "titlebar_enabled", view->cfg.titlebar.enabled);

	struct cow_titlebar_squeeze sq;
	cow_titlebar_effective_squeeze(view, &sq);
	struct state_value *sqv = state_value_record();
	rec_set_string(sqv, "mode",
	    sq.mode == COW_SQUEEZE_NATURAL ? "natural" :
	    sq.mode == COW_SQUEEZE_FIXED ? "fixed" : "none");
	rec_set_string(sqv, "justify",
	    sq.justify == COW_JUST_CENTRE ? "centre" :
	    sq.justify == COW_JUST_RIGHT ? "right" : "left");
	rec_set_number(sqv, "width", sq.width);
	rec_set_number(sqv, "offset", sq.offset);
	rec_set_number(sqv, "min_width", sq.min_width);
	state_record_set(r, "squeeze", sqv);
	state_value_unref(sqv);

	struct cow_box tbr = cow_titlebar_squeezed_rect(view);
	rec_set_box(r, "squeezed_rect", &tbr);
	return (r);
}

struct state_value *
state_snapshot_desk(const struct cow_desk *desk)
{
	struct state_value *r = state_value_record();
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

struct state_value *
state_snapshot_output(const struct cow_output *out)
{
	struct state_value *r = state_value_record();
	char		   colour[16];
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
	if (out->has_bg_colour) {
		cow_colour_format(colour, sizeof(colour), out->bg_colour);
		rec_set_string(r, "background", colour);
	} else if (cow_config != NULL && cow_config->has_output_bg_colour) {
		cow_colour_format(colour, sizeof(colour),
		    cow_config->output_bg_colour);
		rec_set_string(r, "background", colour);
	}
	if (out->has_bg_image && out->bg_image != NULL)
		rec_set_string(r, "background_image", out->bg_image);
	else if (cow_config != NULL && cow_config->has_output_bg_image &&
	    cow_config->output_bg_image != NULL)
		rec_set_string(r, "background_image",
		    cow_config->output_bg_image);
	return (r);
}

static void
snapshot_policy(struct state_value *record, const struct cow_policy *policy)
{
	rec_set_string(record, "name", policy->name);
	rec_set_bool(record, "global", policy->is_global);
	rec_set_string(record, "output", policy->output_name != NULL ?
	    policy->output_name : "");
	if (policy->desk_name != NULL)
		rec_set_string(record, "desk", policy->desk_name);
	else
		rec_set_number(record, "desk", policy->desk_nr);
	rec_set_string(record, "match", policy->match_spec != NULL ?
	    policy->match_spec : "");
}

struct state_value *
state_snapshot_rule(const struct cow_rule *rule)
{
	struct state_value *r = state_value_record();
	snapshot_policy(r, &rule->policy);
	rec_set_string(r, "type", cow_rule_type_name(rule->type));
	if (rule->cmdlist != NULL) {
		char *body = cmd_list_to_config(rule->cmdlist);
		rec_set_string(r, "body", body);
		rec_set_bool(r, "body_is_single",
		    TAILQ_FIRST(&rule->cmdlist->list) != NULL &&
		    TAILQ_NEXT(TAILQ_FIRST(&rule->cmdlist->list), qentry) == NULL);
		xfree(body);
	} else {
		rec_set_string(r, "body", "");
		rec_set_bool(r, "body_is_single", false);
	}
	return (r);
}

struct state_value *
state_snapshot_style(const struct cow_style *style)
{
	struct state_value *record = state_value_record();
	snapshot_policy(record, &style->policy);
	rec_set_string(record, "body", style->body != NULL ? style->body : "");
	return (record);
}

static struct state_value *
menu_style_snapshot(const struct cow_menu *menu)
{
	struct cow_menu_style style;
	struct state_value *record = state_value_record();
	struct state_value *gradient;

	cow_menu_style_resolve(menu, &style);
	rec_set_colour(record, "bg", style.bg);
	rec_set_colour(record, "fg", style.fg);
	rec_set_colour(record, "title_bg", style.title_bg);
	rec_set_colour(record, "title_fg", style.title_fg);
	rec_set_colour(record, "separator", style.separator);
	rec_set_colour(record, "border", style.border);
	gradient = gradient_snapshot(&style.bg_gradient);
	state_record_set(record, "bg_gradient", gradient);
	state_value_unref(gradient);
	gradient = gradient_snapshot(&style.title_bg_gradient);
	state_record_set(record, "title_bg_gradient", gradient);
	state_value_unref(gradient);
	rec_set_number(record, "border_width", style.border_width);
	rec_set_number(record, "padding_x", style.padding_x);
	rec_set_number(record, "padding_y", style.padding_y);
	rec_set_number(record, "icon_size", style.icon_size);
	rec_set_number(record, "icon_gap", style.icon_gap);
	rec_set_string(record, "icon_path", style.icon_path);
	rec_set_number(record, "side_width", style.side_width);
	rec_set_colour(record, "side_colour", style.side_colour);
	rec_set_string(record, "side_image", style.side_image);
	rec_set_string(record, "font", style.font);
	rec_set_number(record, "title_underlines", style.title_underlines);
	rec_set_bool(record, "automatic_hotkeys", style.automatic_hotkeys);
	rec_set_string(record, "selection_relief",
	    style.selection_relief == COW_MENU_SELECTION_RELIEF_RAISED ?
	    "raised" :
	    style.selection_relief == COW_MENU_SELECTION_RELIEF_SUNKEN ?
	    "sunken" : "none");
	rec_set_string(record, "border_relief",
	    style.border_relief == COW_MENU_RELIEF_RAISED ? "raised" :
	    style.border_relief == COW_MENU_RELIEF_SUNKEN ? "sunken" :
	    "flat");
	return (record);
}

struct state_value *
state_snapshot_menu(const struct cow_menu *menu)
{
	struct state_value *record = state_value_record();
	struct state_value *items = state_value_list();
	struct state_value *style = menu_style_snapshot(menu);

	rec_set_string(record, "name", menu->name);
	rec_set_bool(record, "numbered", menu->numbered);
	rec_set_bool(record, "typeahead", menu->typeahead);
	rec_set_bool(record, "multi_select", menu->multi_select);
	rec_set_number(record, "style_overrides", menu->style_overrides);
	state_record_set(record, "style", style);
	state_value_unref(style);

	struct cow_menu_item *item;
	wl_list_for_each(item, &menu->items, link)
	{
		struct state_value *entry = state_value_record();
		const char *type = item->type == COW_MENU_ITEM_SEPARATOR ?
		    "separator" : item->type == COW_MENU_ITEM_TITLE ? "title" :
		    item->type == COW_MENU_ITEM_ACCEPT ? "accept" : "normal";

		rec_set_string(entry, "type", type);
		rec_set_string(entry, "label", item->label);
		rec_set_string(entry, "action", item->action);
		rec_set_string(entry, "icon", item->icon);
		rec_set_bool(entry, "popup", item->is_popup);
		rec_set_bool(entry, "popup_pick_context",
		    item->popup_pick_context);
		state_list_append(items, entry);
		state_value_unref(entry);
	}
	state_record_set(record, "items", items);
	state_value_unref(items);
	return (record);
}

struct state_value *
state_snapshot_menu_style(void)
{
	return (menu_style_snapshot(NULL));
}

static struct state_value *
bindings_snapshot(void)
{
	struct state_value *list = state_value_list();
	struct cow_binding_entry *key;
	wl_list_for_each(key, &cow_config->keyboard_bindings, link)
	{
		struct state_value *record = state_value_record();
		rec_set_string(record, "type", "keyboard");
		rec_set_string(record, "trigger", key->key_string);
		rec_set_string(record, "action", key->action_string);
		rec_set_string(record, "release_action",
		    key->release_action_string);
		rec_set_bool(record, "repeatable", key->repeatable);
		state_list_append(list, record);
		state_value_unref(record);
	}
	struct cow_mouse_binding_entry *mouse;
	wl_list_for_each(mouse, &cow_config->mouse_bindings, link)
	{
		struct state_value *record = state_value_record();
		rec_set_string(record, "type", "mouse");
		rec_set_string(record, "trigger", mouse->key_string);
		rec_set_string(record, "action", mouse->action_string);
		state_list_append(list, record);
		state_value_unref(record);
	}
	struct cow_dec_binding_entry *decoration;
	wl_list_for_each(decoration, &cow_config->decoration_bindings, link)
	{
		struct state_value *record = state_value_record();
		rec_set_string(record, "type", "decoration");
		rec_set_string(record, "region",
		    cow_dec_region_name(decoration->region));
		rec_set_number(record, "clicks", decoration->clicks);
		rec_set_string(record, "action", decoration->action_string);
		state_list_append(list, record);
		state_value_unref(record);
	}
	return (list);
}

static struct state_value *
commands_snapshot(void)
{
	struct state_value *list = state_value_list();

	for (struct cmd_entry **entry = cmd_table; *entry != NULL; entry++) {
		if (((*entry)->flags & CMD_MODULE) != 0)
			continue;
		struct state_value *record = state_value_record();
		rec_set_string(record, "name", (*entry)->name);
		rec_set_string(record, "usage", (*entry)->usage);
		state_list_append(list, record);
		state_value_unref(record);
	}
	const struct wl_list *head = cow_aliases_head();
	if (head != NULL) {
		struct cow_alias *alias;
		wl_list_for_each(alias, head, link)
		{
			struct state_value *record = state_value_record();
			rec_set_string(record, "name", alias->name);
			rec_set_string(record, "usage", alias->command);
			rec_set_bool(record, "alias", true);
			state_list_append(list, record);
			state_value_unref(record);
		}
	}
	return (list);
}

struct state_value *
state_snapshot_alias(const struct cow_alias *alias)
{
	struct state_value *record = state_value_record();
	rec_set_string(record, "name", alias->name);
	rec_set_string(record, "command", alias->command);
	return (record);
}

struct state_value *
state_snapshot_collection(enum state_source src)
{
	struct state_value *list = state_value_list();

	switch (src) {
	case STATE_SRC_WINDOWS: {
		struct cow_view *vw;
		wl_list_for_each(vw, &cow_wm.views, wm_link)
		{
			if (vw->closed)
				continue;
			struct state_value *r = state_snapshot_view(vw);
			state_list_append(list, r);
			state_value_unref(r);
		}
		break;
	}
	case STATE_SRC_DESKS: {
		struct cow_output *out;
		wl_list_for_each(out, &cow_wm.outputs, wm_link)
		{
			struct cow_desk *desk;
			wl_list_for_each(desk, &out->desk_set.desks, set_link)
			{
				struct state_value *r = state_snapshot_desk(desk);
				state_list_append(list, r);
				state_value_unref(r);
			}
		}
		break;
	}
	case STATE_SRC_OUTPUTS: {
		struct cow_output *out;
		wl_list_for_each(out, &cow_wm.outputs, wm_link)
		{
			struct state_value *r = state_snapshot_output(out);
			state_list_append(list, r);
			state_value_unref(r);
		}
		break;
	}
	case STATE_SRC_RULES: {
		const struct wl_list *head = cow_rules_head();
		if (head != NULL) {
			struct cow_rule *rule;
			wl_list_for_each(rule, head, link)
			{
				struct state_value *r = state_snapshot_rule(rule);
				state_list_append(list, r);
				state_value_unref(r);
			}
		}
		break;
	}
	case STATE_SRC_STYLES: {
		const struct wl_list *head = cow_styles_head();
		if (head != NULL) {
			struct cow_style *style;
			wl_list_for_each(style, head, link)
			{
				struct state_value *r = state_snapshot_style(style);
				state_list_append(list, r);
				state_value_unref(r);
			}
		}
		break;
	}
	case STATE_SRC_DECOR:
		state_value_unref(list);
		return (cow_decor_snapshot_all());
	case STATE_SRC_MENUS: {
		struct cow_menu *menu;
		wl_list_for_each(menu, &cow_wm.menus, link)
		{
			struct state_value *r = state_snapshot_menu(menu);
			state_list_append(list, r);
			state_value_unref(r);
		}
		break;
	}
	case STATE_SRC_BINDINGS:
		state_value_unref(list);
		return (bindings_snapshot());
	case STATE_SRC_COMMANDS:
		state_value_unref(list);
		return (commands_snapshot());
	case STATE_SRC_SELECTIONS: {
		struct cow_selection_summary *summaries;
		size_t count = cow_selection_summaries(&summaries);
		for (size_t i = 0; i < count; i++) {
			struct state_value *record = state_value_record();
			rec_set_string(record, "name", summaries[i].name);
			rec_set_number(record, "count", summaries[i].count);
			state_list_append(list, record);
			state_value_unref(record);
		}
		cow_selection_summaries_free(summaries, count);
		break;
	}
	case STATE_SRC_ALIASES: {
		const struct wl_list *head = cow_aliases_head();
		if (head != NULL) {
			struct cow_alias *alias;
			wl_list_for_each(alias, head, link)
			{
				struct state_value *record =
				    state_snapshot_alias(alias);
				state_list_append(list, record);
				state_value_unref(record);
			}
		}
		break;
	}
	}

	return (list);
}
