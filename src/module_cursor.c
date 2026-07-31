/*
 * Copyright (c) 2026 Thomas Adam
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR
 * IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */
#include "cow/module_cursor.h"

#include <string.h>

#include <wayland-client.h>

#include "cursor-shape-v1-client-protocol.h"

static void
module_cursor_ensure_device(struct cow_module_cursor *cursor)
{
	if (cursor == NULL || cursor->device != NULL ||
	    cursor->manager == NULL || cursor->pointer == NULL)
		return;

	cursor->device = wp_cursor_shape_manager_v1_get_pointer(
	    cursor->manager, cursor->pointer);
}

bool
cow_module_cursor_handle_global(struct cow_module_cursor *cursor,
    struct wl_registry *registry, uint32_t name, const char *interface,
    uint32_t version)
{
	if (cursor == NULL || cursor->manager != NULL ||
	    strcmp(interface, wp_cursor_shape_manager_v1_interface.name) != 0)
		return (false);

	cursor->manager = wl_registry_bind(registry, name,
	    &wp_cursor_shape_manager_v1_interface, version < 1 ? version : 1);
	module_cursor_ensure_device(cursor);
	return (true);
}

void
cow_module_cursor_set_pointer(struct cow_module_cursor *cursor,
    struct wl_pointer *pointer)
{
	if (cursor == NULL)
		return;

	cursor->pointer = pointer;
	module_cursor_ensure_device(cursor);
}

void
cow_module_cursor_clear_pointer(struct cow_module_cursor *cursor,
    struct wl_pointer *pointer)
{
	if (cursor == NULL || cursor->pointer != pointer)
		return;

	if (cursor->device != NULL) {
		wp_cursor_shape_device_v1_destroy(cursor->device);
		cursor->device = NULL;
	}
	cursor->pointer = NULL;
}

void
cow_module_cursor_fini(struct cow_module_cursor *cursor)
{
	if (cursor == NULL)
		return;

	if (cursor->device != NULL) {
		wp_cursor_shape_device_v1_destroy(cursor->device);
		cursor->device = NULL;
	}
	if (cursor->manager != NULL) {
		wp_cursor_shape_manager_v1_destroy(cursor->manager);
		cursor->manager = NULL;
	}
	cursor->pointer = NULL;
}

bool
cow_module_cursor_available(const struct cow_module_cursor *cursor)
{
	return (cursor != NULL && cursor->manager != NULL);
}

bool
cow_module_cursor_set_shape(struct cow_module_cursor *cursor, uint32_t serial,
    uint32_t shape)
{
	if (cursor == NULL || cursor->pointer == NULL || serial == 0)
		return (false);

	module_cursor_ensure_device(cursor);
	if (cursor->device == NULL)
		return (false);

	wp_cursor_shape_device_v1_set_shape(cursor->device, serial, shape);
	return (true);
}

bool
cow_module_cursor_set_default(struct cow_module_cursor *cursor, uint32_t serial)
{
	return cow_module_cursor_set_shape(cursor, serial,
	    WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT);
}

bool
cow_module_cursor_set_crosshair(struct cow_module_cursor *cursor,
    uint32_t serial)
{
	return cow_module_cursor_set_shape(cursor, serial,
	    WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_CROSSHAIR);
}
