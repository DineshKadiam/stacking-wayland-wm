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
#include <stdio.h>
#include <limits.h>
#include <string.h>

#include "cow/cmd.h"
#include "cow/memory.h"
#include "cow/view.h"
#include "cow/wm.h"
#include "cow/update.h"
#include "cow/menu.h"
#include "cow/seat.h"

static enum cmd_retval cmd_container_exec(struct cmd *, struct cmd_q *);

bool
cow_container_hidden_menu(struct cow_container *container)
{
	if (!cow_container_hidden_count(container))
		return false;
	struct cow_view *host = container->frame_view ? container->frame_view :
	    container->active ? container->active->view : NULL;
	if (!host || host->closed || !host->identifier)
		return false;
	struct cow_menu *menu = cow_menu_generated_begin("\037cow-hidden-panes",
	    "Restore hidden pane", true, true);
	if (!menu)
		return false;
	struct cow_container_pane *pane = NULL;
	while ((pane = cow_container_pane_next(container, pane))) {
		if (!pane->layout.hidden)
			continue;
		struct cow_view *view = pane->active ? pane->active->view : NULL;
		char		*label, *action;
		const char *title = view && view->title ? view->title : "Empty";
		if (pane->name)
			xasprintf(&label, "%s — %s", pane->name, title);
		else
			xasprintf(&label, "Pane %d — %s", pane->id, title);
		xasprintf(&action, "window-container -t '#%s' -b '#%d' -v on",
		    host->identifier, pane->id);
		cow_menu_item_add(menu, COW_MENU_ITEM_NORMAL, label, action,
		    false, false, NULL);
		xfree(action);
		xfree(label);
	}
	int x = 0, y = 0;
	cow_seat_get_pointer(&x, &y);
	cow_menu_popup_open(menu, x, y, container->frame_view, NULL, true);
	cow_update_request(COW_UPDATE_MANAGE);
	return true;
}

static enum cmd_retval
cmd_attach_exec(struct cmd *self, struct cmd_q *cmdq)
{
	struct args	*args = self->args;
	struct cow_view *host = args_has(args, 't') ?
	    cmd_ctx_target_window(cmdq) :
	    NULL;
	if (host == NULL) {
		cmdq_error(cmdq, "window-attach: an explicit host is required");
		return CMD_RETURN_ERROR;
	}
	if (args_has(args, 'D')) {
		struct cow_container *c = cow_container_window_owner(host);
		if (c && c->attached)
			cow_container_destroy(c);
		cow_update_request(COW_UPDATE_MANAGE | COW_UPDATE_STATUS);
		return CMD_RETURN_NORMAL;
	}
	struct cow_box slot;
	int	   *values[] = { &slot.x, &slot.y, &slot.width, &slot.height };
	const char *keys     = "xywh";
	for (size_t i = 0; i < 4; i++) {
		char *cause = NULL;
		if (!args_has(args, keys[i])) {
			cmdq_error(cmdq,
			    "window-attach: slot geometry is required");
			return CMD_RETURN_ERROR;
		}
		*values[i] = args_strtonum(args, keys[i], i < 2 ? -32768 : 1,
		    32768, &cause);
		if (cause) {
			cmdq_error(cmdq, "window-attach: invalid geometry: %s",
			    cause);
			xfree(cause);
			return CMD_RETURN_ERROR;
		}
	}
	const char *visibility = args_get(args, 'v');
	const char *close_policy = args_get(args, 'c');
	if (close_policy && strcmp(close_policy, "on") &&
	    strcmp(close_policy, "off")) {
		cmdq_error(cmdq,
		    "window-attach: expected close policy on or off");
		return CMD_RETURN_ERROR;
	}
	if (visibility && strcmp(visibility, "on") &&
	    strcmp(visibility, "off")) {
		cmdq_error(cmdq,
		    "window-attach: expected visibility on or off");
		return CMD_RETURN_ERROR;
	}
	if (!cow_container_attach(host,
		args_has(args, 's') ? cmdq->source.window : NULL,
		args_get(args, 'n'), slot,
		!visibility || !strcmp(visibility, "on"),
		close_policy && !strcmp(close_policy, "on"))) {
		cmdq_error(cmdq,
		    "window-attach: cannot attach this window or slot");
		return CMD_RETURN_ERROR;
	}
	return CMD_RETURN_NORMAL;
}

struct cmd_entry cmd_attach_entry = {
	"window-attach",
	"t:s:n:x:y:w:h:v:c:D",
	0,
	0,
	"-t host [-D | [-s client] -n slot -x X -y Y -w width -h height [-v on|off] [-c on|off]]",
	cmd_attach_exec,
	CMD_MODULE,
};

static struct cow_view *
container_frame_target(struct cow_view *view)
{
	struct cow_container *c = view ? cow_view_container(view) : NULL;
	return c && c->frame_view ? c->frame_view : view;
}

static bool
container_direction(const char *edge, enum cow_container_axis *axis,
    bool *before)
{
	if (edge == NULL)
		return false;
	if (strcmp(edge, "left") == 0 || strcmp(edge, "right") == 0)
		*axis = COW_CONTAINER_HORIZONTAL;
	else if (strcmp(edge, "up") == 0 || strcmp(edge, "down") == 0)
		*axis = COW_CONTAINER_VERTICAL;
	else
		return false;
	*before = strcmp(edge, "left") == 0 || strcmp(edge, "up") == 0;
	return true;
}

struct cmd_entry cmd_container_entry = {
	"window-container",
	"t:s:e:f:N:G:B:b:z:o:v:arcqnpRPHUEDTA",
	0,
	0,
	"[-t target | -G name] [-b pane] [-s source] "
	"[-E -N name [-B pane] | -E -e direction [-B pane] | -D | "
	"-N name | -B pane-name | -a | -e direction | -f direction | "
	"-z column|container|off | -o clockwise|anticlockwise|180 | "
	"-v on|off|toggle|menu | -T [-A] | -rcqnpRPHU]",
	cmd_container_exec,
	0,
};

static enum cmd_retval
cmd_container_exec(struct cmd *self, struct cmd_q *cmdq)
{
	struct args *args = self->args;
	unsigned     actions = 0;
	for (const char *flag = "arcqnpRPHUTzovfED"; *flag; flag++)
		actions += args_has(args, *flag) != 0;
	/* -e qualifies an add, and -N/-B name an empty layout or pane. */
	if (args_has(args, 'e') && !args_has(args, 'a') && !args_has(args, 'E'))
		actions++;
	if (!args_has(args, 'E'))
		actions += (args_has(args, 'N') != 0) +
		    (args_has(args, 'B') != 0);
	if (actions != 1 || (args_has(args, 'A') && !args_has(args, 'T')) ||
	    (args_has(args, 's') &&
		(args_has(args, 'E') ||
		    (!args_has(args, 'a') && !args_has(args, 'e'))))) {
		cmdq_error(cmdq,
		    "container: specify one action with its supported options");
		return CMD_RETURN_ERROR;
	}
	if (args_has(args, 'E') && args_has(args, 'N')) {
		if (args_has(args, 'G') ||
		    args_has(args, 't') || args_has(args, 'e') ||
		    cow_container_create_empty(args_get(args, 'N'),
			args_get(args, 'B')) == NULL) {
			cmdq_error(cmdq,
			    "container: use -E -N unique-name [-B pane-name]");
			return CMD_RETURN_ERROR;
		}
		return CMD_RETURN_NORMAL;
	}

	/* Resolve the target window (-t or focused) */
	struct cow_view	     *target = NULL;
	struct cow_container *named  = NULL;
	if (args_has(args, 'G')) {
		if (args_has(args, 't')) {
			cmdq_error(cmdq,
			    "container: -G and -t are mutually exclusive");
			return CMD_RETURN_ERROR;
		}
		named = cow_container_find(args_get(args, 'G'));
		if (named == NULL) {
			cmdq_error(cmdq, "container: unknown name: %s",
			    args_get(args, 'G'));
			return CMD_RETURN_ERROR;
		}
		target = named->active ? named->active->view : NULL;
	} else {
		target = cmd_require_target_window(cmdq, "container");
	}
	if (target == NULL && named == NULL)
		return (CMD_RETURN_ERROR);
	struct cow_container	  *container = named ? named :
						       cow_view_container(target);
	/* A nested frame's restore menu refers to its own layout, not the
	 * outer pane in which the frame is tabbed.
	 */
	if (!named && args_has(args, 'v') &&
	    (args_has(args, 'b') || !strcmp(args_get(args, 'v'), "menu")) &&
	    cow_container_window_owner(target))
		container = cow_container_window_owner(target);
	if (container == NULL)
		container = cow_container_window_owner(target);
	if (container && container->attached) {
		cmdq_error(cmdq, "container: this layout is owned by a module");
		return CMD_RETURN_ERROR;
	}
	struct cow_container_pane *selected  = target ?
	     cow_view_pane(target) :
	     cow_container_pane_next(container, NULL);
	if (selected == NULL && container != NULL)
		selected = cow_container_pane_next(container, NULL);
	if (args_has(args, 'b')) {
		const char *name = args_get(args, 'b');
		struct cow_container_pane *pane =
		    cow_container_pane_find(container, name);
		/* Names take precedence over the stable numeric pane selector.
		 */
		if (!pane && container && name[0] == '#') {
			const char *error;
			int id = strtonum(name + 1, 0, INT_MAX, &error);
			if (!error)
				while ((pane = cow_container_pane_next(container, pane)))
					if (pane->id == id)
						break;
		}
		if (pane == NULL) {
			cmdq_error(cmdq, "container: unknown pane: %s",
			    args_get(args, 'b'));
			return CMD_RETURN_ERROR;
		}
		selected = pane;
		target	 = pane->active ? pane->active->view : NULL;
	}
	if (args_has(args, 'v')) {
		const char *value = args_get(args, 'v');
		if (!strcmp(value, "menu")) {
			if (cow_container_hidden_menu(container))
				return CMD_RETURN_NORMAL;
			cmdq_error(cmdq, "container: no hidden panes");
			return CMD_RETURN_ERROR;
		}
		bool visible;
		if (!strcmp(value, "on"))
			visible = true;
		else if (!strcmp(value, "off"))
			visible = false;
		else if (!strcmp(value, "toggle") && selected != NULL)
			visible = selected->layout.hidden;
		else {
			cmdq_error(cmdq,
			    "container: expected visibility on, off, toggle or menu");
			return CMD_RETURN_ERROR;
		}
		if (!cow_container_pane_visible(container, selected, visible)) {
			cmdq_error(cmdq, "container: cannot hide the last visible pane or show this pane");
			return CMD_RETURN_ERROR;
		}
		return CMD_RETURN_NORMAL;
	}
	if (args_has(args, 'o')) {
		if (!named && cow_container_window_owner(target))
			container = cow_container_window_owner(target);
		const char *direction = args_get(args, 'o');
		unsigned    turns = strcmp(direction, "clockwise") == 0 ? 1 :
		       strcmp(direction, "anticlockwise") == 0		? 3 :
		       strcmp(direction, "180") == 0			? 2 :
									  0;
		if (turns == 0) {
			cmdq_error(cmdq,
			    "container: expected rotation clockwise, anticlockwise or 180");
			return CMD_RETURN_ERROR;
		}
		if (!cow_container_rotate(container, turns)) {
			cmdq_error(cmdq,
			    "container: cannot rotate layout; "
			    "use an expanded, unzoomed split container with room for client minimum sizes");
			return CMD_RETURN_ERROR;
		}
		cow_update_request(COW_UPDATE_MANAGE | COW_UPDATE_STATUS);
		return CMD_RETURN_NORMAL;
	}
	if (args_has(args, 'D')) {
		if (container == NULL) {
			cmdq_error(cmdq, "container: no container to dissolve");
			return CMD_RETURN_ERROR;
		}
		cow_container_destroy(container);
		cow_update_request(COW_UPDATE_MANAGE | COW_UPDATE_STATUS);
		return CMD_RETURN_NORMAL;
	}
	if (args_has(args, 'E')) {
		enum cow_container_axis axis;
		bool			before;
		if ((args_has(args, 'B') && *args_get(args, 'B') == '\0') ||
		    !container_direction(args_get(args, 'e'), &axis, &before)) {
			cmdq_error(cmdq, "container: use -E -e direction [-B name]");
			return CMD_RETURN_ERROR;
		}
		bool created = false;
		if (container == NULL && target != NULL && !target->parent &&
		    !target->fullscreen && target->sd == COW_SHADE_NONE) {
			container = cow_container_create(target, NULL);
			selected = cow_view_pane(target);
			created = true;
		}
		if (container == NULL || selected == NULL ||
		    !cow_container_split_empty(container, selected,
			args_get(args, 'B'), axis, before)) {
			if (created)
				cow_container_destroy(container);
			cmdq_error(cmdq,
			    "container: cannot add empty pane; select an unshaded, non-fullscreen window or a container with room to split");
			return CMD_RETURN_ERROR;
		}
		return CMD_RETURN_NORMAL;
	}
	if (container != NULL && selected != NULL && args_has(args, 'a') &&
	    !args_has(args, 'e') && args_has(args, 'b')) {
		struct cow_view *source = args_has(args, 's') ?
		    cmdq->source.window :
		    cmd_ctx_target_window(cmdq);
		if (source == NULL ||
		    !cow_container_insert(container, selected, source)) {
			cmdq_error(cmdq,
			    "container: cannot insert source into pane");
			return CMD_RETURN_ERROR;
		}
		return CMD_RETURN_NORMAL;
	}
	if (args_has(args, 'N') || args_has(args, 'B')) {
		bool ok = args_has(args, 'N') ?
		    (container ? cow_container_rename(container,
				     args_get(args, 'N')) :
				 cow_container_set_name(target,
				     args_get(args, 'N'))) :
		    (selected &&
			cow_container_pane_rename(container, selected,
			    args_get(args, 'B')));
		if (!ok)
			cmdq_error(cmdq,
			    "container: name is already used or target cannot be named");
		return ok ? CMD_RETURN_NORMAL : CMD_RETURN_ERROR;
	}
	/* A named layout without -b addresses its outer window. */
	if (named && !args_has(args, 'b') && named->frame_view &&
	    (args_has(args, 'a') || args_has(args, 'r') ||
		args_has(args, 'n') || args_has(args, 'p') ||
		args_has(args, 'R') || args_has(args, 'P') ||
		args_has(args, 'T') || args_has(args, 'U') ||
		args_has(args, 'z')))
		target = named->frame_view;
	if (target == NULL) {
		cmdq_error(cmdq,
		    "container: this action requires a populated pane");
		return CMD_RETURN_ERROR;
	}
	if (args_has(args, 'z')) {
		const char	       *value = args_get(args, 'z');
		enum cow_container_zoom scope;
		if (strcmp(value, "column") == 0)
			scope = COW_CONTAINER_ZOOM_COLUMN;
		else if (strcmp(value, "container") == 0)
			scope = COW_CONTAINER_ZOOM_ALL;
		else if (strcmp(value, "off") == 0)
			scope = COW_CONTAINER_ZOOM_NONE;
		else {
			cmdq_error(cmdq,
			    "container: expected zoom scope column, container or off");
			return CMD_RETURN_ERROR;
		}
		if (!cow_container_zoom(target, scope)) {
			cmdq_error(cmdq, "container: cannot zoom this pane");
			return CMD_RETURN_ERROR;
		}
		return CMD_RETURN_NORMAL;
	}
	if (args_has(args, 'H') || args_has(args, 'U') || args_has(args, 'T')) {
		bool ok = args_has(args, 'T') ?
		    cow_container_titles_toggle(target, args_has(args, 'A')) :
		    args_has(args, 'H') ? cow_container_title_hide(target) :
					  cow_container_titles_restore(target);
		if (!ok)
			cmdq_error(cmdq,
			    "container: cannot change a protected, tabbed or collapsed titlebar");
		return ok ? CMD_RETURN_NORMAL : CMD_RETURN_ERROR;
	}
	if (args_has(args, 'c') || args_has(args, 'q')) {
		bool ok = args_has(args, 'c') ? cow_container_collapse(target) :
						cow_container_equalize(target);
		if (!ok)
			cmdq_error(cmdq, "container: pane cannot be changed");
		return ok ? CMD_RETURN_NORMAL : CMD_RETURN_ERROR;
	}
	if (args_has(args, 'f')) {
		const char	*direction = args_get(args, 'f');
		uint32_t	 edge = strcmp(direction, "left") == 0 ? 4 :
			    strcmp(direction, "right") == 0	       ? 8 :
			    strcmp(direction, "up") == 0	       ? 1 :
			    strcmp(direction, "down") == 0	       ? 2 :
									 0;
		struct cow_view *next = edge ?
		    cow_container_neighbor(target, edge) :
		    NULL;
		if (next == NULL) {
			cmdq_error(cmdq,
			    "container: no pane in that direction");
			return CMD_RETURN_ERROR;
		}
		cow_container_activate(cow_view_container(next),
		    next->container_member);
		return CMD_RETURN_NORMAL;
	}

	if (args_has(args, 'a') || args_has(args, 'e')) {
		const char	       *edge   = args_get(args, 'e');
		enum cow_container_axis axis   = COW_CONTAINER_LEAF;
		bool			before = false;
		if (edge != NULL) {
			if (!container_direction(edge, &axis, &before)) {
				cmdq_error(cmdq,
				    "container: expected left, right, up or down");
				return CMD_RETURN_ERROR;
			}
		}
		/* Resolve source window (-s flag, else focused) */
		struct cow_view *source = args_has(args, 's') ?
		    cmdq->source.window :
		    cow_wm.focused_view;

		if (source == NULL || source->closed) {
			cmdq_error(cmdq, "container add: no source window");
			return (CMD_RETURN_ERROR);
		}
		/* The selector-free tab binding groups whole top-level windows.
		 * Explicit pane selectors retain their existing local meaning.
		 */
		if (!edge && !args_has(args, 't') && !args_has(args, 's') &&
		    !args_has(args, 'G') && !args_has(args, 'b')) {
			source = container_frame_target(source);
			target = source;
		}
		if (source->container_placeholder ||
		    target->container_placeholder ||
		    (edge &&
			(cow_container_window_owner(source) ||
			    cow_container_window_owner(target)))) {
			cmdq_error(cmdq,
			    "container: layouts can be tabs, not nested panes");
			return CMD_RETURN_ERROR;
		}

		/*
		 * If no -t was given, target defaulted to focused (same as
		 * source). In that case, use the next focusable window in
		 * stacking order as the target so that -a does the right
		 * thing.
		 */
		if (target == source) {
			if (args_has(args, 'G') || args_has(args, 'b')) {
				cmdq_error(cmdq,
				    "container: source is already the target");
				return CMD_RETURN_ERROR;
			}
			target = cow_view_walk(source, true,
			    cow_view_focusable);
			struct cow_container *group = cow_container_top_group(
			    source);
			struct cow_view *first = target;
			while (group && target && target != source &&
			    cow_container_top_group(target) == group) {
				target = cow_view_walk(target, true,
				    cow_view_focusable);
				if (target == first) {
					target = NULL;
					break;
				}
			}
			if (!edge)
				target = container_frame_target(target);
			if (target == NULL || target == source) {
				cmdq_error(cmdq,
				    "container add: no other window to group with");
				return (CMD_RETURN_ERROR);
			}
		}

		if (source == target) {
			cmdq_error(cmdq,
			    "container add: source and target are the same window");
			return (CMD_RETURN_ERROR);
		}

		struct cow_container *ctr = cow_view_container(target);
		if (!cow_container_can_tab(ctr, source)) {
			cmdq_error(cmdq,
			    "container: a layout must join an outer tab group");
			return CMD_RETURN_ERROR;
		}
		if (source->parent != NULL || target->parent != NULL ||
		    source->fullscreen || target->fullscreen) {
			cmdq_error(cmdq,
			    "container: cannot group dialogs or fullscreen windows");
			return CMD_RETURN_ERROR;
		}
		if (source->container_member != NULL &&
		    source->container_member->container == ctr) {
			if (edge != NULL &&
			    cow_container_split(target, source, axis, before))
				return CMD_RETURN_NORMAL;
			cmdq_error(cmdq,
			    "container add: source is already in this container");
			return (CMD_RETURN_ERROR);
		}

		/* Keep a failed transfer from destroying the source layout. */
		if (source->container_member != NULL) {
			cmdq_error(cmdq,
			    "container: detach the source from its current group first");
			return CMD_RETURN_ERROR;
		}

		if (ctr != NULL) {
			if (!cow_container_add_to(target, source)) {
				cmdq_error(cmdq,
				    "container: source does not fit");
				return CMD_RETURN_ERROR;
			}
		} else {
			ctr = cow_container_create(target, source);
		}
		if (edge != NULL &&
		    !cow_container_split(target, source, axis, before)) {
			cow_container_remove_view(source);
			cmdq_error(cmdq, "container: split does not fit");
			return CMD_RETURN_ERROR;
		}
		cow_container_activate(ctr, source->container_member);
		return (CMD_RETURN_NORMAL);
	}

	if (args_has(args, 'r')) {
		if (target->container_member == NULL) {
			cmdq_error(cmdq,
			    "container remove: window is not in a container");
			return (CMD_RETURN_ERROR);
		}
		cow_container_remove_view(target);
		return (CMD_RETURN_NORMAL);
	}

	/* With no local tab strip, cycle the tab carrying this whole layout. */
	if ((args_has(args, 'n') || args_has(args, 'p') ||
		args_has(args, 'R') || args_has(args, 'P')) &&
	    target->container_member && cow_view_pane(target)->n_tabs == 1) {
		struct cow_container *c = cow_view_container(target);
		if (c->frame_view && c->frame_view->container_member)
			target = c->frame_view;
	}
	if (target->container_member != NULL &&
	    !cow_container_window_owner(target) &&
	    (args_has(args, 'n') || args_has(args, 'p') ||
		args_has(args, 'R') || args_has(args, 'P')))
		cow_container_activate(cow_view_container(target),
		    target->container_member);

	struct cow_container *ctr = cow_view_container(target);
	if (ctr == NULL) {
		cmdq_error(cmdq, "container: window is not in a container");
		return CMD_RETURN_ERROR;
	}
	if (args_has(args, 'n'))
		cow_container_activate_next(ctr);
	else if (args_has(args, 'p'))
		cow_container_activate_prev(ctr);
	else if (args_has(args, 'R'))
		cow_container_swap_next(ctr);
	else
		cow_container_swap_prev(ctr);
	return CMD_RETURN_NORMAL;
}
