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

/* A transient, FVWM-style numbered window list. */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cow/cmd.h"
#include "cow/cmd-filter.h"
#include "cow/compat.h"
#include "cow/decoration.h"
#include "cow/desk_ops.h"
#include "cow/format.h"
#include "cow/memory.h"
#include "cow/menu.h"
#include "cow/output.h"
#include "cow/seat.h"
#include "cow/view.h"
#include "cow/update.h"
#include "cow/wm.h"

#define WINDOW_LIST_MENU "\037cow-window-list"
#define WINDOW_LIST_DEFAULT_FORMAT "%o:%d %a - %T"

static enum cmd_retval cmd_window_list_exec(struct cmd *, struct cmd_q *);

struct cmd_entry cmd_window_list_entry = {
	"window-list",
	"F:IS:f:nps:w",
	0,
	0,
	"window-list [-I] [-n] [-p] [-w] [-F expr] [-f format] [-S sort]",
	cmd_window_list_exec,
	0,
};

enum window_list_sort {
	WINDOW_LIST_SORT_NONE,
	WINDOW_LIST_SORT_TITLE,
	WINDOW_LIST_SORT_APP,
	WINDOW_LIST_SORT_ID,
	WINDOW_LIST_SORT_OUTPUT,
	WINDOW_LIST_SORT_DESK,
	WINDOW_LIST_SORT_LOCATION,
};

struct window_list_entry {
	struct cow_view *view;
};

struct window_list_format_ctx {
	char		      *buf;
	size_t		       cap;
	size_t		       len;
	const struct cow_view *view;
	int		       number;
	char		      *display_title;
};

static void
label_append(struct window_list_format_ctx *ctx, const char *src, size_t srclen,
    bool escape_markup)
{
	if (src == NULL)
		return;

	for (size_t i = 0; i < srclen; i++) {
		if (ctx->cap <
		    ctx->len + (escape_markup && src[i] == '&' ? 3 : 2)) {
			size_t ncap = ctx->cap == 0 ? 64 : ctx->cap * 2;
			ctx->buf    = xrealloc(ctx->buf, ncap, 1);
			ctx->cap    = ncap;
		}
		ctx->buf[ctx->len++] = src[i];
		if (escape_markup && src[i] == '&')
			ctx->buf[ctx->len++] = '&';
		ctx->buf[ctx->len] = '\0';
	}
}

static void
label_append_string(struct window_list_format_ctx *ctx, const char *src,
    bool escape_markup)
{
	label_append(ctx, src, src != NULL ? strlen(src) : 0, escape_markup);
}

static void
label_append_number(struct window_list_format_ctx *ctx, int value)
{
	char number[32];

	snprintf(number, sizeof(number), "%d", value);
	label_append_string(ctx, number, true);
}

static void
window_list_format_text(void *data, const char *text, size_t len)
{
	label_append(data, text, len, true);
}

static const char *
window_list_display_title(const struct window_list_format_ctx *ctx)
{
	if (ctx->display_title != NULL && ctx->display_title[0] != '\0')
		return (ctx->display_title);
	if (ctx->view->title != NULL && ctx->view->title[0] != '\0')
		return (ctx->view->title);
	if (ctx->view->app_id != NULL && ctx->view->app_id[0] != '\0')
		return (ctx->view->app_id);
	return ("(untitled)");
}

static void
window_list_format_conversion(void *data, char conversion)
{
	struct window_list_format_ctx *ctx  = data;
	const struct cow_view	      *view = ctx->view;

	switch (conversion) {
	case 'n': {
		char shortcut[32];
		if (ctx->number < 10)
			snprintf(shortcut, sizeof(shortcut), "&%d",
			    ctx->number);
		else if (ctx->number == 10)
			strlcpy(shortcut, "&0", sizeof(shortcut));
		else
			snprintf(shortcut, sizeof(shortcut), "%d", ctx->number);
		label_append_string(ctx, shortcut, false);
		break;
	}
	case 't':
		label_append_string(ctx, window_list_display_title(ctx), true);
		break;
	case 'T':
		label_append_string(ctx, view->title, true);
		break;
	case 'a':
		label_append_string(ctx, view->app_id, true);
		break;
	case 'i':
		label_append_string(ctx, view->identifier, true);
		break;
	case 'I':
		label_append(ctx, view->identifier,
		    view->identifier != NULL ? strnlen(view->identifier, 8) : 0,
		    true);
		break;
	case 'o':
		label_append_string(ctx,
		    view->output != NULL ? view->output->name : NULL, true);
		break;
	case 'O':
		if (view->output != NULL)
			label_append_number(ctx, view->output->number);
		break;
	case 'd':
		if (view->desk != NULL)
			label_append_number(ctx, view->desk->nr);
		break;
	case 'D':
		label_append_string(ctx,
		    view->desk != NULL ? view->desk->name : NULL, true);
		break;
	case 'x':
		label_append_number(ctx, view->x);
		break;
	case 'y':
		label_append_number(ctx, view->y);
		break;
	case 'w':
		label_append_number(ctx, view->content_width);
		break;
	case 'h':
		label_append_number(ctx, view->content_height);
		break;
	case 'l':
		label_append_number(ctx, view->layer);
		break;
	}
}

static char *
format_label(const char *format, const struct cow_view *view, int number)
{
	struct window_list_format_ctx ctx = {
		.view	       = view,
		.number	       = number,
		.display_title = cow_decoration_display_title(view),
	};

	cow_format_parse(format, "ntTaiIoOdDxywhl", true,
	    window_list_format_text, window_list_format_conversion, &ctx, NULL);
	if (ctx.buf == NULL)
		ctx.buf = xstrdup("");
	xfree(ctx.display_title);
	return (ctx.buf);
}

static bool
parse_sort(const char *value, enum window_list_sort *sort, struct cmd_q *cmdq)
{
	if (value == NULL || strcmp(value, "none") == 0)
		*sort = WINDOW_LIST_SORT_NONE;
	else if (strcmp(value, "title") == 0)
		*sort = WINDOW_LIST_SORT_TITLE;
	else if (strcmp(value, "app") == 0)
		*sort = WINDOW_LIST_SORT_APP;
	else if (strcmp(value, "id") == 0)
		*sort = WINDOW_LIST_SORT_ID;
	else if (strcmp(value, "output") == 0)
		*sort = WINDOW_LIST_SORT_OUTPUT;
	else if (strcmp(value, "desk") == 0)
		*sort = WINDOW_LIST_SORT_DESK;
	else if (strcmp(value, "location") == 0)
		*sort = WINDOW_LIST_SORT_LOCATION;
	else {
		cmdq_error(cmdq, "window-list: invalid sort: %s", value);
		return (false);
	}
	return (true);
}

static int
compare_text(const char *a, const char *b)
{
	return (strcasecmp(a != NULL ? a : "", b != NULL ? b : ""));
}

static int
compare_number(int a, int b)
{
	return (a > b) - (a < b);
}

static int
compare_views(const struct cow_view *a, const struct cow_view *b,
    enum window_list_sort sort)
{
	int cmp;

	switch (sort) {
	case WINDOW_LIST_SORT_TITLE:
		return (compare_text(a->title, b->title));
	case WINDOW_LIST_SORT_APP:
		return (compare_text(a->app_id, b->app_id));
	case WINDOW_LIST_SORT_ID:
		return (compare_text(a->identifier, b->identifier));
	case WINDOW_LIST_SORT_OUTPUT:
		return (
		    compare_number(a->output != NULL ? a->output->number : 0,
			b->output != NULL ? b->output->number : 0));
	case WINDOW_LIST_SORT_DESK:
		return (compare_number(a->desk != NULL ? a->desk->nr : -1,
		    b->desk != NULL ? b->desk->nr : -1));
	case WINDOW_LIST_SORT_LOCATION:
		cmp = compare_views(a, b, WINDOW_LIST_SORT_OUTPUT);
		if (cmp == 0)
			cmp = compare_views(a, b, WINDOW_LIST_SORT_DESK);
		if (cmp == 0)
			cmp = compare_views(a, b, WINDOW_LIST_SORT_APP);
		if (cmp == 0)
			cmp = compare_views(a, b, WINDOW_LIST_SORT_TITLE);
		return (cmp);
	case WINDOW_LIST_SORT_NONE:
		return (0);
	}
	return (0);
}

static void
sort_entries(struct window_list_entry *entries, size_t count,
    enum window_list_sort sort)
{
	/* Insertion sort preserves CoW's order among equal keys. */
	for (size_t i = 1; i < count; i++) {
		struct window_list_entry entry = entries[i];
		size_t			 j     = i;
		while (j > 0 &&
		    compare_views(entries[j - 1].view, entry.view, sort) > 0) {
			entries[j] = entries[j - 1];
			j--;
		}
		entries[j] = entry;
	}
}

static bool
select_window(const char *id, bool warp, struct cmd_q *cmdq)
{
	struct cow_view *view = cow_view_find_by_identifier(id);
	if (view == NULL || view->closed) {
		cmdq_error(cmdq, "window-list: window no longer exists: %s",
		    id);
		return (false);
	}

	cow_view_reveal_location(view);
	if (cow_view_is_iconified(view))
		cow_view_deiconify(view);
	if (view->container_member != NULL)
		cow_container_activate(view->container_member->container,
		    view->container_member);
	cow_wm.pending_focus = view;
	cow_update_request(COW_UPDATE_MANAGE);
	if (warp)
		cow_view_warp_to_focused();
	return (true);
}

/* Index a filtered, sorted list without changing its presentation order.
 * The root groups by container; submenus group by pane. Resolve ancestry
 * once per entry, rather than again for every pane in the layout.
 */
struct window_list_group_entry {
	const void	*group;
	struct cow_view *member;
	size_t		 index;
};

static int
compare_group_entries(const void *a, const void *b)
{
	const struct window_list_group_entry *ea = a, *eb = b;
	uintptr_t ga = (uintptr_t)ea->group, gb = (uintptr_t)eb->group;
	if (ga != gb)
		return ga < gb ? -1 : 1;
	return (ea->index > eb->index) - (ea->index < eb->index);
}

static struct window_list_group_entry *
group_entries(struct window_list_entry *entries, size_t count,
    struct cow_container *c)
{
	struct window_list_group_entry *groups = xcalloc(count,
	    sizeof(*groups));
	for (size_t i = 0; i < count; i++) {
		struct cow_view *member = entries[i].view;
		if (c == NULL) {
			groups[i].group = cow_container_top_group(member);
		} else {
			while (member && cow_view_container(member) != c) {
				struct cow_container *owner =
				    cow_container_group(member);
				struct cow_view *parent = owner ?
				    owner->frame_view :
				    NULL;
				member = parent != member ? parent : NULL;
			}
			groups[i].group = member ? cow_view_pane(member) : NULL;
		}
		groups[i].member = member;
		groups[i].index	 = i;
	}
	qsort(groups, count, sizeof(*groups), compare_group_entries);
	return groups;
}

static size_t
group_start(const struct window_list_group_entry *groups, size_t count,
    const void *group)
{
	size_t lo = 0, hi = count;
	while (lo < hi) {
		size_t mid = lo + (hi - lo) / 2;
		if ((uintptr_t)groups[mid].group < (uintptr_t)group)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo;
}

static struct window_list_entry *
group_windows(const struct window_list_entry *entries,
    const struct window_list_group_entry *groups, size_t count, size_t first,
    const struct cow_view *member, size_t *length)
{
	size_t end = first;
	while (end < count && groups[end].group == groups[first].group)
		end++;
	struct window_list_entry *subset = xcalloc(end - first,
	    sizeof(*subset));
	*length				 = 0;
	for (size_t i = first; i < end; i++) {
		if (member == NULL || groups[i].member == member)
			subset[(*length)++] = entries[groups[i].index];
	}
	return subset;
}

static struct cow_menu_item *
add_window(struct cow_menu *menu, const struct cow_view *view,
    const char *format, int number, bool icons, bool warp, const char *path)
{
	char *label = format_label(format, view, number);
	if (path != NULL) {
		struct window_list_format_ctx ctx = { 0 };
		label_append_string(&ctx, path, true);
		label_append_string(&ctx, " / ", true);
		label_append_string(&ctx, label, false);
		xfree(label);
		label = ctx.buf;
	}
	char *action;
	xasprintf(&action, "window-list -s %s%s", view->identifier,
	    warp ? " -w" : "");
	struct cow_menu_item *item = cow_menu_item_add(menu,
	    COW_MENU_ITEM_NORMAL, label, action, false, false,
	    icons ? cow_view_icon_image(view) : NULL);
	xfree(action);
	xfree(label);
	return item;
}

static struct cow_menu *
container_menu(struct cow_container *c, struct window_list_entry *entries,
    size_t count, const char *format, bool icons, bool typeahead, bool warp,
    unsigned depth)
{
	if (depth > 64)
		return NULL;
	char name[80];
	snprintf(name, sizeof(name), WINDOW_LIST_MENU ":%d", c->id);
	struct cow_menu *menu = cow_menu_find(name);
	if (menu != NULL)
		return menu;
	menu = cow_menu_generated_begin(name, c->name ? c->name : "Container",
	    true, typeahead);
	int number = 0;
	/* Keep the outer frame selectable, but only if it passed the filter.
	 */
	for (size_t i = 0; i < count; i++) {
		if (entries[i].view == c->frame_view)
			add_window(menu, c->frame_view, format, ++number, icons,
			    warp, "Container");
	}
	struct cow_container_pane *pane = NULL;
	struct window_list_group_entry *groups = group_entries(entries, count,
	    c);
	while ((pane = cow_container_pane_next(c, pane)) != NULL) {
		bool heading = false;
		for (size_t i = group_start(groups, count, pane);
		    i < count && groups[i].group == pane; i++) {
			struct cow_view *view	= entries[groups[i].index].view;
			struct cow_view *member = groups[i].member;
			if (!heading) {
				char *title;
				xasprintf(&title, "Pane %d", pane->id);
				struct window_list_format_ctx label = { 0 };
				label_append_string(&label,
				    pane->name ? pane->name : title, true);
				cow_menu_item_add(menu, COW_MENU_ITEM_TITLE,
				    label.buf, NULL, false, false, NULL);
				xfree(label.buf);
				xfree(title);
				heading = true;
			}
			struct cow_container *nested =
			    cow_container_window_owner(member);
			if (nested != NULL) {
				char child_name[80];
				snprintf(child_name, sizeof(child_name),
				    WINDOW_LIST_MENU ":%d", nested->id);
				/* Filtered descendants can introduce the same
				 * branch.
				 */
				if (cow_menu_find(child_name))
					continue;
				size_t			  length;
				struct window_list_entry *subset =
				    group_windows(entries, groups, count, i,
					member, &length);
				struct cow_menu *child = container_menu(nested,
				    subset, length, format, icons, typeahead,
				    warp, depth + 1);
				xfree(subset);
				if (child) {
					struct window_list_format_ctx label = {
						0
					};
					label_append_string(&label,
					    nested->name ? nested->name :
							   "Container",
					    true);
					cow_menu_item_add(menu,
					    COW_MENU_ITEM_NORMAL, label.buf,
					    child->name, true, false, NULL);
					xfree(label.buf);
					number++;
				}
			} else {
				add_window(menu, view, format, ++number, icons,
				    warp, NULL);
			}
		}
	}
	xfree(groups);
	return menu;
}

static enum cmd_retval
show_window_list(const char *filter_expr, const char *format,
    const char *sort_value, bool icons, bool typeahead, bool at_pointer,
    bool warp, struct cmd_q *cmdq)
{
	enum window_list_sort sort;
	if (!parse_sort(sort_value, &sort, cmdq))
		return (CMD_RETURN_ERROR);
	if (format == NULL)
		format = WINDOW_LIST_DEFAULT_FORMAT;
	char *cause = NULL;
	if (!cow_format_parse(format, "ntTaiIoOdDxywhl", true, NULL, NULL, NULL,
		&cause)) {
		cmdq_error(cmdq, "window-list: %s", cause);
		xfree(cause);
		return (CMD_RETURN_ERROR);
	}

	struct cmd_filter filter;
	if (!cmd_filter_parse(&filter, cmdq, "window-list", "-F", filter_expr))
		return (CMD_RETURN_ERROR);

	struct window_list_entry *entries = NULL;
	size_t			  count	  = 0;
	struct cow_view		 *view;
	wl_list_for_each(view, &cow_wm.views, wm_link)
	{
		if (view->closed || !view->has_dimensions ||
		    view->identifier == NULL)
			continue;
		bool matches = false;
		if (!cmd_filter_view(&filter, view, &matches))
			break;
		if (!matches)
			continue;

		entries = xrealloc(entries, count + 1, sizeof(*entries));
		entries[count++].view = view;
	}

	bool filter_ok = cmd_filter_finish(&filter, cmdq);
	cmd_filter_free(&filter);
	if (!filter_ok) {
		xfree(entries);
		return (CMD_RETURN_ERROR);
	}
	if (count == 0) {
		xfree(entries);
		cmdq_error(cmdq, "window-list: no matching windows");
		return (CMD_RETURN_ERROR);
	}

	sort_entries(entries, count, sort);
	struct cow_menu *old, *next;
	wl_list_for_each_safe(old, next, &cow_wm.menus, link)
	{
		if (strcmp(old->name, WINDOW_LIST_MENU) == 0 ||
		    strncmp(old->name, WINDOW_LIST_MENU ":",
			strlen(WINDOW_LIST_MENU ":")) == 0)
			cow_menu_destroy(old);
	}

	struct cow_menu *menu = cow_menu_generated_begin(WINDOW_LIST_MENU,
	    typeahead ? "Window List (type to search)" : "Window List", true,
	    typeahead);
	if (menu == NULL) {
		xfree(entries);
		cmdq_error(cmdq, "window-list: cannot create menu");
		return (CMD_RETURN_ERROR);
	}
	struct window_list_group_entry *groups = group_entries(entries, count,
	    NULL);
	for (size_t i = 0; i < count; i++) {
		view			= entries[i].view;
		struct cow_container *c = cow_container_top_group(view);
		if (c == NULL) {
			add_window(menu, view, format, (int)i + 1, icons, warp,
			    NULL);
			continue;
		}
		size_t	    first = group_start(groups, count, c);
		const char *title = c->name ? c->name :
		    c->frame_view && c->frame_view->title ?
					      c->frame_view->title :
					      "Container";
		if (groups[first].index == i) {
			size_t			  length;
			struct window_list_entry *subset = group_windows(
			    entries, groups, count, first, NULL, &length);
			struct cow_menu *child = container_menu(c, subset,
			    length, format, icons, typeahead, warp, 0);
			xfree(subset);
			if (child == NULL)
				continue;
			struct window_list_format_ctx label = { 0 };
			label_append_string(&label, title, true);
			int windows = 0, panes = 0;
			for (size_t j = first;
			    j < count && groups[j].group == c; j++) {
				if (!cow_container_window_view(
					groups[j].member))
					windows++;
			}
			struct cow_container_pane *pane = NULL;
			while (
			    (pane = cow_container_pane_next(c, pane)) != NULL)
				panes++;
			char *summary;
			xasprintf(&summary, " (%d panes, %d windows)", panes,
			    windows);
			label_append_string(&label, summary, true);
			struct cow_menu_item *item = cow_menu_item_add(menu,
			    COW_MENU_ITEM_NORMAL, label.buf, child->name, true,
			    false, NULL);
			item->browse_only	   = true;
			xfree(summary);
			xfree(label.buf);
		}
		if (typeahead) {
			struct cow_container_pane *pane = cow_view_pane(view);
			char			  *path;
			if (cow_container_window_owner(view))
				path = xstrdup(title);
			else if (pane && pane->name)
				xasprintf(&path, "%s / %s", title, pane->name);
			else
				xasprintf(&path, "%s / Pane %d", title,
				    pane ? pane->id : -1);
			struct cow_menu_item *item = add_window(menu, view,
			    format, (int)i + 1, icons, warp, path);
			item->search_only	   = true;
			xfree(path);
		}
	}
	xfree(groups);
	xfree(entries);

	if (at_pointer) {
		int x = 0, y = 0;

		if (!cow_seat_get_pointer(&x, &y) && cow_wm.has_raw_pointer) {
			x = cow_wm.raw_pointer_x;
			y = cow_wm.raw_pointer_y;
		}
		cow_menu_popup_open(menu, x, y, NULL, NULL, true);
	} else {
		struct cow_output *output = cmd_ctx_target_output(cmdq);
		if (output == NULL)
			output = cow_wm.current_output;
		int cx = 0, cy = 0;
		if (output != NULL) {
			cx = output->usable_area.x + output->usable_area.width / 2;
			cy = output->usable_area.y + output->usable_area.height / 2;
		}
		cow_menu_popup_open_centered(menu, cx, cy, NULL);
	}
	cow_update_request(COW_UPDATE_MANAGE);
	return (CMD_RETURN_NORMAL);
}

static enum cmd_retval
cmd_window_list_exec(struct cmd *self, struct cmd_q *cmdq)
{
	const char *id	 = args_get(self->args, 's');
	bool	    icons = args_has(self->args, 'I');
	bool	    typeahead = !args_has(self->args, 'n');
	bool	    at_pointer = args_has(self->args, 'p');
	bool	    warp = args_has(self->args, 'w');
	if (id != NULL) {
		if (args_get(self->args, 'F') != NULL || icons || at_pointer ||
		    args_get(self->args, 'f') != NULL ||
		    args_get(self->args, 'S') != NULL ||
		    args_has(self->args, 'n')) {
			cmdq_error(cmdq,
			    "window-list: -F, -f, -I, -n, -p, and -S cannot "
			    "be used with -s");
			return (CMD_RETURN_ERROR);
		}
		return (select_window(id, warp, cmdq) ? CMD_RETURN_NORMAL :
							CMD_RETURN_ERROR);
	}
	return (show_window_list(args_get(self->args, 'F'),
	    args_get(self->args, 'f'), args_get(self->args, 'S'), icons,
	    typeahead, at_pointer, warp, cmdq));
}
