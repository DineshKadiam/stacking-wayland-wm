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

/* Tabbed container membership. */
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

/* View flags. */
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

/* Per-view visual configuration snapshot. */
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

/* Initialise/free/update a visual snapshot. */
void cow_view_cfg_init_from_global(struct cow_view_cfg *);
void cow_view_cfg_apply_global(struct cow_view_cfg *);
void cow_view_cfg_fini(struct cow_view_cfg *);

/* A managed river window. */
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
	bool on_map_fired;   /* on-map/on-desk rules fired once */
	bool placed;

	/* Virtual desk-space position. */
	int vx, vy;

	/* Rendered global compositor position. */
	int x, y;

	/* The geometry we want the window to have (proposed) */
	int proposed_width;
	int proposed_height;

	/* Desk/output assignment */
	struct cow_desk	  *desk;
	struct cow_output *output;

	/* Orphan restore state. */
	int8_t orphan_desk_nr;
	int    orphan_page_vx;
	int    orphan_page_vy;

	char *orphan_output_name;

	/* Flags */
	uint32_t flags;

	/* Decoration hint from the window */
	uint32_t decoration_hint;

	/* State tracking */
	bool closed;
	bool configured; /* true after first propose_dimensions */
	bool use_ssd;	 /* true if we want server-side decorations */
	bool fullscreen;
	/* Unified and per-axis maximized state. */
	bool maximized;
	bool maximized_h;
	bool maximized_v;
	bool needs_raise; /* set during manage, applied in render */

	/* Monotonic intra-layer z-order. */
	uint64_t raise_seq;

	/* Stack layer. */
	uint8_t layer;

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

	/* Avoid showing a shrinking window at its new top-left too early. */
	bool deferred_position;
	int  deferred_x, deferred_y;

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

	/* Container membership, if any. */
	struct cow_container_member *container_member;

	struct cow_view_cfg cfg;
};

/* Create/destroy view wrappers. */
struct cow_view *cow_view_create(struct river_window_v1 *);
void cow_view_destroy(struct cow_view *);
void cow_view_clear_orphan(struct cow_view *);

/* Configure/render a view during WM cycles. */
void cow_view_configure(struct cow_view *);
void cow_view_apply_render(struct cow_view *);

struct cow_view *cow_view_find(struct river_window_v1 *);

/* Reassign a view to another output. */
void cow_view_reassign_output(struct cow_view *, struct cow_output *);

/* Update output assignment from current position. */
void cow_view_update_output(struct cow_view *);

bool cow_view_visible(const struct cow_view *, const struct cow_output *);

bool cow_view_focusable(const struct cow_view *);

/* Effective layer used by stack sorting. */
uint8_t cow_view_effective_layer(const struct cow_view *);

/* Set a view/container layer; returns the clamped value. */
uint8_t cow_view_set_layer(struct cow_view *, int layer);

/*
 * Save current geometry for later restore (maximize/expand undo).
 */
void cow_view_save_geometry(struct cow_view *);

/* Keep unified and per-axis maximized bits in sync. */
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

/* Compute a frame box at a hypothetical content origin. */
struct cow_box cow_view_frame_box_at(const struct cow_view *, int, int);

/* Find a non-closed view by persistent identifier. */
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

/* Walk the view list circularly from start. */
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

/* Iconify/deiconify a view. */
void cow_view_iconify(struct cow_view *);
void cow_view_deiconify(struct cow_view *);

/* Render an iconified view's icon surface. */
void cow_view_icon_render(struct cow_view *);

/*
 * Recompute icon positions for all iconified views on an output.
 */
void cow_view_layout_icons_on_output(struct cow_output *);

bool cow_view_is_iconified(const struct cow_view *);

/* -----------------------------------------------------------------------
 * Container API
 * ----------------------------------------------------------------------- */

/* Create a container from two uncontained views. */
struct cow_container *cow_container_create(struct cow_view *,
    struct cow_view *);

/* Add/remove a view from a container. */
bool cow_container_add(struct cow_container *, struct cow_view *);
void cow_container_remove_view(struct cow_view *);

/* Activate a container tab. */
void cow_container_activate(struct cow_container *,
    struct cow_container_member *);

void cow_container_activate_next(struct cow_container *);
void cow_container_activate_prev(struct cow_container *);

/* Move the active tab within the tab strip. */
void cow_container_swap_next(struct cow_container *);
void cow_container_swap_prev(struct cow_container *);

struct cow_container *cow_view_container(const struct cow_view *);
bool cow_view_is_active_tab(const struct cow_view *);

/* Destroy a container and detach its members. */
void cow_container_destroy(struct cow_container *);

bool cow_view_is_drop_target(const struct cow_view *);

/* Sync container members from the active member. */
void cow_view_sync_container_pos(struct cow_view *);

/* Sync container geometry from explicit values. */
void cow_view_sync_container_geometry(struct cow_view *, int, int, int, int);

void cow_view_warp_to_focused(void);
void cow_view_centre(const struct cow_view *, int *, int *);

/* Minimum content width/height for resize/clamp operations. */
#define COW_MIN_WINDOW_DIM 50

void cow_clamp_dimensions(int *, int *);

/* Common snap/expand skip predicate. */
bool cow_view_skip_for_expand(const struct cow_view *, const struct cow_view *);

/* Sync virtual coordinates from rendered coordinates. */
void cow_view_sync_vxy_from_global(struct cow_view *);

/* Sync rendered coordinates from virtual coordinates. */
void cow_view_sync_global_from_vxy(struct cow_view *);

/* Restore saved pre-maximize/pre-expand geometry. */
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
