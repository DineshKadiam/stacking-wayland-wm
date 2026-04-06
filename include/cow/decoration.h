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
#if !defined(COW_DECORATION_H)
#define COW_DECORATION_H

#include <stdbool.h>
#include <stdint.h>

struct cow_view;
struct cow_shm_buffer;

/*
 * Per-view decoration state.
 *
 * Manages the river decoration surface (below the window), which
 * contains the FVWM/MWM border frame ring.  The frame is rendered
 * into a wl_shm buffer and committed to the decoration wl_surface.
 *
 * The frame has a transparent center so the window content shows
 * through from behind.
 */
struct cow_view_decoration {
	/* Total frame dimensions (including border + titlebar) */
	int frame_width;
	int frame_height;

	/* Border width and titlebar thickness */
	int border_width;
	int title_thickness;

	/* Frame buffer (currently committed or being rendered into) */
	struct cow_shm_buffer *buffer;

	/* Cached generation state */
	bool dirty;
	bool last_active;
	int  last_content_width;
	int  last_content_height;
};

/*
 * Initialize a view's decoration state.
 */
void cow_decoration_init(struct cow_view_decoration *dec);

/*
 * Free all decoration resources.
 */
void cow_decoration_fini(struct cow_view_decoration *dec);

/*
 * Ensure the decoration surface exists, render the frame if dirty,
 * and commit the buffer. Called during a render sequence.
 *
 * `active` selects active or inactive border/titlebar colors.
 */
void cow_decoration_update(struct cow_view *view, bool active);

#endif
