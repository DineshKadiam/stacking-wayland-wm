/* Exercise live binding invalidation and validation without a compositor. */
#undef NDEBUG
#include <assert.h>
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
	cow_config_destroy(cow_config);
	return 0;
}
