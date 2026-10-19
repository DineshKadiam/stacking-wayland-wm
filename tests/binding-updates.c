/* Exercise live binding invalidation and validation without a compositor. */
#undef NDEBUG
#include <assert.h>
#include <unistd.h>
#include "cow/notification.h"
#include "../src/keybinding.c"

static bool
run(const char *text)
{
	struct cmd_list *list = NULL;
	char *cause = NULL;
	assert(cmd_string_parse(text, &list, NULL, 0, &cause) == 0);
	struct cmd_q *queue = cmdq_new();
	queue->client_fd = -1;
	cmdq_run(queue, list);
	bool ok = !queue->failed;
	cmdq_free(queue);
	cmd_list_free(list);
	return ok;
}

int
main(void)
{
	cow_config = cow_config_create();
	wl_list_init(&cow_wm.seats);
	wl_list_init(&cow_wm.outputs);
	wl_list_init(&cow_wm.open_menus);
	wl_list_init(&cow_wm.menu_graveyard);
	cow_ipc.startup_mode = true;
	cow_rebind_all_seats();
	assert(!bindings_dirty);
	cow_ipc.startup_mode = false;
	assert(run("mouse CL+left 'scroll -r'"));
	assert(bindings_dirty);
	cow_flush_binding_updates();
	assert(!bindings_dirty);
	assert(run("mouse CL+left 'exec foot'"));
	assert(bindings_dirty);
	assert(wl_list_length(&cow_config->mouse_bindings) == 1);
	assert(!run("mouse c+l 'Left scroll -r'"));
	assert(!run("mouse CL+bogus 'scroll -r'"));
	assert(!run("mouse Q+left 'scroll -r'"));
	assert(!run("mouse bogus:CL+left 'scroll -r'"));
	assert(wl_list_length(&cow_config->mouse_bindings) == 1);
	assert(run("mouse -d 'c+l'"));
	assert(run("mouse -d CL+left"));
	assert(wl_list_empty(&cow_config->mouse_bindings));

	uint32_t button, modifiers, context;
	const char *error;
	assert(cow_binding_parse_mouse("CL+Left", &button, &modifiers,
	    &context, &error));
	assert(button == BTN_LEFT);
	assert(modifiers == (RIVER_SEAT_V1_MODIFIERS_CTRL |
	    RIVER_SEAT_V1_MODIFIERS_MOD4));
	assert(context == COW_BIND_CONTEXT_GLOBAL);
	uint32_t expected = modifiers;
	assert(cow_binding_parse_mouse("cL+LEFT", &button, &modifiers,
	    &context, &error));
	assert(button == BTN_LEFT && modifiers == expected);
	xkb_keysym_t keysym;
	assert(parse_key_string("cl+Return", &keysym, &modifiers, &context));
	assert(keysym == XKB_KEY_Return && modifiers == expected);
	uint32_t upper, lower;
	assert(cow_binding_parse_modifier_mask("LSAMC5", &upper));
	assert(cow_binding_parse_modifier_mask("lsamc5", &lower));
	assert(upper == lower);

	assert(cow_config->binding_sequence_hints);
	assert(run("set binding.sequence_hints false"));
	assert(!cow_config->binding_sequence_hints);
	assert(run("set binding.sequence_hints true"));
	struct cow_keymap prefix = {.prefix = "L+c"};
	struct cow_keymap nested = {.prefix = "L+c,0+t"};
	struct cow_seat seat = {.active_keymap = &prefix};
	wl_list_init(&seat.keybindings);
	struct cow_keybinding rotate = {
		.keymap = &prefix, .key_label = "0+r", .keysym = XKB_KEY_r,
		.context_mask = COW_BIND_CONTEXT_GLOBAL,
		.action_arg = "window-container -o clockwise",
	};
	struct cow_keybinding title = {
		.keymap = &prefix, .key_label = "0+t", .keysym = XKB_KEY_t,
		.context_mask = COW_BIND_CONTEXT_GLOBAL, .next_keymap = &nested,
	};
	wl_list_insert(&seat.keybindings, &rotate.link);
	wl_list_insert(&seat.keybindings, &title.link);
	char *hint = binding_sequence_hint_text(&seat);
	assert(strcmp(hint, "L+c …\n0+r    window-container -o clockwise"
	    "\n0+t    More keys …") == 0);
	cow_chord_hint_show(&seat, hint);
	assert(seat.chord_hint != NULL);
	assert(!cow_menu_is_open());
	assert(!cow_menu_process_input());
	xfree(hint);
	binding_sequence_reset(&seat);
	assert(seat.chord_hint == NULL && seat.active_keymap == NULL);
	cow_menu_render_all();
	assert(wl_list_empty(&cow_wm.menu_graveyard));
	cow_config->binding_sequence_hints = false;
	cow_chord_hint_show(&seat, "hidden");
	assert(seat.chord_hint == NULL);

	assert(run("notify -n test -t 0 'Persistent notice'"));
	assert(cow_notification_next_ms() == -1);
	assert(run("notify -n test -t 1000 'Replacement'"));
	assert(cow_notification_next_ms() >= 0);
	assert(cow_notification_next_ms() <= 1000);
	assert(cow_notification_dismiss("test"));
	assert(!cow_notification_dismiss("test"));
	assert(run("notify 'Default notice'"));
	assert(run("notify -n default -d"));
	assert(cow_notification_next_ms() == -1);
	assert(!run("notify -t -1 'Bad timeout'"));
	assert(!run("notify -d"));
	assert(!run("notify -n test -d 'Unexpected text'"));
	assert(!run("notify"));
	assert(run("notify -n expires -t 1 'Short notice'"));
	usleep(20000);
	cow_notification_render_all();
	assert(!cow_notification_dismiss("expires"));
	assert(!cow_menu_is_open());
	cow_config->binding_sequence_hints = true;
	cow_chord_hint_show(&seat, "private");
	assert(run("notify -n default -d"));
	assert(seat.chord_hint != NULL);
	cow_notification_clear();
	assert(seat.chord_hint == NULL);
	cow_menu_render_all();
	cow_config_destroy(cow_config);
	return 0;
}
