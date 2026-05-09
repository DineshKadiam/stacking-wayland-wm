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
#if !defined(COW_OUTPUT_H)
#define COW_OUTPUT_H

#include <stdbool.h>
struct zwlr_layer_surface_v1;
struct zwp_confined_pointer_v1;
#include <wayland-client.h>

#include "cow/cow_box.h"

#define COW_NR_OF_DESKS 10

struct river_output_v1;

/*
 * A virtual desktop: a number and the list of views on it.
 */
struct cow_desk {
	uint8_t		   nr;
	struct wl_list	   views; /* cow_view.desk_views */
	struct cow_output *output;

	/*
	 * Virtual page viewport: the top-left corner of the currently
	 * visible page in virtual desktop coordinates.
	 *   page_vx = page_col * output_width
	 *   page_vy = page_row * output_height
	 * Both are 0 when the page grid is 1×1 (the default).
	 */
	int page_vx;
	int page_vy;

	/* Smooth scroll animation state (only used when page_scroll_animate) */
	int  scroll_from_vx, scroll_from_vy; /* start viewport */
	int  scroll_to_vx, scroll_to_vy;     /* target viewport */
	long scroll_start_ms;		     /* animation start time (ms) */
	bool scrolling;			     /* animation in progress */
};

/*
 * A logical output tracked by the window manager.
 * Wraps a river_output_v1 object.
 */
struct cow_output {
	struct river_output_v1 *river_output;
	struct wl_list		wm_link; /* cow_wm.outputs */

	/* Output geometry in compositor logical coordinates */
	int x, y;
	int width, height;

	/* The usable area (after subtracting layer shell exclusive zones).
	 * Initially equals the full output dimensions. Updated by
	 * river-layer-shell-v1 non_exclusive_area events in Phase 10. */
	struct cow_box usable_area;

	/* Corresponding wl_output global name (for wl_registry binding) */
	uint32_t wl_output_name;

	/* Bound wl_output object (for name/description events) */
	struct wl_output *wl_output;

	/* Human-readable output name (e.g. "DP-1", "HDMI-A-1") */
	char *name;

	/* Per-output background colour (overrides global if set) */
	float bg_colour[4];
	bool  has_bg_colour;

	/* Desks: 10 virtual desktops per output */
	struct cow_desk	 desks[COW_NR_OF_DESKS];
	struct cow_desk *current_desk;

	/* Bitmask of desks whose windows are temporarily shown
	 * on the current desk (collect overlay).  Bit N set means
	 * desk N is collected. */
	uint32_t collected_desks;

	/* Views currently visible on this output */
	struct wl_list visible_views; /* cow_view.output_views */

	bool removed;

	/* Background shell surface (filled with background colour) */
	struct wl_surface	      *bg_surface;
	struct river_shell_surface_v1 *bg_shell_surface;
	struct river_node_v1	      *bg_node;
	struct cow_shm_buffer	      *bg_buffer;
	bool			       bg_dirty;

	/* Per-output layer-shell state (opaque to consumers). */
	struct cow_layer_shell_output *layer_shell_state;

	/*
	 * Edge scroll dwell state.
	 *
	 * Primary path: zwlr_layer_shell pan frame surfaces (pan_frames.c).
	 * Each edge has a thin input-capable layer surface; wl_pointer
	 * enter/leave events arm/cancel the dwell timer and paint the frame.
	 *
	 * Fallback path (no wl_layer_shell): proximity detection via the
	 * pointer-position events River already delivers.
	 *
	 * pan_edge:         armed edge (-1 = none; 0=top 1=bottom 2=left
	 * 3=right) pan_enter_ms:     monotonic ms when dwell started.
	 * pan_last_fire_ms: monotonic ms of the last successful pan_tick
	 *                   fire on this output.  Used to (a) shorten
	 *                   the dwell on rapid consecutive paging and
	 *                   (b) suppress view-edge-based arming for a
	 *                   short cooldown so paging in one direction
	 *                   doesn't immediately re-arm the opposite edge.
	 */
	int  pan_edge;
	long pan_enter_ms;
	long pan_last_fire_ms;

	/*
	 * Pan frame input surfaces (zwlr_layer_surface_v1).
	 * Index: 0=top 1=bottom 2=left 3=right.
	 * Each is a thin input-capable layer surface at the edge.
	 * When the pointer enters one it turns orange and the dwell
	 * timer arms; on leave it becomes transparent again.
	 */
	struct wl_surface	     *pan_surf[4];
	struct zwlr_layer_surface_v1 *pan_lsurf[4];
	struct cow_shm_buffer	     *pan_buf_clear[4];
	struct cow_shm_buffer	     *pan_buf_orange[4];
	void *pan_ctx[4]; /* pan_frame_ctx*, freed on destroy */
	struct zwp_confined_pointer_v1
	    *pan_confined[4]; /* active confinement */
	bool pan_entered[4];  /* pointer is over this frame */
};

/*
 * Create a new output wrapper for the given river_output_v1.
 * Initializes desks and adds to cow_wm.outputs.
 */
struct cow_output *cow_output_create(struct river_output_v1 *);

/*
 * Destroy an output wrapper (called on river_output_v1.removed).
 */
void cow_output_destroy(struct cow_output *);

/*
 * Recompute the usable area by applying the configured reserved area
 * on top of the current usable area (from output dimensions or
 * layer shell non_exclusive_area).
 */
void cow_output_apply_reserved_area(struct cow_output *);

/*
 * Create the background shell surface if needed.
 * Must be called during a manage sequence.
 */
void cow_output_prepare_background(struct cow_output *);

/*
 * Render and commit the background surface.
 * Must be called during a render sequence.
 */
void cow_output_render_background(struct cow_output *);

/*
 * Find the output that contains the given global coordinate.
 */
struct cow_output *cow_output_at(int, int);

/*
 * Find an output by its river_output_v1 pointer.
 */
struct cow_output *cow_output_find(struct river_output_v1 *);

/*
 * Find an output by its human-readable name (e.g. "DP-1").
 * Returns NULL if not found.
 */
struct cow_output *cow_output_find_by_name(const char *);

/*
 * Return the largest (output->y + output->height) over all outputs,
 * i.e. the bottom edge of the union bounding box of every output.
 * Used by view-parking math that needs an off-screen y coordinate
 * guaranteed not to land inside any physical monitor.
 */
int cow_outputs_max_bottom(void);

void cow_output_update_current(void);

/*
 * Pan frame management API lives in cow/pan_frames.h.
 */

#endif
