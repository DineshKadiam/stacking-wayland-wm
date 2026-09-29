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


/* Exercise icon appearance without a compositor or real Wayland objects. */
#include "../src/view.c"
#include <stdio.h>

#define CHECK(expr) do { \
	if (!(expr)) { \
		fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); \
		abort(); \
	} \
} while (0)

int
main(void)
{
	cow_config = cow_config_create();
	struct cow_view view = {0};
	cow_view_cfg_init_from_global(&view.cfg);
	struct cmd command = {0};
	struct cmd_q *queue = cmdq_new();
	queue->cmd = &command;
	queue->target.window = &view;
	queue->in_style = true; /* Parse visual settings without live refresh. */
	const float *bg, *fg;
	icon_colours(&view, &bg, &fg);
	CHECK(bg == view.cfg.titlebar.inactive_colour && fg == view.cfg.titlebar.fg_inactive);
	CHECK(cmd_set_apply_decor("icon.background", "0x60a0c0", queue) ==
	    CMD_RETURN_NORMAL);
	CHECK(cmd_set_apply_decor("icon.foreground", "0x123456", queue) ==
	    CMD_RETURN_NORMAL);
	icon_colours(&view, &bg, &fg);
	CHECK(bg == view.cfg.icon_background.rgba && fg == view.cfg.icon_foreground.rgba);
	CHECK(cmd_set_apply_decor("icon.background", "default", queue) ==
	    CMD_RETURN_NORMAL);
	CHECK(cmd_set_apply_decor("icon.foreground", "default", queue) ==
	    CMD_RETURN_NORMAL);
	cow_update_reset();
	cow_view_icon_set_hovered(&view, true);
	CHECK(cow_update_take() == COW_UPDATE_MANAGE);
	cow_view_icon_set_hovered(&view, true);
	CHECK(cow_update_take() == COW_UPDATE_NONE);
	CHECK(cow_wm.focused_view == NULL && view.flags == 0);
	icon_colours(&view, &bg, &fg);
	CHECK(bg == view.cfg.titlebar.active_colour);
	CHECK(fg == view.cfg.titlebar.fg_active);
	CHECK(cmd_set_apply_decor("icon.active.background", "0x12345680", queue) ==
	    CMD_RETURN_NORMAL);
	CHECK(cmd_set_apply_decor("icon.active.foreground", "0xabcdef", queue) ==
	    CMD_RETURN_NORMAL);
	icon_colours(&view, &bg, &fg);
	CHECK(bg == view.cfg.icon_active_background.rgba && bg[3] > 0.49f && bg[3] < 0.51f);
	CHECK(fg == view.cfg.icon_active_foreground.rgba);
	CHECK(cmd_set_apply_decor("icon.active.background", "default", queue) ==
	    CMD_RETURN_NORMAL);
	CHECK(cmd_set_apply_decor("icon.active.foreground", "default", queue) ==
	    CMD_RETURN_NORMAL);
	icon_colours(&view, &bg, &fg);
	CHECK(bg == view.cfg.titlebar.active_colour && fg == view.cfg.titlebar.fg_active);

	/* Global propagation copies the colour and its optional-state flag. */
	cow_config->icon_active_background.set = true;
	cow_config->icon_active_foreground.set = true;
	cow_colour_convert(cow_config->icon_active_background.rgba, 0x123456);
	cow_colour_convert(cow_config->icon_active_foreground.rgba, 0xabcdef);
	cow_view_cfg_apply_global_option(&view.cfg, COW_VIEW_CFG_ICON_ACTIVE_BACKGROUND);
	cow_view_cfg_apply_global_option(&view.cfg, COW_VIEW_CFG_ICON_ACTIVE_FOREGROUND);
	CHECK(view.cfg.icon_active_background.set && view.cfg.icon_active_foreground.set);
	CHECK(memcmp(view.cfg.icon_active_background.rgba, cow_config->icon_active_background.rgba,
	    sizeof(view.cfg.icon_active_background.rgba)) == 0);
	cow_view_cfg_set_override(&view.cfg, COW_VIEW_CFG_ICON_ACTIVE_BACKGROUND);
	cow_config->icon_active_background.set = false;
	cow_view_cfg_apply_global_option(&view.cfg, COW_VIEW_CFG_ICON_ACTIVE_BACKGROUND);
	CHECK(view.cfg.icon_active_background.set);
	cow_view_icon_set_hovered(&view, false);
	CHECK(cow_update_take() == COW_UPDATE_MANAGE);
	icon_colours(&view, &bg, &fg);
	CHECK(bg == view.cfg.titlebar.inactive_colour && fg == view.cfg.titlebar.fg_inactive);
	cow_config->icon_background.set = true;
	cow_config->icon_foreground.set = true;
	cow_colour_convert(cow_config->icon_background.rgba, 0x60a0c0);
	cow_view_cfg_apply_global_option(&view.cfg, COW_VIEW_CFG_ICON_BACKGROUND);
	cow_view_cfg_apply_global_option(&view.cfg, COW_VIEW_CFG_ICON_FOREGROUND);
	CHECK(view.cfg.icon_background.set && view.cfg.icon_foreground.set);
	CHECK(memcmp(view.cfg.icon_background.rgba, cow_config->icon_background.rgba,
	    sizeof(view.cfg.icon_background.rgba)) == 0);
	cow_config->icon_background.set = false;
	cow_config->icon_foreground.set = false;
	cow_view_cfg_apply_global_option(&view.cfg, COW_VIEW_CFG_ICON_BACKGROUND);
	cow_view_cfg_apply_global_option(&view.cfg, COW_VIEW_CFG_ICON_FOREGROUND);
	icon_colours(&view, &bg, &fg);
	CHECK(bg == view.cfg.titlebar.inactive_colour && fg == view.cfg.titlebar.fg_inactive);
	view.icon_hovered = true;
	cow_view_icon_destroy(&view);
	CHECK(!view.icon_hovered);
	CHECK(ARRAY_LENGTH(&queue->causes) == 0);
	cmdq_free(queue);
	cow_view_cfg_fini(&view.cfg);
	cow_config_destroy(cow_config);
	cow_config = NULL;
	return 0;
}
