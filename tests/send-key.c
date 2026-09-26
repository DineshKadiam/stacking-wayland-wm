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

#include "cow/memory.h"
#include "cow/virtual_keyboard.h"

#define CHECK(expr) do { if (!(expr)) { \
	fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); \
	abort(); \
} } while (0)

static void
check(const char *spec, xkb_keysym_t expected, const char *modifier)
{
	struct cow_send_keymap keys = {0};
	uint32_t code, mask;
	char *error;
	struct xkb_keymap *map = cow_send_key_keymap(&keys, spec, &code,
	    &mask, &error);
	CHECK(map != NULL);
	CHECK(error == NULL);
	struct xkb_state *state = xkb_state_new(map);
	CHECK(state != NULL);
	xkb_state_update_mask(state, mask, 0, 0, 0, 0, 0);
	CHECK(xkb_state_key_get_one_sym(state, code + 8) == expected);
	if (modifier != NULL)
		CHECK(xkb_state_mod_name_is_active(state, modifier,
		    XKB_STATE_MODS_DEPRESSED) > 0);
	else
		CHECK(mask == 0);
	/* Every generated map must survive the protocol's text round trip. */
	char *text = xkb_keymap_get_as_string(map, XKB_KEYMAP_FORMAT_TEXT_V1);
	CHECK(text != NULL);
	struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	struct xkb_keymap *roundtrip = xkb_keymap_new_from_string(context,
	    text, XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
	CHECK(roundtrip != NULL);
	xkb_keymap_unref(roundtrip);
	xkb_context_unref(context);
	xfree(text);
	xkb_state_unref(state);
	xkb_keymap_unref(map);
}

static void
check_stable_codes(void)
{
	struct cow_send_keymap keys = {0};
	const char *names[] = { "h", "i", "Return", "U03BB" };
	xkb_keysym_t expected[] = { XKB_KEY_h, XKB_KEY_i,
	    XKB_KEY_Return, 0x010003bb };
	uint32_t codes[4], mask;
	char *error;
	for (unsigned i = 0; i < 4; i++) {
		struct xkb_keymap *map = cow_send_key_keymap(&keys, names[i],
		    &codes[i], &mask, &error);
		CHECK(map != NULL && error == NULL);
		struct xkb_state *state = xkb_state_new(map);
		CHECK(state != NULL);
		/* Later maps must still decode every earlier queued key. */
		for (unsigned j = 0; j <= i; j++)
			CHECK(xkb_state_key_get_one_sym(state, codes[j] + 8) ==
			    expected[j]);
		xkb_state_unref(state);
		xkb_keymap_unref(map);
	}
	uint32_t code;
	struct xkb_keymap *map = cow_send_key_keymap(&keys, "C+h", &code,
	    &mask, &error);
	CHECK(map != NULL && code == codes[0] && keys.count == 4);
	xkb_keymap_unref(map);
	/* Exhaustion must fail without changing already assigned symbols. */
	while (keys.count < COW_SEND_KEY_SYMBOLS) {
		unsigned i = keys.count++;
		keys.symbols[i] = 0x01001000 + i;
	}
	CHECK(cow_send_key_keymap(&keys, "F12", &code, &mask, &error) == NULL);
	CHECK(error != NULL && keys.count == COW_SEND_KEY_SYMBOLS);
	xfree(error);
	map = cow_send_key_keymap(&keys, "h", &code, &mask, &error);
	CHECK(map != NULL && code == codes[0] && error == NULL);
	xkb_keymap_unref(map);
}

int
main(void)
{
	check_stable_codes();
	check("a", XKB_KEY_a, NULL);
	check("A", XKB_KEY_A, NULL);
	check("S+a", XKB_KEY_A, "Shift");
	check("C+c", XKB_KEY_c, "Control");
	check("AS+Tab", XKB_KEY_Tab, "Mod1");
	check("L+Left", XKB_KEY_Left, "Mod4");
	check("5+Return", XKB_KEY_Return, "Mod5");
	check("M+F4", XKB_KEY_F4, "Mod1");
	check("0+plus", XKB_KEY_plus, NULL);
	check("exclam", XKB_KEY_exclam, NULL);
	check("U03BB", 0x010003bb, NULL);
	const char *invalid[] = { "", "NotAKeysym", "Q+a", "+a",
	    "C+", "C++", "W:C+a", "C-30" };
	for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
		struct cow_send_keymap keys = {0};
		uint32_t code, mask;
		char *error;
		CHECK(cow_send_key_keymap(&keys, invalid[i], &code, &mask,
		    &error) == NULL);
		CHECK(error != NULL);
		xfree(error);
	}
	return 0;
}
