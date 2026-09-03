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
struct cow_font;

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

	/* Border width and visible/configured titlebar thickness */
	int border_width;
	int title_thickness;
	int nominal_title_thickness;

	/* Frame buffer (currently committed or being rendered into) */
	struct cow_shm_buffer *buffer;

	/* Cached generation state */
	bool dirty;
	bool mapped;
	bool last_active;
	int  last_content_width;
	int  last_content_height;
};

/*
 * Initialize a view's decoration state.  Reads initial border width
 * and titlebar thickness from the view's cfg snapshot, so the view's
 * cfg must already be initialised when this is called.
 */
void cow_decoration_init(struct cow_view *);

/*
 * Free all decoration resources.
 */
void cow_decoration_fini(struct cow_view_decoration *);

/*
 * Ensure the decoration surface exists, render the frame if dirty,
 * and commit the buffer. Called during a render sequence.
 *
 * `active` selects active or inactive border/titlebar colors.
 */
void cow_decoration_update(struct cow_view *, bool);

/*
 * Unmap the decoration surface while its window is hidden.
 */
void cow_decoration_hide(struct cow_view *);

/*
 * Return the shared titlebar font (lazily initialised from
 * cow_config->font_name).  Reused across decoration renders so pango
 * doesn't need to rebuild the font description each frame.
 */
struct cow_font *cow_decoration_titlebar_font(void);

/*
 * Build the display title for 'view' ("[<short-id>] <title>").
 * The id prefix grows until it is unique among live views; falls
 * back to view->title if the view has no identifier.  Caller must
 * xfree() the returned string.
 */
char *cow_decoration_display_title(const struct cow_view *);

/*
 * Return the corner-handle length after applying the same minimum used by
 * the decoration renderer.
 */
int cow_decoration_corner_length(const struct cow_view *);

/*
 * Return the resize edges at a point in decoration-surface coordinates.
 * The returned mask uses COW_EDGE_* values and is zero outside the frame's
 * resize regions, including over the titlebar.
 */
uint32_t cow_decoration_edges_at(struct cow_view *, int, int, int, int);

#endif
