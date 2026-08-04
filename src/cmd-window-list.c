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
#include <string.h>

#include "cow/cmd.h"
#include "cow/cmd-filter.h"
#include "cow/compat.h"
#include "cow/decoration.h"
#include "cow/desk_ops.h"
#include "cow/format.h"
#include "cow/memory.h"
#include "cow/menu.h"
#include "cow/output.h"
#include "cow/view.h"
#include "cow/wm.h"

#define WINDOW_LIST_MENU "\037cow-window-list"

static enum cmd_retval cmd_window_list_exec(struct cmd *, struct cmd_q *);

struct cmd_entry cmd_window_list_entry = {
	"window-list",
	"F:f:s:w",
	0,
	0,
	"window-list [-w] [-F expr] [-f format]",
	cmd_window_list_exec,
	0,
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
	struct window_list_format_ctx *ctx = data;
	const struct cow_view *view = ctx->view;

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

	cow_format_parse(format, "ntTaioOdDxywhl", true,
	    window_list_format_text, window_list_format_conversion, &ctx, NULL);
	if (ctx.buf == NULL)
		ctx.buf = xstrdup("");
	xfree(ctx.display_title);
	return (ctx.buf);
}

/* Menu labels use '&' as markup, so quote literal ampersands in titles. */
static char *
escape_label(const char *src)
{
	if (src == NULL)
		return (xstrdup(""));
	size_t len = strlen(src);
	char  *dst = xcalloc(len * 2 + 1, 1);
	char  *d   = dst;
	for (const char *s = src; *s != '\0'; s++) {
		*d++ = *s;
		if (*s == '&')
			*d++ = '&';
	}
	return (dst);
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

	if (view->output != NULL) {
		cow_output_set_current(view->output);
		if (view->desk != NULL &&
		    !(view->flags & COW_VIEW_STICKY_DESK_FLAG))
			cow_desk_switch(view->output, view->desk->nr);
		if (!(view->flags & COW_VIEW_STICKY_PAGE_FLAG))
			cow_page_switch(view->output, cow_view_page_col(view),
			    cow_view_page_row(view));
	}
	if (cow_view_is_iconified(view))
		cow_view_deiconify(view);
	if (view->container_member != NULL)
		cow_container_activate(view->container_member->container,
		    view->container_member);
	cow_wm.pending_focus = view;
	cow_wm_request_manage_dirty();
	if (warp)
		cow_view_warp_to_focused();
	return (true);
}

static enum cmd_retval
show_window_list(const char *filter_expr, const char *format, bool warp,
    struct cmd_q *cmdq)
{
	if (format != NULL) {
		char *cause = NULL;
		if (!cow_format_parse(format, "ntTaioOdDxywhl", true, NULL,
			NULL, NULL, &cause)) {
			cmdq_error(cmdq, "window-list: %s", cause);
			xfree(cause);
			return (CMD_RETURN_ERROR);
		}
	}

	struct cmd_filter filter;
	if (!cmd_filter_parse(&filter, cmdq, "window-list", "-F", filter_expr))
		return (CMD_RETURN_ERROR);

	struct cow_menu *old = cow_menu_find(WINDOW_LIST_MENU);
	if (old != NULL)
		cow_menu_destroy(old);
	struct cow_menu *menu = cow_menu_create(WINDOW_LIST_MENU);
	if (menu == NULL) {
		cmd_filter_free(&filter);
		cmdq_error(cmdq, "window-list: cannot create menu");
		return (CMD_RETURN_ERROR);
	}
	cow_menu_enable_typeahead(menu);
	cow_menu_item_add(menu, COW_MENU_ITEM_TITLE,
	    "Window List (type to search)", NULL, false, false);

	int		 number = 0;
	struct cow_view *view;
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

		number++;
		char *label;
		if (format != NULL) {
			label = format_label(format, view, number);
		} else {
			char	   *title = cow_decoration_display_title(view);
			const char *shown = title != NULL && title[0] != '\0' ?
			    title :
			    view->title != NULL && view->title[0] != '\0' ?
			    view->title :
			    view->app_id != NULL && view->app_id[0] != '\0' ?
			    view->app_id :
			    "(untitled)";
			char	   *safe_title = escape_label(shown);
			char	   *safe_app   = escape_label(view->app_id);
			char	    prefix[32];
			if (number < 10)
				snprintf(prefix, sizeof(prefix), "&%d", number);
			else if (number == 10)
				strlcpy(prefix, "&0", sizeof(prefix));
			else
				snprintf(prefix, sizeof(prefix), "%d", number);

			if (safe_app[0] != '\0' &&
			    strcmp(shown, view->app_id) != 0)
				xasprintf(&label, "%s  %s  [%s]", prefix,
				    safe_title, safe_app);
			else
				xasprintf(&label, "%s  %s", prefix, safe_title);
			xfree(safe_app);
			xfree(safe_title);
			xfree(title);
		}
		char *action;
		xasprintf(&action, "window-list -s %s%s", view->identifier,
		    warp ? " -w" : "");
		cow_menu_item_add(menu, COW_MENU_ITEM_NORMAL, label, action,
		    false, false);
		xfree(action);
		xfree(label);
	}

	bool filter_ok = cmd_filter_finish(&filter, cmdq);
	cmd_filter_free(&filter);
	if (!filter_ok) {
		cow_menu_destroy(menu);
		return (CMD_RETURN_ERROR);
	}
	if (number == 0) {
		cow_menu_destroy(menu);
		cmdq_error(cmdq, "window-list: no matching windows");
		return (CMD_RETURN_ERROR);
	}

	struct cow_output *output = cmd_ctx_target_output(cmdq);
	if (output == NULL)
		output = cow_wm.current_output;
	int cx = 0, cy = 0;
	if (output != NULL) {
		cx = output->usable_area.x + output->usable_area.width / 2;
		cy = output->usable_area.y + output->usable_area.height / 2;
	}
	cow_menu_popup_open_centered(menu, cx, cy, NULL);
	cow_wm.needs_manage_dirty = true;
	return (CMD_RETURN_NORMAL);
}

static enum cmd_retval
cmd_window_list_exec(struct cmd *self, struct cmd_q *cmdq)
{
	const char *id	 = args_get(self->args, 's');
	bool	    warp = args_has(self->args, 'w');
	if (id != NULL) {
		if (args_get(self->args, 'F') != NULL ||
		    args_get(self->args, 'f') != NULL) {
			cmdq_error(cmdq,
			    "window-list: -F and -f cannot be used with -s");
			return (CMD_RETURN_ERROR);
		}
		return (select_window(id, warp, cmdq) ? CMD_RETURN_NORMAL :
							CMD_RETURN_ERROR);
	}
	return (show_window_list(args_get(self->args, 'F'),
	    args_get(self->args, 'f'), warp, cmdq));
}
