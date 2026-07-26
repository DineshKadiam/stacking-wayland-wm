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
#include "cow/util.h"
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
	/*
	 * Track the press for repeat scheduling.  The action is fired
	 * the next manage cycle (via the `pending` flag); we'll stamp
	 * last_fire_ms there so the repeat clock starts when the
	 * action actually runs, not when river told us about the
	 * keypress.
	 */
	if (kb->repeatable) {
		kb->held	 = true;
		kb->first_repeat = true;
	}
}

static void
xkb_binding_handle_released(void	 *data,
    __unused struct river_xkb_binding_v1 *river_binding)
{
	struct cow_keybinding *kb = data;
	/* End the repeat sequence; the loop in cow_seat_process_bindings
	 * will stop scheduling further fires. */
	kb->held	 = false;
	kb->first_repeat = false;
}

static void
xkb_binding_handle_stop_repeat(void	 *data,
    __unused struct river_xkb_binding_v1 *river_binding)
{
	struct cow_keybinding *kb = data;
	/*
	 * River's hint that repetition should stop.  Per the
	 * river_xkb_binding_v1.stop_repeat protocol docs this is
	 * "generally sent when some other (possibly unbound) key is
	 * pressed after the pressed event is sent and before the
	 * released event is sent for this binding" -- i.e. the user
	 * has moved on to another action while the original key is
	 * still held, so we should stop firing the bound action even
	 * though `released` has not yet arrived.
	 *
	 * Modifier-only changes (e.g. releasing Super while Down is
	 * still held on a Super+Down binding) do NOT trigger
	 * `released` per the protocol; observing those would require
	 * river_xkb_bindings_seat_v1.modifiers_watch (v3) which we
	 * don't currently use.
	 */
	kb->held	 = false;
	kb->first_repeat = false;
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
    uint32_t modifiers, cow_action_fn action, void *action_arg, bool repeatable)
{
	struct cow_keybinding *kb = xcalloc(1, sizeof(*kb));

	kb->keysym	 = keysym;
	kb->modifiers	 = modifiers;
	kb->action	 = action;
	kb->action_arg	 = action_arg;
	kb->seat	 = seat;
	kb->enabled	 = false;
	kb->pending	 = false;
	kb->repeatable	 = repeatable;
	kb->held	 = false;
	kb->first_repeat = false;
	kb->last_fire_ms = 0;

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

/*
 * How long to wait between consecutive fires of a held repeatable
 * binding.  The first repeat after the initial press waits
 * repeat_delay_ms; subsequent repeats use repeat_interval_ms.  Both
 * fall back to the COW_CFG_DEFAULT_* values when cow_config isn't
 * available or carries non-positive values.
 */
static long
repeat_wait_ms(const struct cow_keybinding *kb)
{
	int delay    = COW_CFG_DEFAULT_REPEAT_DELAY_MS;
	int interval = COW_CFG_DEFAULT_REPEAT_INTERVAL_MS;
	if (cow_config != NULL) {
		if (cow_config->repeat_delay_ms > 0)
			delay = cow_config->repeat_delay_ms;
		if (cow_config->repeat_interval_ms > 0)
			interval = cow_config->repeat_interval_ms;
	}
	return kb->first_repeat ? (long)delay : (long)interval;
}

void
cow_seat_process_bindings(struct cow_seat *seat)
{
	long now_ms = cow_mono_ms();

	struct cow_keybinding *kb;
	wl_list_for_each(kb, &seat->keybindings, link)
	{
		/*
		 * Fire on a fresh press (kb->pending), or on a scheduled
		 * repeat tick for a held repeatable binding.  Repeat-only
		 * fires (held but not pending) check the elapsed wait
		 * against the appropriate delay/interval.
		 */
		/*
		 * Two fire reasons: a fresh press (pending), or a
		 * repeat tick on a held repeatable binding.  Track which
		 * so the post-fire bookkeeping can advance the repeat
		 * phase (first_repeat -> steady-state) only after the
		 * first repeat actually runs.
		 */
		bool fire_press	 = kb->pending;
		bool fire_repeat = false;
		if (!fire_press && kb->held && kb->repeatable) {
			long wait = repeat_wait_ms(kb);
			if (now_ms - kb->last_fire_ms >= wait)
				fire_repeat = true;
		}

		if (fire_press || fire_repeat) {
			kb->pending = false;
			if (kb->action != NULL)
				kb->action(kb->action_arg);
			/*
			 * Stamp the fire time AFTER the action runs so
			 * the delay clock counts from the user-visible
			 * event.  Clear first_repeat only on a true
			 * repeat fire -- the press path leaves it true
			 * so the very first repeat waits repeat_delay_ms,
			 * not repeat_interval_ms.
			 */
			kb->last_fire_ms = cow_mono_ms();
			if (fire_repeat)
				kb->first_repeat = false;
		}
	}

	struct cow_pointer_binding *pb, *pb_tmp;
	wl_list_for_each_safe(pb, pb_tmp, &seat->pointer_bindings, link)
	{
		if (pb->pending) {
			pb->pending = false;
			if (pb->action != NULL)
				pb->action(pb->action_arg);
		}
	}
}

int
cow_seat_next_repeat_ms(void)
{
	long		 now_ms = cow_mono_ms();
	long		 min	= -1;
	struct cow_seat *seat;
	wl_list_for_each(seat, &cow_wm.seats, wm_link)
	{
	struct cow_keybinding *kb, *kb_tmp;
	wl_list_for_each_safe(kb, kb_tmp, &seat->keybindings, link)
	{
			if (!kb->held || !kb->repeatable)
				continue;
			long due   = kb->last_fire_ms + repeat_wait_ms(kb);
			long delta = due - now_ms;
			if (delta < 0)
				delta = 0;
			if (min < 0 || delta < min)
				min = delta;
		}
	}
	return (min < 0) ? -1 : (int)min;
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

		cow_keybinding_create(seat, keysym, modifiers, fn, arg,
		    entry->repeatable);
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
/*
 * Each entry binds a single XKB key (or button) + modifier to either
 * a custom action_fn (NULL cmd) or the standard action_execute_cmd_string
 * (cmd != NULL).  Defined as a static table so the wiring up of all
 * default bindings is data-driven.
 */
struct default_binding {
	bool	      is_pointer; /* false = key, true = pointer button */
	xkb_keysym_t  key;	  /* used if is_pointer == false */
	uint32_t      button;	  /* used if is_pointer == true */
	const char   *modifier;	  /* "super" / "super_shift" / "" */
	cow_action_fn action;	  /* NULL falls back to cmd-string action */
	const char   *cmd; /* command string for action_execute_cmd_string;
			      ignored when action != NULL */
};

#define DB_KEY(k, mods, cmd) { false, (k), 0, (mods), NULL, (cmd) }
#define DB_KEY_FN(k, mods, fn) { false, (k), 0, (mods), (fn), NULL }
#define DB_PTR(b, mods, cmd) { true, 0, (b), (mods), NULL, (cmd) }

static const struct default_binding default_bindings[] = {
	/* Terminal */
	DB_KEY_FN(XKB_KEY_Return, "super", action_exec_terminal),

	/* Close / quit */
	DB_KEY(XKB_KEY_q, "super", "winops -c"),
	DB_KEY(XKB_KEY_q, "super_shift", "quit"),

	/* Focus cycle */
	DB_KEY(XKB_KEY_j, "super", "focus -n"),
	DB_KEY(XKB_KEY_k, "super", "focus -p"),
	DB_KEY(XKB_KEY_Tab, "super", "focus -n"),

	/* Desk 0 + interactive move/resize keys */
	DB_KEY(XKB_KEY_0, "super", "desk -d 0"),
	DB_KEY(XKB_KEY_m, "super", "window-move"),
	DB_KEY(XKB_KEY_r, "super", "resize"),

	/* Pointer bindings */
	DB_PTR(BTN_LEFT, "super", "window-move"),
	DB_PTR(BTN_RIGHT, "super", "resize"),

	/* Container tab cycling via mouse side buttons (no modifier).
	 * BTN_SIDE / BTN_EXTRA are the back/forward side buttons present
	 * on many mice.  Scroll wheel (X11 buttons 4/5) generates axis
	 * events in Wayland and cannot be used as pointer bindings. */
	DB_PTR(BTN_SIDE, "", "container next"),
	DB_PTR(BTN_EXTRA, "", "container prev"),
};

#undef DB_KEY
#undef DB_KEY_FN
#undef DB_PTR

static uint32_t
modifier_mask(const char *name)
{
	uint32_t super = RIVER_SEAT_V1_MODIFIERS_MOD4;
	if (name == NULL || name[0] == '\0')
		return 0;
	if (strcmp(name, "super") == 0)
		return super;
	if (strcmp(name, "super_shift") == 0)
		return super | RIVER_SEAT_V1_MODIFIERS_SHIFT;
	return 0;
}

static void
register_default_bindings(struct cow_seat *seat)
{
	uint32_t super	     = RIVER_SEAT_V1_MODIFIERS_MOD4;
	uint32_t super_shift = RIVER_SEAT_V1_MODIFIERS_MOD4 |
	    RIVER_SEAT_V1_MODIFIERS_SHIFT;

	for (size_t i = 0;
	    i < sizeof(default_bindings) / sizeof(default_bindings[0]); i++) {
		const struct default_binding *db = &default_bindings[i];
		uint32_t      mods		 = modifier_mask(db->modifier);
		cow_action_fn fn		 = db->action ? db->action :
								action_execute_cmd_string;
		void	     *arg = db->action ? NULL : xstrdup(db->cmd);

		if (db->is_pointer)
			cow_pointer_binding_create(seat, db->button, mods, fn,
			    arg);
		else
			cow_keybinding_create(seat, db->key, mods, fn, arg,
			    /* repeatable = */ false);
	}

	/* Desk switching: Super+1..9 -> desk -d N
	 * Move-to-desk:   Super+Shift+1..9 -> window-move -k N
	 * Built dynamically because the cmd string is per-iteration. */
	xkb_keysym_t desk_syms[] = { XKB_KEY_1, XKB_KEY_2, XKB_KEY_3, XKB_KEY_4,
		XKB_KEY_5, XKB_KEY_6, XKB_KEY_7, XKB_KEY_8, XKB_KEY_9 };
	for (int i = 0; i < 9; i++) {
		char *switch_cmd, *move_cmd;
		xasprintf(&switch_cmd, "desk -d %d", i + 1);
		xasprintf(&move_cmd, "window-move -k %d", i + 1);
		cow_keybinding_create(seat, desk_syms[i], super,
		    action_execute_cmd_string, switch_cmd,
		    /* repeatable = */ false);
		cow_keybinding_create(seat, desk_syms[i], super_shift,
		    action_execute_cmd_string, move_cmd,
		    /* repeatable = */ false);
	}
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
	cow_wm_request_manage_dirty();
}
