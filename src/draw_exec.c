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
#include "cow/draw_exec.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <cairo/cairo.h>
#include <wayland-client.h>

#include "cow/cmd.h"
#include "cow/cmd-filter.h"
#include "cow/cmd_scan.h"
#include "cow/compat.h"
#include "cow/cow_box.h"
#include "cow/memory.h"
#include "cow/output.h"
#include "cow/seat.h"
#include "cow/shm.h"
#include "cow/titlebar.h"
#include "cow/view.h"
#include "cow/wm.h"

#include "river-window-management-v1-client-protocol.h"

#define DRAW_EXEC_MIN_SIZE 8
#define DRAW_EXEC_TIMEOUT_SECS 15

struct draw_exec_state {
	bool active;
	bool pending;
	bool dirty;

	int start_x, start_y;
	int cur_x, cur_y;
	struct cow_box box;
	time_t pending_expires;

	char *cmd_str;
	struct cmd_filter filter;
	struct cow_seat *seat;
	struct cow_output *output;

	struct wl_surface *surface;
	struct river_shell_surface_v1 *shell;
	struct river_node_v1 *node;
	struct cow_shm_buffer *buffer;
	int buffer_w, buffer_h;
};

static struct draw_exec_state draw_exec;

static bool draw_exec_create(void);
static void draw_exec_destroy_overlay(void);
static void draw_exec_draw(void);
static void draw_exec_set_box_from_points(void);
static void draw_exec_run_command(char *, struct cow_seat *,
    struct cow_output *);
static char *draw_exec_command_string(const char *);
static void draw_exec_clear_pending(void);

bool
cow_draw_exec_begin(struct cow_seat *seat, const char *cmd_str,
    struct cmd_filter *filter)
{
	if (seat == NULL || cmd_str == NULL || *cmd_str == '\0')
		return false;
	if (!seat->has_pointer_position)
		return false;

	cow_draw_exec_cancel();

	draw_exec.active = true;
	draw_exec.pending = false;
	draw_exec.dirty = true;
	draw_exec.start_x = seat->pointer_x;
	draw_exec.start_y = seat->pointer_y;
	draw_exec.cur_x = seat->pointer_x;
	draw_exec.cur_y = seat->pointer_y;
	draw_exec.cmd_str = xstrdup(cmd_str);
	draw_exec.filter = *filter;
	memset(filter, 0, sizeof(*filter));
	draw_exec.seat = seat;
	draw_exec.output = cow_output_at(seat->pointer_x, seat->pointer_y);
	draw_exec_set_box_from_points();

	return true;
}

void
cow_draw_exec_update(struct cow_seat *seat)
{
	if (!draw_exec.active || seat == NULL)
		return;

	if (seat->has_pointer_position) {
		draw_exec.cur_x = seat->pointer_x;
		draw_exec.cur_y = seat->pointer_y;
	} else {
		draw_exec.cur_x = draw_exec.start_x + seat->op_dx;
		draw_exec.cur_y = draw_exec.start_y + seat->op_dy;
	}
	draw_exec_set_box_from_points();
	draw_exec.dirty = true;
}

void
cow_draw_exec_finish(struct cow_seat *seat)
{
	char *cmd_str;
	struct cow_output *output;

	if (!draw_exec.active)
		return;

	cow_draw_exec_update(seat);
	draw_exec_destroy_overlay();
	draw_exec.active = false;

	if (draw_exec.box.width < DRAW_EXEC_MIN_SIZE ||
	    draw_exec.box.height < DRAW_EXEC_MIN_SIZE) {
		cow_draw_exec_cancel();
		return;
	}

	draw_exec.pending = true;
	draw_exec.pending_expires = time(NULL) + DRAW_EXEC_TIMEOUT_SECS;
	output = cow_output_at(draw_exec.box.x + draw_exec.box.width / 2,
	    draw_exec.box.y + draw_exec.box.height / 2);
	if (output != NULL)
		draw_exec.output = output;

	cmd_str = draw_exec.cmd_str;
	draw_exec.cmd_str = NULL;
	draw_exec_run_command(cmd_str, draw_exec.seat, draw_exec.output);
	xfree(cmd_str);
}

void
cow_draw_exec_cancel(void)
{
	draw_exec_destroy_overlay();
	xfree(draw_exec.cmd_str);
	draw_exec.cmd_str = NULL;
	cmd_filter_free(&draw_exec.filter);
	memset(&draw_exec, 0, sizeof(draw_exec));
}

bool
cow_draw_exec_apply_pending(struct cow_view *view)
{
	bool matches = false;
	int content_w, content_h;
	int frame_w, frame_h, content_x, content_y;
	struct cow_output *output;

	if (!draw_exec.pending || view == NULL || view->closed)
		return false;

	if (time(NULL) > draw_exec.pending_expires) {
		draw_exec_clear_pending();
		return false;
	}

	if (!cmd_filter_view(&draw_exec.filter, view, &matches)) {
		cmd_filter_fprint_error(&draw_exec.filter, stderr, "cow: ");
		draw_exec_clear_pending();
		return false;
	}
	if (!matches)
		return false;

	output = cow_output_at(draw_exec.box.x + draw_exec.box.width / 2,
	    draw_exec.box.y + draw_exec.box.height / 2);
	if (output != NULL && output != view->output) {
		cow_view_reassign_output(view, output);
		cow_view_clear_orphan(view);
	}

	content_w = draw_exec.box.width;
	content_h = draw_exec.box.height;
	cow_titlebar_frame_geometry(view, content_w, content_h, &frame_w,
	    &frame_h, &content_x, &content_y);
	content_w = draw_exec.box.width - (frame_w - content_w);
	content_h = draw_exec.box.height - (frame_h - content_h);
	cow_clamp_dimensions(&content_w, &content_h);
	cow_titlebar_frame_geometry(view, content_w, content_h, &frame_w,
	    &frame_h, &content_x, &content_y);

	view->x = draw_exec.box.x + content_x;
	view->y = draw_exec.box.y + content_y;
	cow_view_set_pending_geometry(view, view->x, view->y, content_w,
	    content_h);
	view->placed = true;
	view->dec.dirty = true;

	draw_exec_clear_pending();
	return true;
}

void
cow_draw_exec_render(void)
{
	if (!draw_exec.active)
		return;
	if (draw_exec.box.width < 1 || draw_exec.box.height < 1)
		return;
	if (!draw_exec_create())
		return;

	river_node_v1_set_position(draw_exec.node, draw_exec.box.x,
	    draw_exec.box.y);
	river_node_v1_place_top(draw_exec.node);

	if (draw_exec.buffer == NULL ||
	    draw_exec.buffer_w != draw_exec.box.width ||
	    draw_exec.buffer_h != draw_exec.box.height) {
		if (draw_exec.buffer != NULL && draw_exec.buffer->busy)
			return;
		cow_shm_buffer_destroy(draw_exec.buffer);
		draw_exec.buffer = cow_shm_buffer_create(draw_exec.box.width,
		    draw_exec.box.height);
		if (draw_exec.buffer == NULL)
			return;
		draw_exec.buffer_w = draw_exec.box.width;
		draw_exec.buffer_h = draw_exec.box.height;
		draw_exec.dirty = true;
	}

	if (!draw_exec.dirty || draw_exec.buffer->busy)
		return;

	draw_exec_draw();
	draw_exec.dirty = false;

	wl_surface_attach(draw_exec.surface, draw_exec.buffer->buffer, 0, 0);
	wl_surface_damage_buffer(draw_exec.surface, 0, 0,
	    draw_exec.buffer_w, draw_exec.buffer_h);
	river_shell_surface_v1_sync_next_commit(draw_exec.shell);
	wl_surface_commit(draw_exec.surface);
}

static bool
draw_exec_create(void)
{
	struct wl_surface *surface;
	struct river_shell_surface_v1 *shell;

	if (draw_exec.surface != NULL)
		return true;
	if (cow_wm.compositor == NULL || cow_wm.manager == NULL)
		return false;

	surface = wl_compositor_create_surface(cow_wm.compositor);
	if (surface == NULL)
		return false;

	shell = river_window_manager_v1_get_shell_surface(cow_wm.manager,
	    surface);
	if (shell == NULL) {
		wl_surface_destroy(surface);
		return false;
	}

	draw_exec.surface = surface;
	draw_exec.shell = shell;
	draw_exec.node = river_shell_surface_v1_get_node(shell);
	draw_exec.dirty = true;
	return true;
}

static void
draw_exec_destroy_overlay(void)
{
	if (draw_exec.node != NULL) {
		river_node_v1_destroy(draw_exec.node);
		draw_exec.node = NULL;
	}
	if (draw_exec.shell != NULL) {
		river_shell_surface_v1_destroy(draw_exec.shell);
		draw_exec.shell = NULL;
	}
	if (draw_exec.surface != NULL) {
		wl_surface_destroy(draw_exec.surface);
		draw_exec.surface = NULL;
	}
	cow_shm_buffer_destroy(draw_exec.buffer);
	draw_exec.buffer = NULL;
	draw_exec.buffer_w = 0;
	draw_exec.buffer_h = 0;
	draw_exec.dirty = false;
}

static void
draw_exec_draw(void)
{
	cairo_surface_t *cs;
	cairo_t *cr;
	double w = draw_exec.buffer_w;
	double h = draw_exec.buffer_h;

	cs = cairo_image_surface_create_for_data(
	    (unsigned char *)draw_exec.buffer->data, CAIRO_FORMAT_ARGB32,
	    draw_exec.buffer_w, draw_exec.buffer_h, draw_exec.buffer->stride);
	cr = cairo_create(cs);

	cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
	cairo_paint(cr);
	cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

	cairo_set_source_rgba(cr, 0.12, 0.33, 0.78, 0.28);
	cairo_rectangle(cr, 0, 0, w, h);
	cairo_fill(cr);

	cairo_set_line_width(cr, 1.0);
	cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.90);
	cairo_rectangle(cr, 0.5, 0.5, w - 1.0, h - 1.0);
	cairo_stroke(cr);
	if (w > 3.0 && h > 3.0) {
		cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.80);
		cairo_rectangle(cr, 1.5, 1.5, w - 3.0, h - 3.0);
		cairo_stroke(cr);
	}

	cairo_surface_flush(cs);
	cairo_destroy(cr);
	cairo_surface_destroy(cs);
}

static void
draw_exec_set_box_from_points(void)
{
	int x1 = draw_exec.start_x;
	int y1 = draw_exec.start_y;
	int x2 = draw_exec.cur_x;
	int y2 = draw_exec.cur_y;

	if (x2 < x1) {
		int t = x1;
		x1 = x2;
		x2 = t;
	}
	if (y2 < y1) {
		int t = y1;
		y1 = y2;
		y2 = t;
	}
	draw_exec.box.x = x1;
	draw_exec.box.y = y1;
	draw_exec.box.width = x2 - x1 + 1;
	draw_exec.box.height = y2 - y1 + 1;
}

static void
draw_exec_run_command(char *cmd_str, struct cow_seat *seat,
    struct cow_output *output)
{
	struct cmd_list *cmdlist = NULL;
	struct cmd_q *cmdq;
	char *cause = NULL;

	if (cmd_str == NULL || *cmd_str == '\0')
		return;

	if (cmd_string_parse(cmd_str, &cmdlist, NULL, 0, &cause) != 0) {
		if (cause != NULL) {
			fprintf(stderr, "cow: draw-exec: %s\n", cause);
			xfree(cause);
		}
		return;
	}
	if (cmdlist == NULL)
		return;

	cmdq = cmdq_new();
	cmdq->client_fd = -1;
	if (output != NULL)
		cmdq_set_default_target(cmdq, NULL, output->current_desk,
		    output);

	struct cow_seat *old_seat = cow_wm.current_interaction_seat;
	cow_wm.current_interaction_seat =
	    (seat != NULL && !seat->removed) ? seat : NULL;
	cmdq_run(cmdq, cmdlist);
	cow_wm.current_interaction_seat = old_seat;

	cmd_list_free(cmdlist);
	cmdq_free(cmdq);
}

static char *
draw_exec_command_string(const char *cmd)
{
	char *copy = xstrdup(cmd);
	size_t end;

	if (copy[0] != '{')
		return copy;
	if (cow_cmd_find_matching_brace(copy, strlen(copy), &end) !=
	    COW_CMD_SCAN_COMPLETE)
		return copy;
	for (size_t i = end + 1; copy[i] != '\0'; i++) {
		if (!isspace((unsigned char)copy[i]))
			return copy;
	}

	char *body = xstrdup(copy + 1);
	body[end - 1] = '\0';
	xfree(copy);
	return body;
}

static void
draw_exec_clear_pending(void)
{
	draw_exec.pending = false;
	draw_exec.pending_expires = 0;
	cmd_filter_free(&draw_exec.filter);
}

char *
cow_draw_exec_make_command_string(int argc, char **argv)
{
	char *cmd;

	if (argc == 1 && argv[0][0] == '{')
		cmd = draw_exec_command_string(argv[0]);
	else
		cmd = cmd_argv_to_command_string(argc, argv, 0);

	return cmd;
}
