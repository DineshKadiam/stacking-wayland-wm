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
#include <stdlib.h>
#include <string.h>

#include "cow/compat.h"
#include "cow/keybinding.h"
#include "cow/cmd.h"
#include "cow/command.h"
#include "cow/compat.h"
#include "cow/config.h"
#include "cow/desk_ops.h"
#include "cow/ipc.h"
#include "cow/memory.h"
#include "cow/output.h"
#include "cow/pointer_op.h"
#include "cow/seat.h"
#include "cow/view.h"
#include "cow/wm.h"

#include "river-window-management-v1-client-protocol.h"
#include "river-xkb-bindings-v1-client-protocol.h"

/* ================================================================
 * xkb_binding_v1 event handlers
 * ================================================================ */

static void
xkb_binding_handle_pressed(void		 *data,
    __unused struct river_xkb_binding_v1 *river_binding)
{
	struct cow_keybinding *kb = data;
	kb->pending		  = true;
}

static void
xkb_binding_handle_released(__unused void *data,
    __unused struct river_xkb_binding_v1  *river_binding)
{
}

static void
xkb_binding_handle_stop_repeat(__unused void *data,
    __unused struct river_xkb_binding_v1     *river_binding)
{
}

static const struct river_xkb_binding_v1_listener xkb_binding_listener = {
	.pressed     = xkb_binding_handle_pressed,
	.released    = xkb_binding_handle_released,
	.stop_repeat = xkb_binding_handle_stop_repeat,
};

/* ================================================================
 * pointer_binding_v1 event handlers
 * ================================================================ */

static void
pointer_binding_handle_pressed(void	     *data,
    __unused struct river_pointer_binding_v1 *river_binding)
{
	struct cow_pointer_binding *pb = data;
	pb->pending		       = true;
}

static void
pointer_binding_handle_released(__unused void *data,
    __unused struct river_pointer_binding_v1  *river_binding)
{
}

static const struct river_pointer_binding_v1_listener
    pointer_binding_listener = {
	    .pressed  = pointer_binding_handle_pressed,
	    .released = pointer_binding_handle_released,
    };

struct cow_keybinding *
cow_keybinding_create(struct cow_seat *seat, xkb_keysym_t keysym,
    uint32_t modifiers, cow_action_fn action, void *action_arg)
{
	struct cow_keybinding *kb = xcalloc(1, sizeof(*kb));

	kb->keysym     = keysym;
	kb->modifiers  = modifiers;
	kb->action     = action;
	kb->action_arg = action_arg;
	kb->seat       = seat;
	kb->enabled    = false;
	kb->pending    = false;

	kb->river_binding = river_xkb_bindings_v1_get_xkb_binding(
	    cow_wm.xkb_bindings, seat->river_seat, keysym, modifiers);

	river_xkb_binding_v1_add_listener(kb->river_binding,
	    &xkb_binding_listener, kb);

	wl_list_insert(&seat->keybindings, &kb->link);

	return kb;
}

struct cow_pointer_binding *
cow_pointer_binding_create(struct cow_seat *seat, uint32_t button,
    uint32_t modifiers, cow_action_fn action, void *action_arg)
{
	struct cow_pointer_binding *pb = xcalloc(1, sizeof(*pb));

	pb->button     = button;
	pb->modifiers  = modifiers;
	pb->action     = action;
	pb->action_arg = action_arg;
	pb->seat       = seat;
	pb->enabled    = false;
	pb->pending    = false;

	pb->river_binding = river_seat_v1_get_pointer_binding(seat->river_seat,
	    button, modifiers);

	river_pointer_binding_v1_add_listener(pb->river_binding,
	    &pointer_binding_listener, pb);

	wl_list_insert(&seat->pointer_bindings, &pb->link);

	return pb;
}

void
cow_seat_enable_all_bindings(struct cow_seat *seat)
{
	if (seat->bindings_enabled)
		return;

	struct cow_keybinding *kb;
	wl_list_for_each(kb, &seat->keybindings, link)
	{
		if (!kb->enabled) {
			river_xkb_binding_v1_enable(kb->river_binding);
			kb->enabled = true;
		}
	}

	struct cow_pointer_binding *pb;
	wl_list_for_each(pb, &seat->pointer_bindings, link)
	{
		if (!pb->enabled) {
			river_pointer_binding_v1_enable(pb->river_binding);
			pb->enabled = true;
		}
	}

	seat->bindings_enabled = true;
}

void
cow_seat_process_bindings(struct cow_seat *seat)
{
	struct cow_keybinding *kb;
	wl_list_for_each(kb, &seat->keybindings, link)
	{
		if (kb->pending) {
			kb->pending = false;
			if (kb->action != NULL)
				kb->action(kb->action_arg);
		}
	}

	struct cow_pointer_binding *pb;
	wl_list_for_each(pb, &seat->pointer_bindings, link)
	{
		if (pb->pending) {
			pb->pending = false;
			if (pb->action != NULL)
				pb->action(pb->action_arg);
		}
	}
}

/*
 * Execute a command string through the command system.
 * This bridges any registered command into keybindings.
 */
static void
action_execute_cmd_string(void *arg)
{
	const char *cmd_str = arg;
	if (cmd_str == NULL)
		return;

	struct cmd_list *cmdlist = NULL;
	char		*cause	 = NULL;

	if (cmd_string_parse(cmd_str, &cmdlist, NULL, 0, &cause) != 0) {
		if (cause != NULL) {
			fprintf(stderr, "cow: keybinding command error: %s\n",
			    cause);
			xfree(cause);
		}
		return;
	}

	if (cmdlist == NULL)
		return;

	struct cmd_q *cmdq = cmdq_new();
	cmdq->client_fd	   = -1;
	cmdq_run(cmdq, cmdlist);
	cmd_list_free(cmdlist);

	/* Log any errors */
	if (!ARRAY_EMPTY(&cfg_causes)) {
		for (u_int i = 0; i < ARRAY_LENGTH(&cfg_causes); i++) {
			char *c = ARRAY_ITEM(&cfg_causes, i);
			fprintf(stderr, "cow: keybinding command: %s\n", c);
			xfree(c);
		}
		ARRAY_FREE(&cfg_causes);
	}

	cmdq_free(cmdq);
}

/*
 * Launch a terminal (fallback for default bindings).
 */
static void
action_exec_terminal(__unused void *arg)
{
	const char *term = getenv("TERMINAL");
	if (term == NULL)
		term = "foot";
	cow_command_execute(term);
}

/* ================================================================
 * Modifier and keysym string parsing
 * ================================================================ */

static const char *
parse_modifiers(const char *str, uint32_t *modifiers_out)
{
	uint32_t    mods = 0;
	const char *p	 = str;

	while (*p != '\0' && *p != '+') {
		switch (*p) {
		case 'L':
			mods |= RIVER_SEAT_V1_MODIFIERS_MOD4;
			break;
		case 'S':
			mods |= RIVER_SEAT_V1_MODIFIERS_SHIFT;
			break;
		case 'A':
			mods |= RIVER_SEAT_V1_MODIFIERS_MOD1;
			break;
		case 'C':
			mods |= RIVER_SEAT_V1_MODIFIERS_CTRL;
			break;
		case '5':
			mods |= RIVER_SEAT_V1_MODIFIERS_MOD5;
			break;
		case '0':
			break;
		default:
			fprintf(stderr,
			    "cow: unknown modifier character '%c' in "
			    "\"%s\"\n",
			    *p, str);
			break;
		}
		p++;
	}

	if (*p == '+')
		p++;

	*modifiers_out = mods;
	return p;
}

static bool
parse_key_string(const char *str, xkb_keysym_t *keysym_out,
    uint32_t *modifiers_out)
{
	uint32_t    mods;
	const char *key_part = parse_modifiers(str, &mods);

	xkb_keysym_t sym = xkb_keysym_from_name(key_part,
	    XKB_KEYSYM_CASE_INSENSITIVE);
	if (sym == XKB_KEY_NoSymbol) {
		fprintf(stderr,
		    "cow: unknown keysym \"%s\" in binding \"%s\"\n", key_part,
		    str);
		return false;
	}

	*keysym_out    = sym;
	*modifiers_out = mods;
	return true;
}

static uint32_t
parse_button_name(const char *name)
{
	if (!strcmp(name, "left"))
		return BTN_LEFT;
	if (!strcmp(name, "right"))
		return BTN_RIGHT;
	if (!strcmp(name, "middle"))
		return BTN_MIDDLE;
	if (!strcmp(name, "side"))
		return BTN_SIDE;
	if (!strcmp(name, "extra"))
		return BTN_EXTRA;
	if (!strcmp(name, "forward"))
		return BTN_FORWARD;
	if (!strcmp(name, "back"))
		return BTN_BACK;
	if (!strcmp(name, "task"))
		return BTN_TASK;

	fprintf(stderr, "cow: unknown mouse button \"%s\"\n", name);
	return 0;
}

static bool
parse_mouse_string(const char *str, uint32_t *button_out,
    uint32_t *modifiers_out)
{
	uint32_t    mods;
	const char *btn_part = parse_modifiers(str, &mods);

	uint32_t button = parse_button_name(btn_part);
	if (button == 0) {
		fprintf(stderr, "cow: invalid mouse binding \"%s\"\n", str);
		return false;
	}

	*button_out    = button;
	*modifiers_out = mods;
	return true;
}

/* ================================================================
 * Action resolver
 * ================================================================ */

bool
cow_resolve_action(const char *action_str, cow_action_fn *fn_out,
    void **arg_out)
{
	/* Try as a registered command (e.g. "close", "focus next",
	 * "expand", "workspace switch 3", etc.) */
	char tmp[256];
	strlcpy(tmp, action_str, sizeof(tmp));
	char *sp = strchr(tmp, ' ');
	if (sp != NULL)
		*sp = '\0';
	if (cmd_find_cmd(tmp) != NULL) {
		*fn_out	 = action_execute_cmd_string;
		*arg_out = xstrdup(action_str);
		return true;
	}

	/* Unrecognized -- silently ignore (may be compositor-specific) */
	fprintf(stderr, "cow: ignoring unimplemented action \"%s\"\n",
	    action_str);
	return false;
}

/* ================================================================
 * Config-driven binding registration
 * ================================================================ */

static void
register_config_keyboard_bindings(struct cow_seat *seat)
{
	struct cow_binding_entry *entry;
	wl_list_for_each(entry, &cow_config->keyboard_bindings, link)
	{
		xkb_keysym_t keysym;
		uint32_t     modifiers;

		if (!parse_key_string(entry->key_string, &keysym, &modifiers))
			continue;

		cow_action_fn fn;
		void	     *arg;
		if (!cow_resolve_action(entry->action_string, &fn, &arg))
			continue;

		cow_keybinding_create(seat, keysym, modifiers, fn, arg);
	}
}

static void
register_config_mouse_bindings(struct cow_seat *seat)
{
	struct cow_mouse_binding_entry *entry;
	wl_list_for_each(entry, &cow_config->mouse_bindings, link)
	{
		uint32_t button;
		uint32_t modifiers;

		if (!parse_mouse_string(entry->key_string, &button, &modifiers))
			continue;

		cow_action_fn fn;
		void	     *arg;
		if (!cow_resolve_action(entry->action_string, &fn, &arg))
			continue;

		cow_pointer_binding_create(seat, button, modifiers, fn, arg);
	}
}

/*
 * Register default keybindings (fallback when no config is loaded).
 */
static void
register_default_bindings(struct cow_seat *seat)
{
	uint32_t super	     = RIVER_SEAT_V1_MODIFIERS_MOD4;
	uint32_t super_shift = RIVER_SEAT_V1_MODIFIERS_MOD4 |
	    RIVER_SEAT_V1_MODIFIERS_SHIFT;

	/* Terminal */
	cow_keybinding_create(seat, XKB_KEY_Return, super, action_exec_terminal,
	    NULL);

	/* Close/quit */
	cow_keybinding_create(seat, XKB_KEY_q, super, action_execute_cmd_string,
	    xstrdup("winops -c"));
	cow_keybinding_create(seat, XKB_KEY_q, super_shift,
	    action_execute_cmd_string, xstrdup("quit"));

	/* Focus cycle */
	cow_keybinding_create(seat, XKB_KEY_j, super, action_execute_cmd_string,
	    xstrdup("focus -n"));
	cow_keybinding_create(seat, XKB_KEY_k, super, action_execute_cmd_string,
	    xstrdup("focus -p"));
	cow_keybinding_create(seat, XKB_KEY_Tab, super,
	    action_execute_cmd_string, xstrdup("focus -n"));

	/* Desk switching: Super+1..9 -> desk -d 1..9 */
	/* Move to desk:  Super+Shift+1..9 -> window-move -k 1..9 */
	xkb_keysym_t desk_syms[] = { XKB_KEY_1, XKB_KEY_2, XKB_KEY_3, XKB_KEY_4,
		XKB_KEY_5, XKB_KEY_6, XKB_KEY_7, XKB_KEY_8, XKB_KEY_9 };
	for (int i = 0; i < 9; i++) {
		char *switch_cmd, *move_cmd;
		xasprintf(&switch_cmd, "desk -d %d", i + 1);
		xasprintf(&move_cmd, "window-move -k %d", i + 1);
		cow_keybinding_create(seat, desk_syms[i], super,
		    action_execute_cmd_string, switch_cmd);
		cow_keybinding_create(seat, desk_syms[i], super_shift,
		    action_execute_cmd_string, move_cmd);
	}
	cow_keybinding_create(seat, XKB_KEY_0, super, action_execute_cmd_string,
	    xstrdup("desk -d 0"));

	/* Interactive move/resize */
	cow_keybinding_create(seat, XKB_KEY_m, super, action_execute_cmd_string,
	    xstrdup("window-move"));
	cow_keybinding_create(seat, XKB_KEY_r, super, action_execute_cmd_string,
	    xstrdup("resize"));

	cow_pointer_binding_create(seat, BTN_LEFT, super,
	    action_execute_cmd_string, xstrdup("window-move"));
	cow_pointer_binding_create(seat, BTN_RIGHT, super,
	    action_execute_cmd_string, xstrdup("resize"));

	/* Container tab cycling via mouse side buttons (no modifier).
	 * BTN_SIDE / BTN_EXTRA are the back/forward side buttons present
	 * on many mice.  Scroll wheel (X11 buttons 4/5) generates axis
	 * events in Wayland and cannot be used as pointer bindings. */
	cow_pointer_binding_create(seat, BTN_SIDE, 0, action_execute_cmd_string,
	    xstrdup("container next"));
	cow_pointer_binding_create(seat, BTN_EXTRA, 0,
	    action_execute_cmd_string, xstrdup("container prev"));
}

/*
 * Public entry point: register bindings from config, or defaults.
 */
void
cow_seat_register_default_bindings(struct cow_seat *seat)
{
	if (cow_config != NULL &&
	    (!wl_list_empty(&cow_config->keyboard_bindings) ||
		!wl_list_empty(&cow_config->mouse_bindings))) {
		register_config_keyboard_bindings(seat);
		register_config_mouse_bindings(seat);
	} else {
		register_default_bindings(seat);
	}
}

void
cow_seat_destroy_bindings(struct cow_seat *seat)
{
	struct cow_keybinding *kb, *kb_tmp;
	wl_list_for_each_safe(kb, kb_tmp, &seat->keybindings, link)
	{
		wl_list_remove(&kb->link);
		river_xkb_binding_v1_destroy(kb->river_binding);
		xfree(kb);
	}

	struct cow_pointer_binding *pb, *pb_tmp;
	wl_list_for_each_safe(pb, pb_tmp, &seat->pointer_bindings, link)
	{
		wl_list_remove(&pb->link);
		river_pointer_binding_v1_destroy(pb->river_binding);
		xfree(pb);
	}

	seat->bindings_enabled = false;
}

void
cow_rebind_all_seats(void)
{
	if (cow_ipc.startup_mode)
		return;

	struct cow_seat *seat;
	wl_list_for_each(seat, &cow_wm.seats, wm_link)
	{
		cow_seat_destroy_bindings(seat);
		cow_seat_register_default_bindings(seat);
	}
}
