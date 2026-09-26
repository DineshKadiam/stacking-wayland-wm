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
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "cow/keybinding.h"
#include "cow/memory.h"
#include "cow/seat.h"
#include "cow/shm_util.h"
#include "cow/update.h"
#include "cow/util.h"
#include "cow/virtual_keyboard.h"
#include "cow/wm.h"

#include "river-virtual-keyboard-v1-client-protocol.h"
#include "river-window-management-v1-client-protocol.h"
#include "virtual-keyboard-unstable-v1-client-protocol.h"

/* Leave headroom in River's bounded queue, including implicit events. */
#define MAX_STROKES_PER_MANAGE 16

static struct zwp_virtual_keyboard_manager_v1 *virtual_manager;
static struct river_virtual_keyboard_manager_v1 *sync_manager;
static uint32_t virtual_global, sync_global;
static unsigned pending_strokes;

static const struct {
	uint32_t flag;
	const char *name;
} modifier_names[] = {
	{ RIVER_SEAT_V1_MODIFIERS_SHIFT, "Shift" },
	{ RIVER_SEAT_V1_MODIFIERS_CTRL, "Control" },
	{ RIVER_SEAT_V1_MODIFIERS_MOD1, "Mod1" },
	{ RIVER_SEAT_V1_MODIFIERS_MOD4, "Mod4" },
	{ RIVER_SEAT_V1_MODIFIERS_MOD5, "Mod5" },
};

struct xkb_keymap *
cow_send_key_keymap(struct cow_send_keymap *keys, const char *spec,
    uint32_t *code, uint32_t *mask, char **error)
{
	*error = NULL;
	*mask = 0;
	char *copy = xstrdup(spec);
	char *key = strchr(copy, '+');
	uint32_t flags = 0;
	if (key != NULL) {
		*key++ = '\0';
		if (!cow_binding_parse_modifier_mask(copy, &flags)) {
			xfree(copy);
			xasprintf(error, "invalid modifiers in %s", spec);
			return NULL;
		}
	} else {
		key = copy;
	}
	xkb_keysym_t sym = xkb_keysym_from_name(key, XKB_KEYSYM_NO_FLAGS);
	xfree(copy);
	if (sym == XKB_KEY_NoSymbol) {
		xasprintf(error, "unknown keysym in %s", spec);
		return NULL;
	}
	unsigned slot;
	for (slot = 0; slot < keys->count; slot++) {
		if (keys->symbols[slot] == sym)
			break;
	}
	if (slot == COW_SEND_KEY_SYMBOLS) {
		xasprintf(error, "virtual keyboard symbol limit reached");
		return NULL;
	}
	unsigned count = keys->count + (slot == keys->count);
	/* Each formatted entry is shorter than 128 bytes. */
	char *symbols = xcalloc(count, 128);
	size_t used = 0;
	for (unsigned i = 0; i < count; i++) {
		xkb_keysym_t value = i == slot ? sym : keys->symbols[i];
		used += snprintf(symbols + used, count * 128 - used,
		    "key <K%03u> { type=\"TWO_LEVEL\", "
		    "[ 0x%08x, 0x%08x ] }; ", i, value,
		    xkb_keysym_to_upper(value));
	}
	char *codes = xcalloc(count, 32);
	used = 0;
	for (unsigned i = 0; i < count; i++)
		used += snprintf(codes + used, count * 32 - used,
		    "<K%03u>=%u; ", i, i + 8);

	/*
	 * A private map makes named keysyms independent of the user's layout.
	 * Shift uppercases letters; other modifiers remain available to clients.
	 * Never reassign a keycode: X11 clients may look up earlier events using
	 * the newest map. Use the standard types and compatibility rules:
	 * Xwayland needs complete XKB components, even though libxkbcommon
	 * accepts smaller maps.
	 */
	char *text;
	xasprintf(&text,
	    "xkb_keymap { "
	    "xkb_keycodes \"cow\" { minimum=8; maximum=255; "
	    "%s <SHFT>=250; <CTRL>=251; <ALT>=252; "
	    "<LOGO>=253; <LVL3>=254; }; "
	    "xkb_types \"cow\" { include \"complete\" }; "
	    "xkb_compatibility \"cow\" { include \"complete\" }; "
	    "xkb_symbols \"cow\" { "
	    "%s "
	    "key <SHFT> { [ Shift_L ] }; modifier_map Shift { <SHFT> }; "
	    "key <CTRL> { [ Control_L ] }; modifier_map Control { <CTRL> }; "
	    "key <ALT> { [ Alt_L ] }; modifier_map Mod1 { <ALT> }; "
	    "key <LOGO> { [ Super_L ] }; modifier_map Mod4 { <LOGO> }; "
	    "key <LVL3> { [ ISO_Level3_Shift ] }; modifier_map Mod5 { <LVL3> }; "
	    "}; };", codes, symbols);
	xfree(codes);
	xfree(symbols);
	struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	struct xkb_keymap *map = context != NULL ?
	    xkb_keymap_new_from_string(context, text,
	    XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS) : NULL;
	xfree(text);
	if (context != NULL)
		xkb_context_unref(context);
	if (map == NULL) {
		xasprintf(error, "cannot create virtual keyboard keymap");
		return NULL;
	}
	for (size_t i = 0; i < sizeof(modifier_names) / sizeof(modifier_names[0]); i++) {
		if (!(flags & modifier_names[i].flag))
			continue;
		xkb_mod_index_t index = xkb_keymap_mod_get_index(map,
		    modifier_names[i].name);
		if (index == XKB_MOD_INVALID || index >= 32) {
			xkb_keymap_unref(map);
			xasprintf(error, "cannot map virtual keyboard modifier");
			return NULL;
		}
		*mask |= 1u << index;
	}
	keys->symbols[slot] = sym;
	keys->count = count;
	*code = slot;
	return map;
}

bool
cow_virtual_keyboard_bind(struct wl_registry *registry, uint32_t name,
    const char *interface)
{
	if (strcmp(interface, zwp_virtual_keyboard_manager_v1_interface.name) == 0) {
		if (virtual_manager == NULL) {
			virtual_manager = wl_registry_bind(registry, name,
			    &zwp_virtual_keyboard_manager_v1_interface, 1);
			virtual_global = name;
		}
		return true;
	}
	if (strcmp(interface, river_virtual_keyboard_manager_v1_interface.name) == 0) {
		if (sync_manager == NULL) {
			sync_manager = wl_registry_bind(registry, name,
			    &river_virtual_keyboard_manager_v1_interface, 1);
			sync_global = name;
		}
		return true;
	}
	return false;
}

void
cow_virtual_keyboard_remove(uint32_t name)
{
	if ((virtual_manager != NULL && name == virtual_global) ||
	    (sync_manager != NULL && name == sync_global)) {
		struct cow_seat *seat;
		wl_list_for_each(seat, &cow_wm.seats, wm_link)
			cow_virtual_keyboard_destroy_seat(seat);
	}
	if (virtual_manager != NULL && name == virtual_global) {
		zwp_virtual_keyboard_manager_v1_destroy(virtual_manager);
		virtual_manager = NULL;
	}
	if (sync_manager != NULL && name == sync_global) {
		river_virtual_keyboard_manager_v1_destroy(sync_manager);
		sync_manager = NULL;
	}
}

void
cow_virtual_keyboard_fini(void)
{
	cow_virtual_keyboard_remove(virtual_global);
	cow_virtual_keyboard_remove(sync_global);
	pending_strokes = 0;
}

void
cow_virtual_keyboard_manage_finish(void)
{
	pending_strokes = 0;
}

static bool
send_keymap(struct zwp_virtual_keyboard_v1 *keyboard, struct xkb_keymap *map)
{
	char *text = xkb_keymap_get_as_string(map, XKB_KEYMAP_FORMAT_TEXT_V1);
	if (text == NULL)
		return false;
	size_t length = strlen(text) + 1;
	int fd = cow_shm_file_create("cow-send-key", length);
	void *mapping = fd >= 0 ?
	    mmap(NULL, length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0) :
	    MAP_FAILED;
	if (mapping == MAP_FAILED) {
		if (fd >= 0)
			close(fd);
		xfree(text);
		return false;
	}
	memcpy(mapping, text, length);
	munmap(mapping, length);
	xfree(text);
	zwp_virtual_keyboard_v1_keymap(keyboard, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1,
	    fd, (uint32_t)length);
	close(fd);
	return true;
}

void
cow_virtual_keyboard_destroy_seat(struct cow_seat *seat)
{
	xfree(seat->virtual_keymap);
	seat->virtual_keymap = NULL;
	if (seat->virtual_keyboard != NULL) {
		zwp_virtual_keyboard_v1_destroy(seat->virtual_keyboard);
		seat->virtual_keyboard = NULL;
	}
	if (seat->virtual_keyboard_seat != NULL) {
		wl_seat_release(seat->virtual_keyboard_seat);
		seat->virtual_keyboard_seat = NULL;
	}
}

/* Prepare before applications map, including on seats without real keyboards. */
void
cow_virtual_keyboard_prepare(void)
{
	struct cow_seat *seat = cow_seat_primary();
	if (virtual_manager == NULL || sync_manager == NULL || seat == NULL ||
	    seat->removed || seat->wl_seat_name == 0 || seat->virtual_keyboard != NULL)
		return;
	char *error;
	uint32_t code, mask;
	seat->virtual_keymap = xcalloc(1, sizeof(*seat->virtual_keymap));
	struct xkb_keymap *map = cow_send_key_keymap(seat->virtual_keymap,
	    "a", &code, &mask, &error);
	if (map == NULL) {
		xfree(error);
		cow_virtual_keyboard_destroy_seat(seat);
		return;
	}
	seat->virtual_keyboard_seat = wl_registry_bind(cow_wm.registry,
	    seat->wl_seat_name, &wl_seat_interface, 5);
	seat->virtual_keyboard =
	    zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(
	    virtual_manager, seat->virtual_keyboard_seat);
	river_virtual_keyboard_manager_v1_sync_keyboard(sync_manager,
	    seat->virtual_keyboard);
	if (!send_keymap(seat->virtual_keyboard, map))
		cow_virtual_keyboard_destroy_seat(seat);
	xkb_keymap_unref(map);
}

bool
cow_virtual_keyboard_send_key(const char *spec, char **error)
{
	*error = NULL;
	if (virtual_manager == NULL || sync_manager == NULL) {
		xasprintf(error, "River's synchronized virtual keyboard "
		    "protocol is unavailable");
		return false;
	}
	struct cow_seat *seat = cow_seat_primary();
	if (seat == NULL || seat->removed || seat->wl_seat_name == 0) {
		xasprintf(error, "no keyboard seat available");
		return false;
	}
	if (pending_strokes >= MAX_STROKES_PER_MANAGE) {
		xasprintf(error, "too many keys in one manage cycle");
		return false;
	}
	if (seat->virtual_keymap == NULL) {
		xasprintf(error, "virtual keyboard is not initialized");
		return false;
	}
	uint32_t code, modifiers;
	struct xkb_keymap *map = cow_send_key_keymap(seat->virtual_keymap,
	    spec, &code, &modifiers, error);
	if (map == NULL)
		return false;
	struct zwp_virtual_keyboard_v1 *keyboard = seat->virtual_keyboard;
	if (keyboard == NULL || !send_keymap(keyboard, map)) {
		xkb_keymap_unref(map);
		xasprintf(error, "cannot initialize virtual keyboard keymap");
		return false;
	}
	xkb_keymap_unref(map);
	uint32_t now = (uint32_t)cow_mono_ms();
	zwp_virtual_keyboard_v1_modifiers(keyboard, modifiers, 0, 0, 0);
	zwp_virtual_keyboard_v1_key(keyboard, now, code,
	    WL_KEYBOARD_KEY_STATE_PRESSED);
	zwp_virtual_keyboard_v1_key(keyboard, now, code,
	    WL_KEYBOARD_KEY_STATE_RELEASED);
	zwp_virtual_keyboard_v1_modifiers(keyboard, 0, 0, 0, 0);
	pending_strokes++;
	cow_update_request(COW_UPDATE_MANAGE);
	return true;
}
