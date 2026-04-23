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
 * Command context: parse target/source specifiers of the form
 * [output]:[desk].[window] and resolve them to cow objects.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cow/cmd.h"
#include "cow/memory.h"
#include "cow/output.h"
#include "cow/view.h"
#include "cow/wm.h"
#include "cow/output.h"

/* ================================================================
 * Output resolution
 * ================================================================ */

static struct cow_output *
resolve_output(const char *spec, char **cause)
{
	if (spec == NULL || *spec == '\0' || (strcmp(spec, "@current") == 0))
		return cow_wm.current_output;

	if (strcmp(spec, "@next") == 0) {
		if (cow_wm.current_output == NULL)
			return NULL;
		struct wl_list *next = cow_wm.current_output->wm_link.next;
		if (next == &cow_wm.outputs)
			next = next->next;
		if (next == &cow_wm.outputs)
			return cow_wm.current_output;
		return wl_container_of(next, cow_wm.current_output, wm_link);
	}

	if (strcmp(spec, "@prev") == 0) {
		if (cow_wm.current_output == NULL)
			return NULL;
		struct wl_list *prev = cow_wm.current_output->wm_link.prev;
		if (prev == &cow_wm.outputs)
			prev = prev->prev;
		if (prev == &cow_wm.outputs)
			return cow_wm.current_output;
		return wl_container_of(prev, cow_wm.current_output, wm_link);
	}

	/* Match by name */
	struct cow_output *out;
	wl_list_for_each(out, &cow_wm.outputs, wm_link)
	{
		if (out->name != NULL && strcmp(out->name, spec) == 0)
			return out;
	}

	xasprintf(cause, "unknown output: %s", spec);
	return NULL;
}

/* ================================================================
 * Desk resolution
 * ================================================================ */

static struct cow_desk *
resolve_desk(const char *spec, struct cow_output *output, char **cause)
{
	if (output == NULL)
		return NULL;

	if (spec == NULL || *spec == '\0')
		return output->current_desk;

	if (strcmp(spec, "@next") == 0) {
		uint8_t nr = (output->current_desk->nr + 1) % COW_NR_OF_DESKS;
		return &output->desks[nr];
	}

	if (strcmp(spec, "@prev") == 0) {
		uint8_t nr = output->current_desk->nr;
		nr	   = (nr == 0) ? COW_NR_OF_DESKS - 1 : nr - 1;
		return &output->desks[nr];
	}

	/* Numeric desk number */
	char *end;
	long  nr = strtol(spec, &end, 10);
	if (end != spec && *end == '\0' && nr >= 0 && nr < COW_NR_OF_DESKS)
		return &output->desks[nr];

	xasprintf(cause, "unknown desk: %s", spec);
	return NULL;
}

/* ================================================================
 * Window resolution
 * ================================================================ */

static struct cow_view *
resolve_window(const char *spec, struct cow_output *output,
    struct cow_desk *desk, char **cause)
{
	if (spec == NULL || *spec == '\0')
		return cow_wm.focused_view;

	if (strcmp(spec, "@focused") == 0)
		return cow_wm.focused_view;

	if (strcmp(spec, "@next") == 0 || strcmp(spec, "@prev") == 0) {
		struct cow_view *focused = cow_wm.focused_view;
		if (focused == NULL || wl_list_empty(&cow_wm.views))
			return NULL;
		struct cow_view *v = cow_view_walk(focused,
		    strcmp(spec, "@next") == 0, cow_view_focusable);
		/*
		 * Behaviour is 'stay on current' rather than 'report none':
		 * historically this resolver has returned the focused view
		 * when no other focusable candidate exists.
		 */
		return v != NULL ? v : focused;
	}

	/* %app_id -- match by app_id */
	if (spec[0] == '%') {
		const char	*app_id = spec + 1;
		struct cow_view *v;
		wl_list_for_each(v, &cow_wm.views, wm_link)
		{
			if (!cow_view_focusable(v))
				continue;
			if (output != NULL && v->output != output)
				continue;
			if (desk != NULL && v->desk != desk)
				continue;
			if (v->app_id != NULL && strcmp(v->app_id, app_id) == 0)
				return v;
		}
		xasprintf(cause, "no window with app_id: %s", app_id);
		return NULL;
	}

	/* /title substring/ -- match by title */
	if (spec[0] == '/') {
		size_t len = strlen(spec);
		/* Strip trailing / if present */
		char *pattern;
		if (len > 1 && spec[len - 1] == '/') {
			pattern = xmalloc(len - 1);
			memcpy(pattern, spec + 1, len - 2);
			pattern[len - 2] = '\0';
		} else {
			pattern = xstrdup(spec + 1);
		}

		struct cow_view *v;
		wl_list_for_each(v, &cow_wm.views, wm_link)
		{
			if (!cow_view_focusable(v))
				continue;
			if (output != NULL && v->output != output)
				continue;
			if (desk != NULL && v->desk != desk)
				continue;
			if (v->title != NULL &&
			    strstr(v->title, pattern) != NULL) {
				xfree(pattern);
				return v;
			}
		}
		xasprintf(cause, "no window with title matching: %s", pattern);
		xfree(pattern);
		return NULL;
	}

	/* #identifier -- match by river window id */
	if (spec[0] == '#') {
		const char	*id = spec + 1;
		struct cow_view *v;
		wl_list_for_each(v, &cow_wm.views, wm_link)
		{
			if (!cow_view_focusable(v))
				continue;
			if (v->identifier != NULL &&
			    strcmp(v->identifier, id) == 0)
				return v;
		}
		xasprintf(cause, "no window with id: %s", id);
		return NULL;
	}

	xasprintf(cause, "invalid window selector: %s", spec);
	return NULL;
}

/* ================================================================
 * Main specifier parser
 * ================================================================ */

bool
cmd_ctx_parse(const char *spec, struct cmd_ctx *ctx, char **cause)
{
	*cause = NULL;
	memset(ctx, 0, sizeof(*ctx));

	if (spec == NULL || *spec == '\0') {
		/* Empty spec: all defaults */
		ctx->output = cow_wm.current_output;
		if (ctx->output != NULL)
			ctx->desk = ctx->output->current_desk;
		ctx->window = cow_wm.focused_view;
		return true;
	}

	/* Make a mutable copy */
	char *buf	 = xstrdup(spec);
	char *output_str = NULL;
	char *desk_str	 = NULL;
	char *window_str = NULL;

	/* Split on ':' and '.' */
	char *colon = strchr(buf, ':');
	char *dot   = NULL;

	if (colon != NULL) {
		*colon	   = '\0';
		output_str = buf;
		dot	   = strchr(colon + 1, '.');
		if (dot != NULL) {
			*dot	   = '\0';
			desk_str   = colon + 1;
			window_str = dot + 1;
		} else {
			desk_str = colon + 1;
		}
	} else {
		dot = strchr(buf, '.');
		if (dot != NULL) {
			*dot	   = '\0';
			desk_str   = buf;
			window_str = dot + 1;
		} else {
			/* No delimiters: check if it matches an output name
			 * first; if so treat it as an output specifier rather
			 * than a window selector. */
			struct cow_output *o;
			bool		   is_output = false;
			wl_list_for_each(o, &cow_wm.outputs, wm_link)
			{
				if (o->name != NULL &&
				    strcmp(o->name, buf) == 0) {
					is_output = true;
					break;
				}
			}
			if (is_output)
				output_str = buf;
			else
				window_str = buf;
		}
	}

	/* Resolve output */
	ctx->output = resolve_output(output_str, cause);
	if (*cause != NULL) {
		xfree(buf);
		return false;
	}

	/* Resolve desk */
	ctx->desk = resolve_desk(desk_str, ctx->output, cause);
	if (*cause != NULL) {
		xfree(buf);
		return false;
	}

	/* Resolve window */
	ctx->window = resolve_window(window_str, ctx->output, ctx->desk, cause);
	if (*cause != NULL) {
		xfree(buf);
		return false;
	}

	xfree(buf);
	return true;
}

/* ================================================================
 * Convenience accessors with defaults
 * ================================================================ */

struct cow_view *
cmd_ctx_target_window(struct cmd_q *cmdq)
{
	if (cmdq->target.window != NULL)
		return cmdq->target.window;
	return cow_wm.focused_view;
}

struct cow_output *
cmd_ctx_target_output(struct cmd_q *cmdq)
{
	if (cmdq->target.output != NULL)
		return cmdq->target.output;
	return cow_wm.current_output;
}

struct cow_desk *
cmd_ctx_target_desk(struct cmd_q *cmdq)
{
	if (cmdq->target.desk != NULL)
		return cmdq->target.desk;
	struct cow_output *out = cmd_ctx_target_output(cmdq);
	if (out != NULL)
		return out->current_desk;
	return NULL;
}
