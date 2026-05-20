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
#if !defined(COW_VIEW_H)
#define COW_VIEW_H

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
 * Per-view visual configuration.  A snapshot of the visual subset of
 * cow_config taken at view-create time, with subsequent updates from
 * either (a) global `set` commands (which mutate cow_config and stamp
 * every view's snapshot) or (b) view-targeted `set` commands -- run
 * either via -t with a window selector, or via the rule subsystem
 * fanning out an event to a view (default_target.window resolved).
 *
 * Only fields that affect a single view's rendered decoration live
 * here.  Behavioural fields (placement, focus_mode, desktop_config,
 * page_*, edge_scroll, reserved.*, gap, step, snap_distance,
 * keybindings) stay global on cow_config because they don't make
 * sense to vary per view.  output.colour.background is per-output,
 * not per-view.
 *
 * font_name is duplicated here for completeness so the snapshot is
 * self-contained, but the decoration code currently uses a single
 * global titlebar_font cache keyed off cow_config->font_name; a
 * per-view font cache is left for a later rev.
 */
struct cow_view_cfg {
	int		      border;
	int		      corner_length;
	bool		      handles;
	enum cow_border_style border_style;

	struct cow_border_colour border_active;
	struct cow_border_colour border_inactive;

	struct cow_titlebar_config  titlebar;
	struct cow_container_config container;

	char *font_name;

	int icon_size;
};

/*
 * Initialise a snapshot from the current cow_config.  Heap-allocated
 * fields (font_name, titlebar.font_name) are dup'd so the snapshot
 * owns its own copies.
 */
void cow_view_cfg_init_from_global(struct cow_view_cfg *);

/*
 * Re-stamp a snapshot from the current cow_config.  Used when a
 * global `set` updates cow_config; every existing view's snapshot is
 * brought back into sync for the visual fields.  Frees and re-dups
 * any heap-allocated strings.
 */
void cow_view_cfg_apply_global(struct cow_view_cfg *);

/*
 * Free heap-allocated members of a snapshot.  Does not free the
 * struct itself.
 */
void cow_view_cfg_fini(struct cow_view_cfg *);

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
	bool placed;

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
	/*
	 * Per-axis maximised state.  maximized is the OR of the two
	 * and is the field every non-maximise call site already reads
	 * (drag-to-unmax, state save / restore, status output, pan
	 * frames, icon-button highlight).  cmd-window keeps the two
	 * narrow flags in sync with the unified one so external
	 * readers don't have to care which axes are involved.
	 *
	 * Horizontal-only or vertical-only maximise is initiated by
	 * `window-maximize -h` / `-v`; either path saves only the
	 * affected axis's geometry into saved_x/saved_width or
	 * saved_y/saved_height, and -u / -T -h / -T -v restores just
	 * that axis.  When both axes are maximised the saved geometry
	 * is the full pre-maximise box.
	 */
	bool maximized;
	bool maximized_h;
	bool maximized_v;
	bool needs_raise; /* set during manage, applied in render */

	/*
	 * Monotonic z-order counter: incremented from cow_wm.raise_seq
	 * each time the view is raised to top.  Higher = on top.
	 * Exposed via 'show windows' for clients that need to pick the
	 * topmost window at a point (e.g. cowident).
	 */
	uint64_t raise_seq;

	/* Window shade state */
	enum shade_direction sd;
	int shade_saved_width; /* content dimensions before shading */
	int shade_saved_height;

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

	/*
	 * Per-view visual configuration snapshot.  Initialised from
	 * cow_config at view-create time and mutated thereafter by
	 * `set` commands targeting this view (or all views, in the
	 * case of a global set).  All decoration / titlebar / icon
	 * rendering paths read visual fields from here, never from
	 * cow_config directly.
	 */
	struct cow_view_cfg cfg;
};

/*
 * Create a new view wrapper for the given river_window_v1.
 * Adds listeners and inserts into cow_wm.views.
 */
struct cow_view *cow_view_create(struct river_window_v1 *);

/*
 * Destroy a view wrapper.
 */
void cow_view_destroy(struct cow_view *);

/*
 * Configure a new view: propose dimensions, set SSD, assign to desk.
 * Called during a manage sequence for newly created views.
 */
void cow_view_configure(struct cow_view *);

/*
 * Apply rendering state for a view during a render sequence:
 * get node, set position, show/hide, set up decorations.
 */
void cow_view_apply_render(struct cow_view *);

/*
 * Find a view by its river_window_v1 pointer.
 */
struct cow_view *cow_view_find(struct river_window_v1 *);

/*
 * Reassign a view to a different output.  Moves the view to the
 * target output's current desk.  Does nothing if already on that output.
 */
void cow_view_reassign_output(struct cow_view *, struct cow_output *);

/*
 * Update a view's output assignment based on its current position.
 * Called after move/drag to ensure the view belongs to the correct output.
 */
void cow_view_update_output(struct cow_view *);

/*
 * Is this view visible on the given output's current desk?
 * A view is visible if it's on the current desk (or sticky)
 * and not iconified.
 */
bool cow_view_visible(const struct cow_view *, const struct cow_output *);

/*
 * Is this view a valid focus/interaction target?
 */
bool cow_view_focusable(const struct cow_view *);

/*
 * Save current geometry for later restore (maximize/expand undo).
 */
void cow_view_save_geometry(struct cow_view *);

/*
 * Set or clear the unified `maximized` flag and the per-axis
 * (maximized_h, maximized_v) bits in lockstep.  Callers in code
 * paths that don't track an axis (state restore, drag-to-unmax,
 * fullscreen toggle, etc.) should use this rather than poking the
 * fields directly so per-axis consumers stay consistent.
 */
void cow_view_set_maximized(struct cow_view *, bool);

/*
 * Set pending geometry change (deferred to manage callback).
 * Also marks decoration dirty and requests a manage cycle.
 */
void cow_view_set_pending_geometry(struct cow_view *, int, int, int, int);

/*
 * Compute the frame bounding box (border + titlebar + content).
 */
struct cow_box cow_view_frame_box(const struct cow_view *);

/*
 * Like cow_view_frame_box, but assume the view's top-left content
 * origin is at (x, y) instead of (view->x, view->y).  Used when
 * computing a hypothetical post-move frame (e.g. clamp checks during
 * a drag) without mutating view->x/y.
 */
struct cow_box cow_view_frame_box_at(const struct cow_view *, int, int);

/*
 * Find a non-closed view by its persistent identifier string.
 * Returns NULL if no match.  Used by IPC and state-restore lookup
 * paths that previously open-coded the same loop.
 */
struct cow_view *cow_view_find_by_identifier(const char *);

/*
 * Test whether 'view' matches the given selector spec.  Selector
 * grammar matches the cmd-focus -t form:
 *   %app_id   exact match on app_id
 *   /title/   substring match on title (trailing '/' optional)
 *   #id       exact match on identifier
 *
 * Returns false on NULL/empty/malformed spec or when no match.
 */
bool cow_view_matches_selector(const struct cow_view *, const char *);

/*
 * Walk the view list circularly from start, returning the first
 * view matching pred.  Returns NULL if none found.
 */
struct cow_view *cow_view_walk(struct cow_view *, bool,
    bool (*)(const struct cow_view *));

/*
 * Maximize/unmaximize a view.
 */
int  cow_view_maximize_button_index(const struct cow_view *);
void cow_view_maximize(struct cow_view *);
void cow_view_unmaximize(struct cow_view *);

/*
 * Shade/unshade a view.
 */
void cow_view_shade(struct cow_view *, int);
void cow_view_unshade(struct cow_view *);

/*
 * Fullscreen/unfullscreen a view.
 */
void cow_view_fullscreen(struct cow_view *);
void cow_view_exit_fullscreen(struct cow_view *);

/*
 * Iconify (minimize) a view -- hide the window and show a small
 * icon on the desktop.  Clicking the icon deiconifies.
 */
void cow_view_iconify(struct cow_view *);
void cow_view_deiconify(struct cow_view *);

/*
 * Render the icon surface for an iconified view.
 * Called during the render sequence.
 */
void cow_view_icon_render(struct cow_view *);

/*
 * Recompute icon positions for all iconified views on an output.
 */
void cow_view_layout_icons_on_output(struct cow_output *);

bool cow_view_is_iconified(const struct cow_view *);

/* -----------------------------------------------------------------------
 * Container API
 * ----------------------------------------------------------------------- */

/*
 * Create a new container from two uncontained views.
 * first_view becomes the active (visible) member.
 */
struct cow_container *cow_container_create(struct cow_view *,
    struct cow_view *);

/*
 * Add a view to an existing container.  The view must not already be
 * in a container.  Returns false if the view is already a member.
 */
bool cow_container_add(struct cow_container *, struct cow_view *);

/*
 * Remove a view from its container.  If the container drops to one
 * member, that member is also released and the container is destroyed.
 * The detached view is given a cascade-offset position relative to the
 * container.
 */
void cow_container_remove_view(struct cow_view *);

/*
 * Make member the active (visible) tab.  Destroys the old active
 * member's decoration and marks the new one dirty.
 */
void cow_container_activate(struct cow_container *,
    struct cow_container_member *);

/*
 * Activate the next or previous tab in the container.
 */
void cow_container_activate_next(struct cow_container *);
void cow_container_activate_prev(struct cow_container *);

/*
 * Move the active tab one position to the right (next) or left (prev)
 * within the container's tab strip.  The tab remains active after the
 * swap.  No-ops if the tab is already at the respective end.
 */
void cow_container_swap_next(struct cow_container *);
void cow_container_swap_prev(struct cow_container *);

/*
 * Return the container that contains view, or NULL.
 */
struct cow_container *cow_view_container(const struct cow_view *);

/*
 * Return true if view is the active tab in its container.
 */
bool cow_view_is_active_tab(const struct cow_view *);

/*
 * Destroy a container and release all its members (detach, no cascade).
 * Called during WM shutdown.
 */
void cow_container_destroy(struct cow_container *);

/*
 * Return true if view is the drop indicator target for a container drag.
 * The view's decoration will draw a highlight when this is set.
 */
bool cow_view_is_drop_target(const struct cow_view *);

/*
 * If view is in a container, propagate view->x/view->y to the container
 * and to every other member's x/y so that all members stay in sync.
 * Must be called after any operation that changes a container member's
 * position (move, snap, centre, etc.).
 */
void cow_view_sync_container_pos(struct cow_view *);

/*
 * Sync the container's full geometry (position AND dimensions) from the
 * given explicit values.  Used by resize operations where the new w/h
 * come from proposed values before the dimensions event has arrived,
 * so view->content_width/height are not yet updated.
 * Also queues pending_resize for all non-active members.
 */
void cow_view_sync_container_geometry(struct cow_view *, int, int, int, int);

void cow_view_warp_to_focused(void);
void cow_view_centre(const struct cow_view *, int *, int *);

/* Minimum content width/height for resize/clamp operations. */
#define COW_MIN_WINDOW_DIM 50

void cow_clamp_dimensions(int *, int *);

/*
 * True if 'other' should be skipped during a snap/expand edge search
 * from 'self': it is 'self' itself, closed, iconified, or not on the
 * same output.  Callers that also require the window to have received
 * dimensions should check !other->has_dimensions separately.
 */
bool cow_view_skip_for_expand(const struct cow_view *, const struct cow_view *);

/*
 * Synchronise view->vx/vy (virtual/page-relative) from view->x/y
 * (global compositor coordinates), using the view's output and desk
 * page viewport offset.  No-op if view->output or view->desk is NULL.
 */
void cow_view_sync_vxy_from_global(struct cow_view *);

/*
 * The inverse: compute view->x/y from view->vx/vy and the current
 * page viewport offset.  No-op if view->output or view->desk is NULL.
 */
void cow_view_sync_global_from_vxy(struct cow_view *);

/*
 * Restore a view to its saved (pre-maximise/pre-expand) geometry.
 * saved_x/y are in virtual coordinates; this converts to global
 * coords, issues a pending-resize to saved_width/height, and clears
 * the maximized flag and the icon inverted-button indicator.
 */
void cow_view_restore_saved_geometry(struct cow_view *);

/*
 * JSON serialisation helpers for view metadata.
 * Each adds scalar fields directly to 'obj'; callers are responsible
 * for wrapping the object into the parent structure they need.
 *
 * _identity  adds: id, app_id, title
 * _placement adds: desk, output
 */
struct cJSON;
void cow_view_json_add_identity(struct cJSON *, const struct cow_view *);
void cow_view_json_add_placement(struct cJSON *, const struct cow_view *);

#endif
