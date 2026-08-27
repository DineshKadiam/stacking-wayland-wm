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
#include <sys/mman.h>
#include <unistd.h>

#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#include "cow/cmd.h"
#include "cow/log.h"
#include "cow/cmd-filter.h"
#include "cow/compat.h"
#include "cow/memory.h"
#include "cow/output.h"
#include "cow/pick.h"
#include "cow/seat.h"
#include "cow/shm.h"
#include "cow/view.h"
#include "cow/update.h"
#include "cow/wm.h"

#include "cursor-shape-v1-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

static enum cmd_retval cmd_pick_window_exec(struct cmd *, struct cmd_q *);

struct cmd_entry cmd_pick_window_entry = {
	"pick-window",
	"F:",
	1,
	-1,
	"pick-window [-F expr] <command...>",
	cmd_pick_window_exec,
	0,
};

struct pick_overlay {
	struct cow_output	     *output;
	struct wl_surface	     *surface;
	struct zwlr_layer_surface_v1 *layer_surface;
	struct cow_shm_buffer	     *buffer;
};

struct pick_session {
	char		    *cmd_str;
	struct pick_overlay *overlays;
	int		     n_overlays;
	struct wp_cursor_shape_device_v1 *cursor_shape_dev;
	struct wl_keyboard  *keyboard;
	struct xkb_keymap   *keymap;
	struct xkb_state    *kb_state;
	uint32_t	     last_serial;
	bool		     have_cursor;
	struct cow_output   *cur_output;
	int		     cur_sx;
	int		     cur_sy;
	struct cmd_filter    filter;
};

struct deferred_pick_command {
	struct wl_list	  link;
	char		 *cmd_str;
	struct cow_view	 *target;
	struct cow_desk	 *target_desk;
	struct cow_output *target_output;
	struct cow_seat	 *seat;
};

static struct pick_session *active_session;
static struct wl_list	deferred_pick_commands;
static bool		deferred_pick_commands_ready;

static void pick_session_destroy(struct pick_session *);
static void pick_session_fire(struct pick_session *);
static char *pick_command_string(struct args *);
static void pick_session_seed_pointer(struct pick_session *);
static void pick_commands_init(void);
static void pick_command_defer(char *, struct cow_view *);
static void pick_command_run(struct deferred_pick_command *);

static struct cow_view *
pick_view_at(struct pick_session *s, int x, int y)
{
	struct cow_view *best = NULL;
	struct cow_view *v;
	wl_list_for_each(v, &cow_wm.views, wm_link)
	{
		if (v->closed || v->output == NULL)
			continue;
		if (!cow_view_visible(v, v->output))
			continue;
		struct cow_box fb = cow_view_frame_box(v);
		if (x < fb.x || x >= fb.x + fb.width || y < fb.y ||
		    y >= fb.y + fb.height)
			continue;
		if (best == NULL || v->raise_seq > best->raise_seq)
			best = v;
	}
	if (best != NULL) {
		bool matches = false;
		if (!cmd_filter_view(&s->filter, best, &matches))
			return NULL;
		if (!matches)
			return NULL;
	}
	return best;
}

static void
ls_configure(void *data, struct zwlr_layer_surface_v1 *lsurf, uint32_t serial,
    __unused uint32_t w, __unused uint32_t h)
{
	struct pick_overlay *ov = data;
	zwlr_layer_surface_v1_ack_configure(lsurf, serial);
	if (ov->surface != NULL && ov->buffer != NULL) {
		wl_surface_attach(ov->surface, ov->buffer->buffer, 0, 0);
		wl_surface_damage_buffer(ov->surface, 0, 0, ov->buffer->width,
		    ov->buffer->height);
		wl_surface_commit(ov->surface);
	}
}

static void
ls_closed(__unused void *data, __unused struct zwlr_layer_surface_v1 *lsurf)
{
	cow_pick_cancel_active();
}

static const struct zwlr_layer_surface_v1_listener ls_listener = {
	.configure = ls_configure,
	.closed	   = ls_closed,
};

static void
kb_keymap(void *data, __unused struct wl_keyboard *kb, uint32_t format,
    int32_t fd, uint32_t size)
{
	struct pick_session *s = data;
	if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
		close(fd);
		return;
	}
	char *map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
	close(fd);

	if (map == MAP_FAILED)
		return;
	struct xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	if (ctx == NULL) {
		munmap(map, size);
		return;
	}
	s->keymap = xkb_keymap_new_from_string(ctx, map,
	    XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
	munmap(map, size);
	xkb_context_unref(ctx);
	if (s->keymap != NULL)
		s->kb_state = xkb_state_new(s->keymap);
}

static void
kb_enter(__unused void *data, __unused struct wl_keyboard *kb,
    __unused uint32_t serial, __unused struct wl_surface *surf,
    __unused struct wl_array *keys)
{
	return;
}

static void
kb_leave(__unused void *data, __unused struct wl_keyboard *kb,
    __unused uint32_t serial, __unused struct wl_surface *surf)
{
	return;
}

static void
kb_key(void *data, __unused struct wl_keyboard *kb, __unused uint32_t serial,
    __unused uint32_t time, uint32_t key, uint32_t state)
{
	(void)kb;
	(void)serial;
	(void)time;
	struct pick_session *s = data;
	if (state != WL_KEYBOARD_KEY_STATE_PRESSED)
		return;
	if (s->kb_state == NULL)
		return;
	xkb_keysym_t sym = xkb_state_key_get_one_sym(s->kb_state, key + 8);
	if (sym == XKB_KEY_Escape)
		cow_pick_cancel_active();
}

static void
kb_modifiers(void *data, __unused struct wl_keyboard *kb,
    __unused uint32_t serial, uint32_t depressed, uint32_t latched,
    uint32_t locked, uint32_t group)
{
	struct pick_session *s = data;
	if (s->kb_state != NULL)
		xkb_state_update_mask(s->kb_state, depressed, latched, locked,
		    0, 0, group);
}

static void
kb_repeat_info(__unused void *data, __unused struct wl_keyboard *kb,
    __unused int32_t rate, __unused int32_t delay)
{
	return;
}

static const struct wl_keyboard_listener kb_listener = {
	.keymap	     = kb_keymap,
	.enter	     = kb_enter,
	.leave	     = kb_leave,
	.key	     = kb_key,
	.modifiers   = kb_modifiers,
	.repeat_info = kb_repeat_info,
};

static struct cow_shm_buffer *
make_pick_buffer(int w, int h)
{
	struct cow_shm_buffer *buf = cow_shm_buffer_create(w, h);
	if (buf == NULL)
		return NULL;
	for (int y = 0; y < h; y++) {
		uint32_t *row = (uint32_t *)((char *)buf->data +
		    (size_t)y * (size_t)buf->stride);
		for (int x = 0; x < w; x++)
			row[x] = 0x22000000;
	}
	return buf;
}

static bool
overlay_init(struct pick_overlay *ov, struct cow_output *out)
{
	if (out->wl_output == NULL || out->width <= 0 || out->height <= 0)
		return false;

	ov->output  = out;
	ov->surface = wl_compositor_create_surface(cow_wm.compositor);
	if (ov->surface == NULL)
		return false;
	ov->layer_surface = zwlr_layer_shell_v1_get_layer_surface(
	    cow_wm.wl_layer_shell, ov->surface, out->wl_output,
	    ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "cow-pick");
	if (ov->layer_surface == NULL) {
		wl_surface_destroy(ov->surface);
		ov->surface = NULL;
		return false;
	}

	ov->buffer = make_pick_buffer(out->width, out->height);
	if (ov->buffer == NULL) {
		zwlr_layer_surface_v1_destroy(ov->layer_surface);
		wl_surface_destroy(ov->surface);
		return false;
	}

	zwlr_layer_surface_v1_set_size(ov->layer_surface, out->width,
	    out->height);
	zwlr_layer_surface_v1_set_anchor(ov->layer_surface,
	    ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
		ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
		ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
		ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
	zwlr_layer_surface_v1_set_exclusive_zone(ov->layer_surface, -1);
	zwlr_layer_surface_v1_set_keyboard_interactivity(ov->layer_surface,
	    ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE);
	zwlr_layer_surface_v1_add_listener(ov->layer_surface, &ls_listener, ov);

	wl_surface_commit(ov->surface);
	return true;
}

static void
overlay_destroy(struct pick_overlay *ov)
{
	if (ov->layer_surface != NULL) {
		zwlr_layer_surface_v1_destroy(ov->layer_surface);
		ov->layer_surface = NULL;
	}
	if (ov->surface != NULL) {
		wl_surface_destroy(ov->surface);
		ov->surface = NULL;
	}
	cow_shm_buffer_destroy(ov->buffer);
	ov->buffer = NULL;
}

static struct pick_overlay *
overlay_for_surface(struct pick_session *s, struct wl_surface *surf)
{
	if (s == NULL)
		return NULL;
	for (int i = 0; i < s->n_overlays; i++) {
		if (s->overlays[i].surface == surf)
			return &s->overlays[i];
	}
	return NULL;
}

bool
cow_pick_pointer_enter(struct wl_surface *surf, uint32_t serial, wl_fixed_t sx,
    wl_fixed_t sy)
{
	struct pick_session *s = active_session;
	if (s == NULL)
		return false;
	struct pick_overlay *ov = overlay_for_surface(s, surf);
	if (ov == NULL)
		return false;

	s->last_serial = serial;
	s->cur_output  = ov->output;
	s->cur_sx      = wl_fixed_to_int(sx);
	s->cur_sy      = wl_fixed_to_int(sy);
	s->have_cursor = true;

	if (s->cursor_shape_dev != NULL)
		wp_cursor_shape_device_v1_set_shape(s->cursor_shape_dev,
		    serial, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_CROSSHAIR);
	return true;
}

bool
cow_pick_pointer_leave(struct wl_surface *surf)
{
	struct pick_session *s = active_session;
	if (s == NULL)
		return false;
	struct pick_overlay *ov = overlay_for_surface(s, surf);
	if (ov == NULL)
		return false;
	if (s->cur_output == ov->output)
		s->have_cursor = false;
	return true;
}

bool
cow_pick_pointer_motion(__unused uint32_t time, wl_fixed_t sx, wl_fixed_t sy)
{
	struct pick_session *s = active_session;
	if (s == NULL || !s->have_cursor)
		return false;
	s->cur_sx = wl_fixed_to_int(sx);
	s->cur_sy = wl_fixed_to_int(sy);
	return true;
}

bool
cow_pick_pointer_button(__unused uint32_t serial, uint32_t button, uint32_t state)
{
	struct pick_session *s = active_session;
	if (s == NULL || !s->have_cursor)
		return false;
	if (state != WL_POINTER_BUTTON_STATE_PRESSED)
		return false;

	if (button == BTN_LEFT) {
		pick_session_fire(s);
		return true;
	}
	if (button == BTN_RIGHT) {
		cow_pick_cancel_active();
		return true;
	}
	return false;
}

static void
pick_session_fire(struct pick_session *s)
{
	struct cow_view *view = NULL;
	if (s->cur_output != NULL) {
		int gx = s->cur_output->x + s->cur_sx;
		int gy = s->cur_output->y + s->cur_sy;
		view   = pick_view_at(s, gx, gy);
	}

	if (s->filter.failed) {
		cmd_filter_fprint_error(&s->filter, stderr, "cow: ");
		pick_session_destroy(s);
		return;
	}

	if (view == NULL || view->closed) {
		pick_session_destroy(s);
		return;
	}

	/*
	 * Tear the overlay down before queueing the command so the cursor
	 * returns to normal immediately.  Run the selected window command
	 * from the next manage pass rather than from this raw pointer
	 * callback, keeping window-management state changes in the same
	 * phase as keybindings and other seat-driven commands.
	 */
	char *cmd_str = s->cmd_str;
	s->cmd_str    = NULL;
	pick_session_destroy(s);
	pick_command_defer(cmd_str, view);
}

static void
pick_commands_init(void)
{
	if (deferred_pick_commands_ready)
		return;
	wl_list_init(&deferred_pick_commands);
	deferred_pick_commands_ready = true;
}

static void
pick_command_defer(char *cmd_str, struct cow_view *view)
{
	if (cmd_str == NULL)
		return;

	if (view == NULL || view->closed) {
		xfree(cmd_str);
		return;
	}

	pick_commands_init();
	struct deferred_pick_command *cmd = xcalloc(1, sizeof(*cmd));
	cmd->cmd_str	     = cmd_str;
	cmd->target	     = view;
	cmd->target_desk     = view->desk;
	cmd->target_output   = view->output;
	cmd->seat	     = cow_seat_first_with_pointer();
	wl_list_insert(deferred_pick_commands.prev, &cmd->link);
	cow_update_request(COW_UPDATE_MANAGE);
}

static void
pick_command_run(struct deferred_pick_command *cmd)
{
	if (cmd == NULL)
		return;
	if (cmd->target == NULL || cmd->target->closed)
		return;

	struct cmd_list *cmdlist = NULL;
	char		*cause	 = NULL;
	if (cmd_string_parse(cmd->cmd_str, &cmdlist, NULL, 0, &cause) != 0) {
		if (cause != NULL) {
			cow_log_error("pick-window: %s", cause);
			xfree(cause);
		}
		return;
	}
	if (cmdlist == NULL)
		return;

	struct cmd_q *cmdq = cmdq_new();
	cmdq->client_fd	   = -1;
	cmdq_set_default_target(cmdq, cmd->target, cmd->target_desk,
	    cmd->target_output);
	struct cow_seat *old_seat = cow_wm.current_interaction_seat;
	cow_wm.current_interaction_seat =
	    (cmd->seat != NULL && !cmd->seat->removed) ? cmd->seat : NULL;
	cmdq_run(cmdq, cmdlist);
	cow_wm.current_interaction_seat = old_seat;
	cmd_list_free(cmdlist);
	cmdq_free(cmdq);
}

void
cow_pick_run_deferred(void)
{
	struct deferred_pick_command *cmd, *tmp;

	if (!deferred_pick_commands_ready)
		return;

	wl_list_for_each_safe(cmd, tmp, &deferred_pick_commands, link)
	{
		wl_list_remove(&cmd->link);
		pick_command_run(cmd);
		xfree(cmd->cmd_str);
		xfree(cmd);
	}
}

void
cow_pick_cancel_active(void)
{
	if (active_session != NULL)
		pick_session_destroy(active_session);
}

static void
pick_session_destroy(struct pick_session *s)
{
	if (s == NULL)
		return;
	if (active_session == s)
		active_session = NULL;

	if (s->keyboard != NULL) {
		wl_keyboard_release(s->keyboard);
		s->keyboard = NULL;
	}
	if (s->kb_state != NULL) {
		xkb_state_unref(s->kb_state);
		s->kb_state = NULL;
	}
	if (s->keymap != NULL) {
		xkb_keymap_unref(s->keymap);
		s->keymap = NULL;
	}
	if (s->cursor_shape_dev != NULL) {
		wp_cursor_shape_device_v1_destroy(s->cursor_shape_dev);
		s->cursor_shape_dev = NULL;
	}
	if (s->overlays != NULL) {
		for (int i = 0; i < s->n_overlays; i++)
			overlay_destroy(&s->overlays[i]);
		xfree(s->overlays);
	}
	cmd_filter_free(&s->filter);
	xfree(s->cmd_str);
	xfree(s);

	if (cow_wm.display != NULL)
		wl_display_flush(cow_wm.display);
}

static enum cmd_retval
cmd_pick_window_exec(struct cmd *self, struct cmd_q *cmdq)
{
	struct args *args = self->args;
	if (args->argc < 1) {
		cmdq_error(cmdq, "pick-window: missing command");
		return (CMD_RETURN_ERROR);
	}
	if (cow_wm.wl_layer_shell == NULL || cow_wm.compositor == NULL ||
	    cow_wm.shm == NULL) {
		cmdq_error(cmdq,
		    "pick-window: layer-shell unavailable on this compositor");
		return (CMD_RETURN_ERROR);
	}
	if (active_session != NULL) {
		cmdq_error(cmdq, "pick-window: already picking");
		return (CMD_RETURN_ERROR);
	}

	struct cmd_filter filter;
	if (!cmd_filter_parse(&filter, cmdq, "pick-window", "-F",
		args_get(args, 'F')))
		return (CMD_RETURN_ERROR);

	int n_outs = 0;
	struct cow_output *out;
	wl_list_for_each(out, &cow_wm.outputs, wm_link) n_outs++;
	if (n_outs == 0) {
		cmd_filter_free(&filter);
		cmdq_error(cmdq, "pick-window: no outputs");
		return (CMD_RETURN_ERROR);
	}

	struct pick_session *s = xcalloc(1, sizeof(*s));
	s->filter = filter;
	s->cmd_str = pick_command_string(args);
	s->overlays = xcalloc((size_t)n_outs, sizeof(*s->overlays));
	s->n_overlays = 0;

	int i = 0;
	wl_list_for_each(out, &cow_wm.outputs, wm_link)
	{
		if (overlay_init(&s->overlays[i], out))
			i++;
	}
	s->n_overlays = i;

	if (s->n_overlays == 0) {
		pick_session_destroy(s);
		cmdq_error(cmdq, "pick-window: no usable output");
		return (CMD_RETURN_ERROR);
	}
	pick_session_seed_pointer(s);

	if (cow_wm.wl_cursor_shape_mgr != NULL &&
	    cow_wm.wl_pointer_raw != NULL) {
		s->cursor_shape_dev = wp_cursor_shape_manager_v1_get_pointer(
		    cow_wm.wl_cursor_shape_mgr, cow_wm.wl_pointer_raw);
	}

	if (cow_wm.wl_seat_raw != NULL && cow_wm.wl_seat_has_keyboard) {
		s->keyboard = wl_seat_get_keyboard(cow_wm.wl_seat_raw);
		if (s->keyboard != NULL)
			wl_keyboard_add_listener(s->keyboard, &kb_listener, s);
	}

	active_session = s;
	wl_display_flush(cow_wm.display);
	return (CMD_RETURN_NORMAL);
}

static void
pick_session_seed_pointer(struct pick_session *s)
{
	int gx, gy;

	if (!cow_seat_get_pointer(&gx, &gy))
		return;

	for (int i = 0; i < s->n_overlays; i++) {
		struct cow_output *out = s->overlays[i].output;
		if (gx < out->x || gx >= out->x + out->width ||
		    gy < out->y || gy >= out->y + out->height)
			continue;

		s->cur_output  = out;
		s->cur_sx      = gx - out->x;
		s->cur_sy      = gy - out->y;
		s->have_cursor = true;
		return;
	}
}

static char *
pick_command_string(struct args *args)
{
	return cmd_argv_to_command_string_or_body(args->argc, args->argv, 0);
}
