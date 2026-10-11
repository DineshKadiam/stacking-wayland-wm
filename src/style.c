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

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "cow/cmd.h"
#include "cow/config.h"
#include "cow/compat.h"
#include "cow/decoration.h"
#include "cow/desk_ops.h"
#include "cow/ipc.h"
#include "cow/log.h"
#include "cow/memory.h"
#include "cow/output.h"
#include "cow/placement.h"
#include "cow/style.h"
#include "cow/update.h"
#include "cow/util.h"
#include "cow/view.h"
#include "cow/wm.h"

#include "decor-profile.h"
#include "config-serialize.h"
#include "state-snapshot.h"

static struct wl_list styles;
static bool styles_initialised;
static bool applying;
static bool applying_all;
static bool reapply_pending;

struct resolved_style {
	char *layer;
	char *circulate_skip;
	char *sticky_desk;
	char *sticky_page;
	char *output;
	char *desk;
	char *page;
	char *x;
	char *y;
	char *width;
	char *height;
	char *anchor;
	char *maximized;
	char *maximize_ignore_reserved;
	char *fullscreen;
	char *shaded;
	char *iconified;
	uint32_t live_mask;
};

static void
styles_lazy_init(void)
{
	if (!styles_initialised) {
		wl_list_init(&styles);
		styles_initialised = true;
	}
}

void
cow_style_init(void)
{
	styles_lazy_init();
}

void
cow_style_destroy(struct cow_style *style)
{
	if (style == NULL)
		return;
	struct cow_style_entry *entry, *tmp;
	wl_list_for_each_safe(entry, tmp, &style->entries, link)
	{
		wl_list_remove(&entry->link);
		xfree(entry->name);
		xfree(entry->value);
		xfree(entry);
	}
	cow_policy_fini(&style->policy);
	xfree(style->body);
	xfree(style->file);
	xfree(style);
}

void
cow_style_fini(void)
{
	if (!styles_initialised)
		return;
	struct cow_style *style, *tmp;
	wl_list_for_each_safe(style, tmp, &styles, link)
	{
		wl_list_remove(&style->link);
		cow_style_destroy(style);
	}
	styles_initialised = false;
}

struct cow_style *
cow_style_find_by_name(const char *name)
{
	styles_lazy_init();
	struct cow_style *style;
	wl_list_for_each(style, &styles, link)
	{
		if (style->policy.name != NULL && name != NULL &&
		    strcmp(style->policy.name, name) == 0)
			return (style);
	}
	return (NULL);
}

const struct wl_list *
cow_styles_head(void)
{
	styles_lazy_init();
	return (&styles);
}

void
cow_style_add(struct cow_style *style)
{
	styles_lazy_init();
	struct cow_style *old = cow_style_find_by_name(style->policy.name);
	if (old != NULL) {
		wl_list_insert(old->link.prev, &style->link);
		wl_list_remove(&old->link);
		cow_style_destroy(old);
	} else
		wl_list_insert(styles.prev, &style->link);
	if (cow_config != NULL && !cow_ipc.startup_mode)
		cow_style_apply_all_live();
}

bool
cow_style_remove_by_name(const char *name)
{
	struct cow_style *style = cow_style_find_by_name(name);
	if (style == NULL)
		return (false);
	wl_list_remove(&style->link);
	cow_style_destroy(style);
	cow_style_apply_all_live();
	return (true);
}

static char *
trim(char *text)
{
	while (isspace((unsigned char)*text))
		text++;
	char *end = text + strlen(text);
	while (end > text && isspace((unsigned char)end[-1]))
		*--end = '\0';
	return (text);
}

static void
strip_comment(char *text)
{
	bool sq = false, dq = false, escape = false;
	for (char *p = text; *p != '\0'; p++) {
		if (escape) {
			escape = false;
			continue;
		}
		if (dq && *p == '\\') {
			escape = true;
			continue;
		}
		if (!dq && *p == '\'')
			sq = !sq;
		else if (!sq && *p == '"')
			dq = !dq;
		else if (!sq && !dq && *p == '#') {
			*p = '\0';
			return;
		}
	}
}

static bool
style_key_from_name(const char *name, enum cow_style_key *key)
{
	static const struct {
		const char *name;
		enum cow_style_key key;
	} keys[] = {
		{ "layer", COW_STYLE_LAYER },
		{ "circulate.skip", COW_STYLE_CIRCULATE_SKIP },
		{ "sticky.desk", COW_STYLE_STICKY_DESK },
		{ "sticky.page", COW_STYLE_STICKY_PAGE },
		{ "placement.output", COW_STYLE_PLACEMENT_OUTPUT },
		{ "placement.desk", COW_STYLE_PLACEMENT_DESK },
		{ "placement.page", COW_STYLE_PLACEMENT_PAGE },
		{ "placement.x", COW_STYLE_PLACEMENT_X },
		{ "placement.y", COW_STYLE_PLACEMENT_Y },
		{ "placement.width", COW_STYLE_PLACEMENT_WIDTH },
		{ "placement.height", COW_STYLE_PLACEMENT_HEIGHT },
		{ "placement.anchor", COW_STYLE_PLACEMENT_ANCHOR },
		{ "state.maximized", COW_STYLE_STATE_MAXIMIZED },
		{ "state.maximized.ignore_reserved",
		    COW_STYLE_STATE_MAXIMIZED_IGNORE_RESERVED },
		{ "state.fullscreen", COW_STYLE_STATE_FULLSCREEN },
		{ "state.shaded", COW_STYLE_STATE_SHADED },
		{ "state.iconified", COW_STYLE_STATE_ICONIFIED },
	};
	for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
		if (strcmp(name, keys[i].name) == 0) {
			*key = keys[i].key;
			return (true);
		}
	}
	return (false);
}

static bool
style_value_valid(enum cow_style_key key, const char *value)
{
	bool b;
	switch (key) {
	case COW_STYLE_LAYER: {
		if (strcmp(value, "top") == 0 || strcmp(value, "bottom") == 0)
			return (true);
		const char *errstr;
		(void)strtonum(value, 0, 255, &errstr);
		return (errstr == NULL);
	}
	case COW_STYLE_CIRCULATE_SKIP:
	case COW_STYLE_STICKY_DESK:
	case COW_STYLE_STICKY_PAGE:
	case COW_STYLE_STATE_MAXIMIZED_IGNORE_RESERVED:
	case COW_STYLE_STATE_FULLSCREEN:
	case COW_STYLE_STATE_SHADED:
	case COW_STYLE_STATE_ICONIFIED:
		return (cmd_parse_bool(value, &b));
	case COW_STYLE_STATE_MAXIMIZED:
		return (strcmp(value, "none") == 0 ||
		    strcmp(value, "horizontal") == 0 ||
		    strcmp(value, "vertical") == 0 ||
		    strcmp(value, "both") == 0);
	case COW_STYLE_PLACEMENT_ANCHOR:
		return (cow_placement_anchor_parse(value, NULL));
	default:
		return (value[0] != '\0');
	}
}

static void
style_entry_add(struct cow_style *style, enum cow_style_key key,
    const char *name, const char *value)
{
	struct cow_style_entry *entry = xcalloc(1, sizeof(*entry));
	entry->key = key;
	entry->name = name != NULL ? xstrdup(name) : NULL;
	entry->value = xstrdup(value);
	wl_list_insert(style->entries.prev, &entry->link);
}

bool
cow_style_parse_body(struct cow_style *style, const char *body,
    struct cmd_q *cmdq)
{
	size_t len = strlen(body);
	if (len < 2 || body[0] != '{' || body[len - 1] != '}') {
		cmdq_error(cmdq, "style: body must be brace-delimited");
		return (false);
	}
	style->body = xstrdup(body + 1);
	style->body[len - 2] = '\0';
	char *source = xstrdup(style->body);
	char *cursor = source;
	bool in_decor = false;
	unsigned line_nr = 0;
	char *line;
	while ((line = strsep(&cursor, "\n")) != NULL) {
		line_nr++;
		strip_comment(line);
		line = trim(line);
		if (*line == '\0')
			continue;
		if (in_decor && strcmp(line, "}") == 0) {
			in_decor = false;
			continue;
		}
		if (!in_decor && (strcmp(line, "decor {") == 0 ||
		    strcmp(line, "decor{") == 0)) {
			in_decor = true;
			continue;
		}

		char *space = line;
		while (*space != '\0' && !isspace((unsigned char)*space))
			space++;
		if (*space == '\0') {
			cmdq_error(cmdq, "style: line %u: missing value", line_nr);
			goto fail;
		}
		*space++ = '\0';
		char *raw = trim(space);
		char *value = cmd_string_unquote(raw);
		if (value == NULL) {
			cmdq_error(cmdq, "style: line %u: bad quoting", line_nr);
			goto fail;
		}
		if (in_decor) {
			if (!cmd_set_is_decor_key(line)) {
				cmdq_error(cmdq, "style: line %u: unknown decor key: %s",
				    line_nr, line);
				xfree(value);
				goto fail;
			}
			char *stored = cmd_set_normalize_decor_value(line, value,
			    cmdq);
			xfree(value);
			if (stored == NULL)
				goto fail;
			style_entry_add(style, COW_STYLE_DECOR_ENTRY, line, stored);
			xfree(stored);
			continue;
		}
		if (strcmp(line, "decor") == 0) {
			style_entry_add(style, COW_STYLE_DECOR_PROFILE, NULL, value);
			xfree(value);
			continue;
		}
		enum cow_style_key key;
		if (!style_key_from_name(line, &key)) {
			cmdq_error(cmdq, "style: line %u: unknown property: %s",
			    line_nr, line);
			xfree(value);
			goto fail;
		}
		if (!style_value_valid(key, value)) {
			cmdq_error(cmdq, "style: line %u: invalid value for %s: %s",
			    line_nr, line, value);
			xfree(value);
			goto fail;
		}
		style_entry_add(style, key, NULL, value);
		xfree(value);
	}
	if (in_decor) {
		cmdq_error(cmdq, "style: unterminated decor block");
		goto fail;
	}
	xfree(source);
	return (true);

fail:
	xfree(source);
	return (false);
}

static bool
run_argv(struct cow_view *view, size_t argc, const char *const *argv)
{
	char *text = cow_command_from_argv(argc, argv);
	if (text == NULL)
		return (false);
	struct cmd_list *commands = NULL;
	char *cause = NULL;
	bool ok = cmd_string_parse(text, &commands, NULL, 0, &cause) == 0 &&
	    commands != NULL;
	if (!ok) {
		cow_log_warn("style command parse failed (command=%s error=%s)",
		    text, cause != NULL ? cause : "unknown");
	} else {
		struct cmd_q *cmdq = cmdq_new();
		cmdq->client_fd = -1;
		cmdq->in_style = 1;
		cmdq_set_default_target(cmdq, view, view->desk, view->output);
		cmdq_run(cmdq, commands);
		(void)cmdq_free(cmdq);
		cmd_list_free(commands);
	}
	xfree(cause);
	free(text);
	return (ok);
}

static void
resolve_entry(struct resolved_style *resolved,
    const struct cow_style_entry *entry)
{
	#define RESOLVE(member) xstrset(&resolved->member, entry->value)
	switch (entry->key) {
	case COW_STYLE_LAYER: RESOLVE(layer); resolved->live_mask |=
	    COW_VIEW_POLICY_LAYER; break;
	case COW_STYLE_CIRCULATE_SKIP: RESOLVE(circulate_skip);
	    resolved->live_mask |= COW_VIEW_POLICY_CIRCULATE_SKIP; break;
	case COW_STYLE_STICKY_DESK: RESOLVE(sticky_desk); resolved->live_mask |=
	    COW_VIEW_POLICY_STICKY_DESK; break;
	case COW_STYLE_STICKY_PAGE: RESOLVE(sticky_page); resolved->live_mask |=
	    COW_VIEW_POLICY_STICKY_PAGE; break;
	case COW_STYLE_PLACEMENT_OUTPUT: RESOLVE(output); break;
	case COW_STYLE_PLACEMENT_DESK: RESOLVE(desk); break;
	case COW_STYLE_PLACEMENT_PAGE: RESOLVE(page); break;
	case COW_STYLE_PLACEMENT_X: RESOLVE(x); break;
	case COW_STYLE_PLACEMENT_Y: RESOLVE(y); break;
	case COW_STYLE_PLACEMENT_WIDTH: RESOLVE(width); break;
	case COW_STYLE_PLACEMENT_HEIGHT: RESOLVE(height); break;
	case COW_STYLE_PLACEMENT_ANCHOR: RESOLVE(anchor); break;
	case COW_STYLE_STATE_MAXIMIZED: RESOLVE(maximized); break;
	case COW_STYLE_STATE_MAXIMIZED_IGNORE_RESERVED:
	    RESOLVE(maximize_ignore_reserved); break;
	case COW_STYLE_STATE_FULLSCREEN: RESOLVE(fullscreen); break;
	case COW_STYLE_STATE_SHADED: RESOLVE(shaded); break;
	case COW_STYLE_STATE_ICONIFIED: RESOLVE(iconified); break;
	case COW_STYLE_DECOR_PROFILE:
	case COW_STYLE_DECOR_ENTRY:
		break;
	}
	#undef RESOLVE
}

static void
resolved_style_fini(struct resolved_style *resolved)
{
	xfree(resolved->layer);
	xfree(resolved->circulate_skip);
	xfree(resolved->sticky_desk);
	xfree(resolved->sticky_page);
	xfree(resolved->output);
	xfree(resolved->desk);
	xfree(resolved->page);
	xfree(resolved->x);
	xfree(resolved->y);
	xfree(resolved->width);
	xfree(resolved->height);
	xfree(resolved->anchor);
	xfree(resolved->maximized);
	xfree(resolved->maximize_ignore_reserved);
	xfree(resolved->fullscreen);
	xfree(resolved->shaded);
	xfree(resolved->iconified);
}

static void
resolve_for_view(struct cow_view *view, struct resolved_style *resolved)
{
	struct cow_style *style;
	wl_list_for_each(style, &styles, link)
	{
		if (!cow_policy_matches_view(&style->policy, view))
			continue;
		struct cow_style_entry *entry;
		wl_list_for_each(entry, &style->entries, link)
			resolve_entry(resolved, entry);
	}
}

static int
resolve_layer(const char *value)
{
	if (value == NULL)
		return (cow_config != NULL ? cow_config->default_layer :
		    COW_CFG_DEFAULT_LAYER);
	if (strcmp(value, "top") == 0)
		return (cow_config != NULL ? cow_config->max_layer :
		    COW_CFG_MAX_LAYER);
	if (strcmp(value, "bottom") == 0)
		return (0);
	const char *errstr;
	long long layer = strtonum(value, 0, 255, &errstr);
	return (errstr == NULL ? (int)layer :
	    (cow_config != NULL ? cow_config->default_layer :
	    COW_CFG_DEFAULT_LAYER));
}

static void
set_desk_sticky(struct cow_view *view, bool target)
{
	bool current = (view->flags & COW_VIEW_STICKY_DESK_FLAG) != 0;
	if (current == target)
		return;
	if (target)
		view->flags |= COW_VIEW_STICKY_DESK_FLAG;
	else {
		view->flags &= ~COW_VIEW_STICKY_DESK_FLAG;
		if (view->output != NULL && view->output->current_desk != NULL)
			cow_view_assign_desk(view,
			    view->output->current_desk->nr);
	}
	view->dec.dirty = true;
	cow_update_request(COW_UPDATE_MANAGE | COW_UPDATE_STATUS);
}

static void
apply_live_values(struct cow_view *view, const struct resolved_style *resolved)
{
	uint32_t owned = view->style_live_mask | resolved->live_mask;
	if ((owned & COW_VIEW_POLICY_LAYER) != 0 &&
	    (view->policy_overrides & COW_VIEW_POLICY_LAYER) == 0)
		cow_view_set_layer(view, resolve_layer(resolved->layer));
	if ((owned & COW_VIEW_POLICY_CIRCULATE_SKIP) != 0 &&
	    (view->policy_overrides & COW_VIEW_POLICY_CIRCULATE_SKIP) == 0) {
		bool value = false;
		if (resolved->circulate_skip != NULL)
			(void)cmd_parse_bool(resolved->circulate_skip, &value);
		if (value)
			view->flags |= COW_VIEW_CIRCULATE_SKIP_FLAG;
		else
			view->flags &= ~COW_VIEW_CIRCULATE_SKIP_FLAG;
	}
	if ((owned & COW_VIEW_POLICY_STICKY_DESK) != 0 &&
	    (view->policy_overrides & COW_VIEW_POLICY_STICKY_DESK) == 0) {
		bool value = false;
		if (resolved->sticky_desk != NULL)
			(void)cmd_parse_bool(resolved->sticky_desk, &value);
		set_desk_sticky(view, value);
	}
	if ((owned & COW_VIEW_POLICY_STICKY_PAGE) != 0 &&
	    (view->policy_overrides & COW_VIEW_POLICY_STICKY_PAGE) == 0) {
		bool value = false;
		if (resolved->sticky_page != NULL)
			(void)cmd_parse_bool(resolved->sticky_page, &value);
		cow_view_set_page_sticky(view, value);
	}
	view->style_live_mask = resolved->live_mask;
}

static void
apply_decor(struct cow_view *view)
{
	cow_view_cfg_rebase_global(&view->cfg);
	struct cmd_q *cmdq = cmdq_new();
	cmdq->client_fd = -1;
	cmdq->in_style = 1;
	cmdq_set_default_target(cmdq, view, view->desk, view->output);
	cmdq->target = cmdq->default_target;

	struct cow_style *style;
	wl_list_for_each(style, &styles, link)
	{
		if (!cow_policy_matches_view(&style->policy, view))
			continue;
		struct cow_style_entry *entry;
		wl_list_for_each(entry, &style->entries, link)
		{
			if (entry->key == COW_STYLE_DECOR_PROFILE)
				(void)cow_decor_apply_named(entry->value, cmdq);
			else if (entry->key == COW_STYLE_DECOR_ENTRY)
				(void)cmd_set_apply_decor(entry->name, entry->value,
				    cmdq);
		}
	}
	(void)cmdq_free(cmdq);
	cow_decoration_sync_metrics(view);
	(void)cow_container_refresh(view);
	view->dec.dirty = true;
	cow_update_request(COW_UPDATE_MANAGE);
}

static void
apply_initial_scope(struct cow_view *view,
    const struct resolved_style *resolved)
{
	if (resolved->output != NULL) {
		const char *argv[] = { "window-move", "-o", resolved->output };
		(void)run_argv(view, 3, argv);
	}
	if (resolved->desk != NULL && view->output != NULL) {
		char *end;
		long nr = strtol(resolved->desk, &end, 10);
		struct cow_desk *desk = NULL;
		if (end != resolved->desk && *end == '\0' && nr >= 0 && nr <= 255)
			desk = cow_desk_by_nr(view->output, (uint8_t)nr);
		else
			desk = cow_desk_by_name(view->output, resolved->desk);
		if (desk != NULL)
			cow_view_assign_desk(view, desk->nr);
	}
}

static void
apply_initial_state(struct cow_view *view,
    const struct resolved_style *resolved)
{
	if (resolved->x != NULL || resolved->y != NULL) {
		const char *argv[5] = { "window-move" };
		size_t argc = 1;
		if (resolved->x != NULL) {
			argv[argc++] = "-x";
			argv[argc++] = resolved->x;
		}
		if (resolved->y != NULL) {
			argv[argc++] = "-y";
			argv[argc++] = resolved->y;
		}
		(void)run_argv(view, argc, argv);
	}
	if (resolved->width != NULL || resolved->height != NULL) {
		const char *argv[5] = { "window-resize" };
		size_t argc = 1;
		if (resolved->width != NULL) {
			argv[argc++] = "-w";
			argv[argc++] = resolved->width;
		}
		if (resolved->height != NULL) {
			argv[argc++] = "-h";
			argv[argc++] = resolved->height;
		}
		(void)run_argv(view, argc, argv);
	}
	if (resolved->anchor != NULL) {
		xstrset(&view->style_pending_anchor, resolved->anchor);
		xstrset(&view->style_anchor, resolved->anchor);
		xstrset(&view->style_anchor_output,
		    view->output != NULL ? view->output->name : NULL);
	}
	if (resolved->page != NULL) {
		const char *argv[] = { "window-move", "-P", resolved->page };
		(void)run_argv(view, 3, argv);
	}
	if (resolved->maximized != NULL &&
	    strcmp(resolved->maximized, "none") == 0) {
		const char *argv[] = { "window-maximize", "-u" };
		(void)run_argv(view, 2, argv);
	} else if (resolved->maximized != NULL) {
		const char *argv[5] = { "window-maximize", "-m" };
		size_t argc = 2;
		if (strcmp(resolved->maximized, "horizontal") == 0)
			argv[argc++] = "-h";
		else if (strcmp(resolved->maximized, "vertical") == 0)
			argv[argc++] = "-v";
		bool ignore = false;
		if (resolved->maximize_ignore_reserved != NULL)
			(void)cmd_parse_bool(resolved->maximize_ignore_reserved,
			    &ignore);
		if (ignore)
			argv[argc++] = "-R";
		(void)run_argv(view, argc, argv);
	}
	bool value;
	if (resolved->fullscreen != NULL &&
	    cmd_parse_bool(resolved->fullscreen, &value)) {
		if (value)
			cow_view_fullscreen(view);
		else if (view->fullscreen)
			cow_view_exit_fullscreen(view);
	}
	if (resolved->shaded != NULL && cmd_parse_bool(resolved->shaded, &value)) {
		if (value && view->sd == COW_SHADE_NONE)
			cow_view_shade(view, COW_SHADE_TITLEBAR);
		else if (!value && view->sd != COW_SHADE_NONE)
			cow_view_unshade(view);
	}
	if (resolved->iconified != NULL &&
	    cmd_parse_bool(resolved->iconified, &value)) {
		if (value && !cow_view_is_iconified(view))
			cow_view_iconify(view);
		else if (!value && cow_view_is_iconified(view))
			cow_view_deiconify(view);
	}
}

void
cow_style_apply_for_event(struct cow_view *view, bool initial)
{
	if (view == NULL || view->closed || applying)
		return;
	styles_lazy_init();
	applying = true;
	struct resolved_style resolved = { 0 };
	resolve_for_view(view, &resolved);
	bool apply_initial_values = initial && !view->style_initial_applied;
	if (apply_initial_values) {
		apply_initial_scope(view, &resolved);
		resolved_style_fini(&resolved);
		memset(&resolved, 0, sizeof(resolved));
		resolve_for_view(view, &resolved);
	}
	apply_decor(view);
	apply_live_values(view, &resolved);
	if (apply_initial_values) {
		apply_initial_state(view, &resolved);
		view->style_initial_applied = true;
	}
	resolved_style_fini(&resolved);
	applying = false;
	if (reapply_pending && !applying_all) {
		reapply_pending = false;
		cow_style_apply_all_live();
	}
}

void
cow_style_apply_pending_initial(struct cow_view *view)
{
	if (view == NULL || view->style_pending_anchor == NULL)
		return;

	char *anchor = view->style_pending_anchor;
	view->style_pending_anchor = NULL;
	const char *argv[] = { "window-move", "-a", anchor };
	(void)run_argv(view, 3, argv);
	xfree(anchor);
}

void
cow_style_cancel_pending_initial(struct cow_view *view)
{
	if (view == NULL)
		return;
	xfree(view->style_pending_anchor);
	view->style_pending_anchor = NULL;
	xfree(view->style_anchor);
	view->style_anchor = NULL;
	xfree(view->style_anchor_output);
	view->style_anchor_output = NULL;
}

void
cow_style_reflow_anchors(struct cow_output *output)
{
	if (output == NULL)
		return;

	struct cow_view *view;
	wl_list_for_each(view, &cow_wm.views, wm_link)
	{
		if (view->closed || view->style_anchor == NULL ||
		    view->style_anchor_output == NULL || output->name == NULL ||
		    strcmp(view->style_anchor_output, output->name) != 0 ||
		    !view->has_dimensions ||
		    view->maximized || view->fullscreen)
			continue;
		if (view->output != output)
			cow_view_reassign_output(view, output);
		const char *argv[] = {
			"window-move", "-a", view->style_anchor,
		};
		(void)run_argv(view, 3, argv);
	}
}

void
cow_style_apply_all_live(void)
{
	if (cow_config == NULL || cow_ipc.startup_mode)
		return;
	if (applying) {
		reapply_pending = true;
		return;
	}
	styles_lazy_init();
	applying_all = true;
	struct cow_view *view;
	wl_list_for_each(view, &cow_wm.views, wm_link)
		cow_style_apply_for_event(view, false);
	applying_all = false;
	if (reapply_pending) {
		reapply_pending = false;
		cow_style_apply_all_live();
	}
}

bool
cow_style_is_applying(void)
{
	return (applying);
}

cJSON *
cow_style_save_runtime(void)
{
	cJSON		 *saved = cJSON_CreateArray();
	struct cow_style *style;
	wl_list_for_each(style, cow_styles_head(), link)
	{
		if (!style->runtime)
			continue;
		struct state_value *snapshot = state_snapshot_style(style);
		/* The serializer supplies its own surrounding newlines. */
		char		   *body  = xstrdup(style->body);
		struct state_value *value = state_value_string(trim(body));
		state_record_set(snapshot, "body", value);
		state_value_unref(value);
		xfree(body);
		char *command = cow_config_serialize_topic("style", snapshot);
		state_value_unref(snapshot);
		cJSON *record = cJSON_CreateObject();
		cJSON_AddStringToObject(record, "command", trim(command));
		if (style->file != NULL)
			cJSON_AddStringToObject(record, "file", style->file);
		cJSON_AddItemToArray(saved, record);
		xfree(command);
	}
	return saved;
}

void
cow_style_restore_runtime(const cJSON *saved)
{
	if (!cJSON_IsArray(saved))
		return;
	const cJSON *record;
	cJSON_ArrayForEach(record, saved)
	{
		const cJSON *text = cJSON_GetObjectItemCaseSensitive(record,
		    "command");
		const cJSON *file = cJSON_GetObjectItemCaseSensitive(record,
		    "file");
		if (!cJSON_IsString(text))
			continue;
		struct cmd_list *list  = NULL;
		char		*cause = NULL;
		if (cmd_string_parse(text->valuestring, &list,
			cJSON_IsString(file) ? file->valuestring : NULL, 0,
			&cause) != 0) {
			cow_log_warn("state restore: invalid style: %s",
			    cause != NULL ? cause : "parse error");
			xfree(cause);
			continue;
		}
		if (list == NULL)
			continue;
		struct cmd *cmd = TAILQ_FIRST(&list->list);
		/* Restore definitions only, never executable startup commands.
		 * Fresh configuration definitions take precedence by name.
		 */
		if (cmd != NULL && TAILQ_NEXT(cmd, qentry) == NULL &&
		    cmd->entry == &cmd_style_entry &&
		    !args_has(cmd->args, 'd') &&
		    args_get(cmd->args, 'n') != NULL &&
		    cow_style_find_by_name(args_get(cmd->args, 'n')) == NULL) {
			struct cmd_q *cmdq = cmdq_new();
			cmdq->client_fd	   = -1;
			if (!cmdq_run_immediate(cmdq, list))
				cow_log_warn(
				    "state restore: could not restore style %s",
				    args_get(cmd->args, 'n'));
			(void)cmdq_free(cmdq);
		}
		cmd_list_free(list);
	}
}
