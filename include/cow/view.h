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
#if !defined(COW_VIEW_NEW_H)
#define COW_VIEW_NEW_H

#include <stdbool.h>
#include <wayland-client.h>

#include "cow/config.h"
#include "cow/cow_box.h"
#include "cow/decoration.h"

struct cow_container;
struct cow_container_member;
struct river_window_v1;
struct river_node_v1;
struct river_decoration_v1;
struct cow_output;
struct cow_desk;

/*
 * Container: a meta-window that groups multiple views as tabs.
 * Only the active member's content is visible; all others are hidden.
 * The active member owns the shared decoration frame which renders
 * the full tab strip in the titlebar.
 */
extern int next_container_id;

struct cow_container_member {
	struct wl_list	      link; /* cow_container.members */
	struct cow_view	     *view;
	struct cow_container *container;
	/* Geometry saved when the view joined, restored on detach */
	int pre_x, pre_y;
	int pre_width, pre_height;
};

struct cow_container {
	struct wl_list		     wm_link; /* cow_wm.containers */
	struct wl_list		     members; /* cow_container_member.link */
	int			     id;
	int			     n_members;
	struct cow_container_member *active;

	/* Geometry (authoritative for all members) */
	int x, y;
	int content_width, content_height;

	struct cow_desk	  *desk;
	struct cow_output *output;
	uint32_t	   flags; /* sticky etc., shared across members */
};

/*
 * View flags (carried over from original cow).
 */
/*
 * COW_VIEW_STICKY_DESK_FLAG: window appears on all desks of its output.
 * COW_VIEW_STICKY_PAGE_FLAG: window appears on all pages of its current desk.
 * Both set: fully sticky (visible regardless of desk or page).
 *
 * The legacy COW_VIEW_STICKY_FLAG alias maps to desk-sticky so that
 * existing code using it is not broken.
 */
#define COW_VIEW_STICKY_DESK_FLAG (1 << 0)
#define COW_VIEW_STICKY_PAGE_FLAG (1 << 2)
#define COW_VIEW_STICKY_FLAG COW_VIEW_STICKY_DESK_FLAG /* legacy alias */
#define COW_VIEW_ICONIFIED_FLAG (1 << 1)

enum shade_direction {
	COW_SHADE_NONE,
	COW_SHADE_NORTH,
	COW_SHADE_SOUTH,
	COW_SHADE_EAST,
	COW_SHADE_WEST,
	COW_SHADE_NW,
	COW_SHADE_NE,
	COW_SHADE_SW,
	COW_SHADE_SE,
	COW_SHADE_ERROR,
};

/*
 * Resolved visual style for a view.  Starts from global config
 * defaults and is overridden by matching rules.
 */
struct cow_view_style {
	int			 border;
	struct cow_border_colour border_active;
	struct cow_border_colour border_inactive;
	bool  has_border_override; /* per-edge override set */
	float titlebar_active[4];
	float titlebar_inactive[4];
	float titlebar_fg_active[4];
	float titlebar_fg_inactive[4];
	bool  has_titlebar_override;
	bool  has_titlebar_fg_override;
	bool  titlebar_enabled;
	bool  resolved;
};

/*
 * A managed window tracked by the window manager.
 * Wraps a river_window_v1 object.
 */
struct cow_view {
	struct river_window_v1 *river_window;
	struct river_node_v1   *node;

	/* Decoration surface (below the window) for FVWM borders + titlebar */
	struct wl_surface	   *decoration_surface;
	struct river_decoration_v1 *decoration;

	/* Per-view decoration rendering state */
	struct cow_view_decoration dec;

	struct wl_list wm_link;	     /* cow_wm.views */
	struct wl_list desk_views;   /* cow_desk.views */
	struct wl_list output_views; /* cow_output.visible_views */

	/* Window properties (from river events) */
	char *
	    identifier; /* unique window ID, persists across WM reconnections */
	char		*app_id;
	char		*title;
	struct cow_view *parent;

	/* Dimensions as reported by the window (from dimensions event) */
	int  content_width;
	int  content_height;
	bool has_dimensions; /* true after first dimensions event */

	/*
	 * Virtual position: the window's location in the virtual page grid
	 * (desk-space coordinates).  On a 3×2 page grid with a 1920-wide
	 * output, vx=1920 puts the window on page column 1.
	 *
	 * With a 1×1 grid (the default), vx==x and vy==y.
	 */
	int vx, vy;

	/*
	 * Rendered global compositor position: derived from vx/vy minus the
	 * current page viewport (desk->page_vx/vy) plus the output origin.
	 * Recomputed each render cycle; do not store persistent state here.
	 */
	int x, y;

	/* The geometry we want the window to have (proposed) */
	int proposed_width;
	int proposed_height;

	/* Desk/output assignment */
	struct cow_desk	  *desk;
	struct cow_output *output;

	/* Flags */
	uint32_t flags;

	/* Decoration hint from the window */
	uint32_t decoration_hint;

	/* State tracking */
	bool closed;
	bool configured; /* true after first propose_dimensions */
	bool use_ssd;	 /* true if we want server-side decorations */
	bool fullscreen;
	bool maximized;
	bool needs_raise; /* set during manage, applied in render */

	/* Window shade state */
	enum shade_direction sd;
	int shade_saved_width; /* content dimensions before shading */
	int shade_saved_height;

	/* Resolved visual style (from global config + matching rules) */
	struct cow_view_style style;

	/* Decoration press state (for sunken title/button visual) */
	bool title_pressed;
	int  pressed_button; /* -1 = none, 0/1/2 = momentary frame sunken */
	int  icon_inverted_button; /* -1 = none, 0/1/2 = icon relief swapped */

	/* New window needs focus on next manage cycle */
	bool needs_focus;

	/* Saved geometry for maximize/restore */
	int saved_x, saved_y;
	int saved_width, saved_height;

	/* Saved geometry when collected (restored on uncollect) */
	bool collected_geo_saved;
	int  collected_saved_x;
	int  collected_saved_y;
	int  collected_saved_width;
	int  collected_saved_height;

	/* Deferred close (applied during manage cycle) */
	bool pending_close;

	/* Deferred lower (applied during render cycle) */
	bool pending_lower;

	/* Min/max dimension hints from the window */
	int min_width, min_height;
	int max_width, max_height;

	/* Pending geometry change (applied during manage cycle) */
	bool pending_resize;
	int  pending_x, pending_y;
	int  pending_width, pending_height;

	/* Pending clip box change (applied during manage cycle) */
	bool pending_clip;
	int  clip_x, clip_y, clip_w, clip_h;

	/* Icon surface for iconified (minimized) windows */
	struct wl_surface	      *icon_surface;
	struct river_shell_surface_v1 *icon_shell;
	struct river_node_v1	      *icon_node;
	struct cow_shm_buffer	      *icon_buffer;
	int			       icon_x, icon_y;
	int			       icon_width, icon_height;

	/*
	 * Container membership.  NULL when the view is not grouped.
	 * When non-NULL the container owns geometry and the decoration
	 * is managed at the container level (only the active member
	 * has use_ssd = true).
	 */
	struct cow_container_member *container_member;
};

/*
 * Create a new view wrapper for the given river_window_v1.
 * Adds listeners and inserts into cow_wm.views.
 */
struct cow_view *cow_view_create(struct river_window_v1 *river_window);

/*
 * Destroy a view wrapper.
 */
void cow_view_destroy(struct cow_view *view);

/*
 * Configure a new view: propose dimensions, set SSD, assign to desk.
 * Called during a manage sequence for newly created views.
 */
void cow_view_configure(struct cow_view *view);

/*
 * Apply rendering state for a view during a render sequence:
 * get node, set position, show/hide, set up decorations.
 */
void cow_view_apply_render(struct cow_view *view);

/*
 * Find a view by its river_window_v1 pointer.
 */
struct cow_view *cow_view_find(struct river_window_v1 *river_window);

/*
 * Reassign a view to a different output.  Moves the view to the
 * target output's current desk.  Does nothing if already on that output.
 */
void cow_view_reassign_output(struct cow_view *view, struct cow_output *output);

/*
 * Update a view's output assignment based on its current position.
 * Called after move/drag to ensure the view belongs to the correct output.
 */
void cow_view_update_output(struct cow_view *view);

/*
 * Is this view visible on the given output's current desk?
 * A view is visible if it's on the current desk (or sticky)
 * and not iconified.
 */
bool cow_view_visible(const struct cow_view *view,
    const struct cow_output		    *output);

/*
 * Is this view a valid focus/interaction target?
 */
bool cow_view_focusable(const struct cow_view *v);

/*
 * Save current geometry for later restore (maximize/expand undo).
 */
void cow_view_save_geometry(struct cow_view *view);

/*
 * Set pending geometry change (deferred to manage callback).
 * Also marks decoration dirty and requests a manage cycle.
 */
void cow_view_set_pending_geometry(struct cow_view *view, int x, int y, int w,
    int h);

/*
 * Compute the frame bounding box (border + titlebar + content).
 */
struct cow_box cow_view_frame_box(const struct cow_view *v);

/*
 * Walk the view list circularly from start, returning the first
 * view matching pred.  Returns NULL if none found.
 */
struct cow_view *cow_view_walk(struct cow_view *start, bool forward,
    bool (*pred)(const struct cow_view *));

/*
 * Maximize/unmaximize a view.
 */
int  cow_find_maximize_button(void);
void cow_view_maximize(struct cow_view *view);
void cow_view_unmaximize(struct cow_view *view);

/*
 * Shade/unshade a view.
 */
void cow_view_shade(struct cow_view *view, int direction);
void cow_view_unshade(struct cow_view *view);

/*
 * Fullscreen/unfullscreen a view.
 */
void cow_view_fullscreen(struct cow_view *view);
void cow_view_exit_fullscreen(struct cow_view *view);

/*
 * Iconify (minimize) a view -- hide the window and show a small
 * icon on the desktop.  Clicking the icon deiconifies.
 */
void cow_view_iconify(struct cow_view *view);
void cow_view_deiconify(struct cow_view *view);

/*
 * Render the icon surface for an iconified view.
 * Called during the render sequence.
 */
void cow_view_icon_render(struct cow_view *view);

/*
 * Recompute icon positions for all iconified views on an output.
 */
void cow_output_layout_icons(struct cow_output *output);

bool cow_view_is_iconified(const struct cow_view *view);

/*
 * Resolve the view's visual style from global config + matching rules.
 * Called when the view is created and when rules/config change.
 */
void cow_view_resolve_style(struct cow_view *view);

/* -----------------------------------------------------------------------
 * Container API
 * ----------------------------------------------------------------------- */

/*
 * Create a new container from two uncontained views.
 * first_view becomes the active (visible) member.
 */
struct cow_container *cow_container_create(struct cow_view *first_view,
    struct cow_view					   *second_view);

/*
 * Add a view to an existing container.  The view must not already be
 * in a container.  Returns false if the view is already a member.
 */
bool cow_container_add(struct cow_container *container, struct cow_view *view);

/*
 * Remove a view from its container.  If the container drops to one
 * member, that member is also released and the container is destroyed.
 * The detached view is given a cascade-offset position relative to the
 * container.
 */
void cow_container_remove_view(struct cow_view *view);

/*
 * Make member the active (visible) tab.  Destroys the old active
 * member's decoration and marks the new one dirty.
 */
void cow_container_activate(struct cow_container *container,
    struct cow_container_member			 *member);

/*
 * Activate the next or previous tab in the container.
 */
void cow_container_activate_next(struct cow_container *container);
void cow_container_activate_prev(struct cow_container *container);

/*
 * Move the active tab one position to the right (next) or left (prev)
 * within the container's tab strip.  The tab remains active after the
 * swap.  No-ops if the tab is already at the respective end.
 */
void cow_container_swap_next(struct cow_container *container);
void cow_container_swap_prev(struct cow_container *container);

/*
 * Return the container that contains view, or NULL.
 */
struct cow_container *cow_view_container(const struct cow_view *view);

/*
 * Return true if view is the active tab in its container.
 */
bool cow_view_is_active_tab(const struct cow_view *view);

/*
 * Destroy a container and release all its members (detach, no cascade).
 * Called during WM shutdown.
 */
void cow_container_destroy(struct cow_container *container);

/*
 * Return true if view is the drop indicator target for a container drag.
 * The view's decoration will draw a highlight when this is set.
 */
bool cow_view_is_drop_target(const struct cow_view *view);

/*
 * If view is in a container, propagate view->x/view->y to the container
 * and to every other member's x/y so that all members stay in sync.
 * Must be called after any operation that changes a container member's
 * position (move, snap, centre, etc.).
 */
void cow_view_sync_container_pos(struct cow_view *view);

/*
 * Sync the container's full geometry (position AND dimensions) from the
 * given explicit values.  Used by resize operations where the new w/h
 * come from proposed values before the dimensions event has arrived,
 * so view->content_width/height are not yet updated.
 * Also queues pending_resize for all non-active members.
 */
void cow_view_sync_container_geometry(struct cow_view *view, int x, int y,
    int w, int h);

void view_warp_to_focused(void);
void view_centre(const struct cow_view *, int *, int *);

#endif
