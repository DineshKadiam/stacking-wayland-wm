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
#include <unistd.h>

#include "cow/config.h"
#include "cow/cmd.h"
#include "cow/ipc.h"
#include "cow/menu.h"
#include "cow/memory.h"
#include "cow/rule.h"
#include "cow/style.h"
#include "cow/wm.h"
#include "cow/view.h"
#include "cow/titlebar.h"
#include "cow/colour.h"
#include "cow/alias.h"

#define CHECK(x)                                                   \
	do {                                                       \
		if (!(x)) {                                        \
			fprintf(stderr, "%d: %s\n", __LINE__, #x); \
			abort();                                   \
		}                                                  \
	} while (0)

static void
reset(void)
{
	cow_menu_destroy_all();
	cow_rule_fini();
	cow_style_fini();
	cow_decor_reset_profiles();
	if (cow_config)
		cow_config_destroy(cow_config);
	cow_config = cow_config_create();
}

static void
write_config(const char *path, const char *text)
{
	FILE *file = fopen(path, "w");
	CHECK(file != NULL);
	CHECK(fputs(text, file) >= 0);
	CHECK(fclose(file) == 0);
}

int
main(int argc, char **argv)
{
	CHECK(argc == 2);
	wl_list_init(&cow_wm.menus);
	wl_list_init(&cow_wm.open_menus);
	wl_list_init(&cow_wm.menu_graveyard);
	wl_list_init(&cow_wm.views);
	wl_list_init(&cow_wm.outputs);
	wl_list_init(&cow_wm.containers);
	wl_list_init(&cow_wm.seats);
	char directory[] = "/tmp/cow-config-layers.XXXXXX";
	CHECK(mkdtemp(directory) != NULL);
	char *defaults, *user;
	xasprintf(&defaults, "%s/defaults", directory);
	xasprintf(&user, "%s/user", directory);
	write_config(defaults,
	    "set step 10\n"
	    "bind L+a winops -i\n"
	    "mouse T:0+middle window-shade\n"
	    "menu-add 'Root Menu' Original title\n"
	    "menu-add 'Root Menu' Quit quit\n"
	    "menu-add Other Keep title\n");
	write_config(user,
	    "set step 40\n"
	    "bind L+a winops -c\n"
	    "mouse -d T:0+middle\n"
	    "menu-add 'Root Menu' Custom title\n"
	    "menu-add 'Root Menu' Exit quit\n");
	for (int i = 0; i < 2; i++) {
		reset();
		CHECK(cow_ipc_load_config(defaults, user));
		CHECK(cow_config->step == 40);
		CHECK(wl_list_length(&cow_config->keyboard_bindings) == 1);
		struct cow_binding_entry *binding = wl_container_of(
		    cow_config->keyboard_bindings.next, binding, link);
		CHECK(strcmp(binding->action_string, "winops -c") == 0);
		CHECK(wl_list_empty(&cow_config->mouse_bindings));
		CHECK(strcmp(cow_ipc.config_path, user) == 0);
		struct cow_menu *menu = cow_menu_find("Root Menu");
		CHECK(menu != NULL && !menu->inherited);
		CHECK(wl_list_length(&menu->items) == 2);
		struct cow_menu_item *item = wl_container_of(menu->items.next,
		    item, link);
		CHECK(strcmp(item->label, "Custom") == 0);
		CHECK(wl_list_length(&cow_menu_find("Other")->items) == 1);
		if (i == 0)
			CHECK(cow_menu_find("Other")->inherited);
		/* Replaying an export over defaults must not duplicate menus.
		 */
		char *exported = cow_config_commands_string(cow_config);
		write_config(user, exported);
		xfree(exported);
	}
	reset();
	CHECK(cow_ipc_load_config(defaults, NULL));
	CHECK(cow_config->step == 10);
	reset();
	write_config(user, "set step 50\n");
	CHECK(cow_ipc_load_config(NULL, user));
	CHECK(cow_config->step == 50 && wl_list_empty(&cow_wm.menus));
	reset();
	CHECK(cow_ipc_load_config("/nonexistent/cow-defaults.conf", user));
	CHECK(cow_config->step == 50);
	reset();
	CHECK(cow_ipc_load_config(argv[1], NULL));
	CHECK(cow_menu_find("RootMenu") != NULL);
	CHECK(cow_menu_find("WindowOps") != NULL);
	CHECK(cow_ipc.startup_mode == false);
	CHECK(cow_config->menu_style.bg_gradient.mode != COW_GRADIENT_NONE);
	CHECK(cow_config->titlebar.active_gradient.mode != COW_GRADIENT_NONE);
	/* A legacy solid-colour config must not inherit optional theme effects.
	 */
	reset();
	write_config(user,
	    "menu-style bg 0x228B22\n"
	    "menu-style title.bg 0x228B33\n"
	    "decor -d default border.active 0x123456\n"
	    "decor -d default border.inactive 0x654321\n"
	    "decor -d default titlebar.active_colour 0x333333\n"
	    "decor -d default titlebar.inactive_colour 0x555555\n"
	    "decor -d default titlebar.fg_inactive 0xFFFFFF\n"
	    "decor -a default\n");
	CHECK(cow_ipc_load_config(argv[1], user));
	CHECK(cow_config->menu_style.bg_gradient.mode == COW_GRADIENT_NONE);
	CHECK(
	    cow_config->menu_style.title_bg_gradient.mode == COW_GRADIENT_NONE);
	CHECK(cow_config->border_active.gradient.mode == COW_GRADIENT_NONE);
	CHECK(cow_config->border_inactive.gradient.mode == COW_GRADIENT_NONE);
	CHECK(cow_config->titlebar.active_gradient.mode == COW_GRADIENT_NONE);
	CHECK(cow_config->titlebar.inactive_gradient.mode == COW_GRADIENT_NONE);
	CHECK(cow_config->container.tab_active_gradient.mode ==
	    COW_GRADIENT_NONE);
	CHECK(cow_config->container.tab_inactive_gradient.mode ==
	    COW_GRADIENT_NONE);
	CHECK(memcmp(cow_config->container.tab_inactive,
		  cow_config->titlebar.inactive_colour,
		  sizeof(cow_config->container.tab_inactive)) == 0);
	CHECK(memcmp(cow_config->container.tab_fg_inactive,
		  cow_config->titlebar.fg_inactive,
		  sizeof(cow_config->container.tab_fg_inactive)) == 0);
	char *exported = cow_config_commands_string(cow_config);
	write_config(user, exported);
	xfree(exported);
	reset();
	CHECK(cow_ipc_load_config(argv[1], user));
	CHECK(cow_config->menu_style.bg_gradient.mode == COW_GRADIENT_NONE);
	CHECK(cow_config->titlebar.inactive_gradient.mode == COW_GRADIENT_NONE);
	reset();
	write_config(user,
	    "menu-style -m RootMenu bg 0x228B22\n"
	    "menu-style bg.gradient HGradient 0x111111 0x222222\n"
	    "menu-style bg 0x228B22\n"
	    "decor -d default titlebar.active_gradient HGradient 0x111111 0x222222\n"
	    "decor -d default titlebar.active_colour 0x333333\n"
	    "decor -a default\n");
	CHECK(cow_ipc_load_config(argv[1], user));
	CHECK(cow_config->menu_style.bg_gradient.mode != COW_GRADIENT_NONE);
	CHECK(cow_menu_find("RootMenu")->style.bg_gradient.mode ==
	    COW_GRADIENT_NONE);
	CHECK(cow_menu_find("RootMenu")->style_overrides &
	    COW_MENU_STYLE_BG_GRADIENT);
	CHECK(cow_config->titlebar.active_gradient.mode != COW_GRADIENT_NONE);
	reset();
	/* Recognize wrapped actions before use and after reloading config. */
	write_config(user,
	    "alias maximize-window window-maximize -k\n"
	    "bind-decoration 0 script {\n"
	    "    if false { nop } else { maximize-window }\n"
	    "}\n");
	CHECK(cow_ipc_load_config(argv[1], user));
	CHECK(cow_view_maximize_button_index() == 0);
	struct cow_view view = { .icon_inverted_button = -1 };
	cow_view_set_maximized(&view, true);
	CHECK(view.icon_inverted_button == 0);
	reset();
	CHECK(cow_ipc_load_config(argv[1], user));
	CHECK(cow_view_maximize_button_index() == 0);
	CHECK(cow_alias_set("maximize-window", "nop"));
	CHECK(cow_view_maximize_button_index() == -1);
	CHECK(cow_alias_set("maximize-window", "window-maximize -T"));
	cow_view_set_maximized(&view, true);
	CHECK(view.icon_inverted_button == 0);
	wl_list_insert(&cow_wm.views, &view.wm_link);
	write_config(user, "bind-decoration 0 nop\n");
	CHECK(cow_ipc_load_config(argv[1], user));
	CHECK(cow_view_maximize_button_index() == -1);
	CHECK(view.icon_inverted_button == -1 && view.dec.dirty);
	wl_list_remove(&view.wm_link);
	CHECK(cow_dec_binding_can_maximize("script {\n"
	    "if self.container_split { winops -c } else { window-maximize -T }\n"
	    "}"));
	CHECK(cow_dec_binding_can_maximize("script {\nscript { window-maximize }\n}"));
	CHECK(!cow_dec_binding_can_maximize("script {\n"
	    "# window-maximize\nexec echo window-maximize\n}"));
	CHECK(!cow_dec_binding_can_maximize("bind L+m window-maximize"));
	CHECK(!cow_dec_binding_can_maximize("window-maximize-other"));
	CHECK(!cow_dec_binding_can_maximize("script {"));
	CHECK(cow_alias_set("recursive-button", "recursive-button"));
	CHECK(!cow_dec_binding_can_maximize("recursive-button"));
	CHECK(cow_alias_remove("recursive-button"));

	/* Maximization inverts the vector; only a pointer press sinks the bevel. */
	uint32_t pixels[100 * 24] = { 0 };
	uint32_t normal_pixels[100 * 24];
	struct cow_titlebar_config titlebar = cow_config->titlebar;
	titlebar.button_style = COW_TITLEBAR_BUTTON_STYLE_FVWM;
	titlebar.buttons[0].active_relief = COW_RELIEF_RAISED;
	titlebar.buttons[0].inactive_relief = COW_RELIEF_RAISED;
	struct cow_titlebar_render_ctx render = {
	    .pixels = pixels, .stride_px = 100, .frame_w = 100, .frame_h = 24,
	    .tb_rect = { 0, 0, 100, 24 }, .title = "", .tb_cfg = &titlebar,
	    .pressed_button = -1, .icon_inverted_button = -1,
	};
	for (int active = 0; active <= 1; active++) {
		render.active = active;
		const float *base = active ? titlebar.active_colour :
		    titlebar.inactive_colour;
		float light[4], dark[4];
		cow_colour_lighten(light, base);
		cow_colour_darken(dark, base);
		render.pressed_button = -1;
		render.icon_inverted_button = -1;
		cow_titlebar_render(&render);
		CHECK(pixels[80] == cow_colour_to_pixel(light));
		memcpy(normal_pixels, pixels, sizeof pixels);
		render.icon_inverted_button = 0;
		cow_titlebar_render(&render);
		CHECK(pixels[80] == cow_colour_to_pixel(light));
		CHECK(memcmp(normal_pixels, pixels, sizeof pixels) != 0);
		render.pressed_button = 0;
		cow_titlebar_render(&render);
		CHECK(pixels[80] == cow_colour_to_pixel(dark));
	}
	cow_config_destroy(cow_config);
	cow_config = NULL;
	xfree(cow_ipc.config_path);
	cow_ipc.config_path = NULL;
	CHECK(unlink(defaults) == 0 && unlink(user) == 0);
	CHECK(rmdir(directory) == 0);
	xfree(defaults);
	xfree(user);
	return 0;
}
