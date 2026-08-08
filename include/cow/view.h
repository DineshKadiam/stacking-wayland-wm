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

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
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
#define COW_VIEW_CIRCULATE_SKIP_FLAG (1 << 3)

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

enum cow_view_cfg_option {
	COW_VIEW_CFG_BORDER,
	COW_VIEW_CFG_CORNER_LENGTH,
	COW_VIEW_CFG_HANDLES,
	COW_VIEW_CFG_BORDER_STYLE,
	COW_VIEW_CFG_BORDER_ACTIVE,
	COW_VIEW_CFG_BORDER_ACTIVE_N,
	COW_VIEW_CFG_BORDER_ACTIVE_S,
	COW_VIEW_CFG_BORDER_ACTIVE_E,
	COW_VIEW_CFG_BORDER_ACTIVE_W,
	COW_VIEW_CFG_BORDER_ACTIVE_NW,
	COW_VIEW_CFG_BORDER_ACTIVE_NE,
	COW_VIEW_CFG_BORDER_ACTIVE_SW,
	COW_VIEW_CFG_BORDER_ACTIVE_SE,
	COW_VIEW_CFG_BORDER_ACTIVE_GRADIENT,
	COW_VIEW_CFG_BORDER_ACTIVE_GRADIENT_N,
	COW_VIEW_CFG_BORDER_ACTIVE_GRADIENT_S,
	COW_VIEW_CFG_BORDER_ACTIVE_GRADIENT_E,
	COW_VIEW_CFG_BORDER_ACTIVE_GRADIENT_W,
	COW_VIEW_CFG_BORDER_ACTIVE_GRADIENT_NW,
	COW_VIEW_CFG_BORDER_ACTIVE_GRADIENT_NE,
	COW_VIEW_CFG_BORDER_ACTIVE_GRADIENT_SW,
	COW_VIEW_CFG_BORDER_ACTIVE_GRADIENT_SE,
	COW_VIEW_CFG_BORDER_ACTIVE_IMAGE,
	COW_VIEW_CFG_BORDER_ACTIVE_IMAGE_N,
	COW_VIEW_CFG_BORDER_ACTIVE_IMAGE_S,
	COW_VIEW_CFG_BORDER_ACTIVE_IMAGE_E,
	COW_VIEW_CFG_BORDER_ACTIVE_IMAGE_W,
	COW_VIEW_CFG_BORDER_ACTIVE_IMAGE_NW,
	COW_VIEW_CFG_BORDER_ACTIVE_IMAGE_NE,
	COW_VIEW_CFG_BORDER_ACTIVE_IMAGE_SW,
	COW_VIEW_CFG_BORDER_ACTIVE_IMAGE_SE,
	COW_VIEW_CFG_BORDER_INACTIVE,
	COW_VIEW_CFG_BORDER_INACTIVE_N,
	COW_VIEW_CFG_BORDER_INACTIVE_S,
	COW_VIEW_CFG_BORDER_INACTIVE_E,
	COW_VIEW_CFG_BORDER_INACTIVE_W,
	COW_VIEW_CFG_BORDER_INACTIVE_NW,
	COW_VIEW_CFG_BORDER_INACTIVE_NE,
	COW_VIEW_CFG_BORDER_INACTIVE_SW,
	COW_VIEW_CFG_BORDER_INACTIVE_SE,
	COW_VIEW_CFG_BORDER_INACTIVE_GRADIENT,
	COW_VIEW_CFG_BORDER_INACTIVE_GRADIENT_N,
	COW_VIEW_CFG_BORDER_INACTIVE_GRADIENT_S,
	COW_VIEW_CFG_BORDER_INACTIVE_GRADIENT_E,
	COW_VIEW_CFG_BORDER_INACTIVE_GRADIENT_W,
	COW_VIEW_CFG_BORDER_INACTIVE_GRADIENT_NW,
	COW_VIEW_CFG_BORDER_INACTIVE_GRADIENT_NE,
	COW_VIEW_CFG_BORDER_INACTIVE_GRADIENT_SW,
	COW_VIEW_CFG_BORDER_INACTIVE_GRADIENT_SE,
	COW_VIEW_CFG_BORDER_INACTIVE_IMAGE,
	COW_VIEW_CFG_BORDER_INACTIVE_IMAGE_N,
	COW_VIEW_CFG_BORDER_INACTIVE_IMAGE_S,
	COW_VIEW_CFG_BORDER_INACTIVE_IMAGE_E,
	COW_VIEW_CFG_BORDER_INACTIVE_IMAGE_W,
	COW_VIEW_CFG_BORDER_INACTIVE_IMAGE_NW,
	COW_VIEW_CFG_BORDER_INACTIVE_IMAGE_NE,
	COW_VIEW_CFG_BORDER_INACTIVE_IMAGE_SW,
	COW_VIEW_CFG_BORDER_INACTIVE_IMAGE_SE,
	COW_VIEW_CFG_TITLEBAR_ENABLED,
	COW_VIEW_CFG_TITLEBAR_ACTIVE_COLOUR,
	COW_VIEW_CFG_TITLEBAR_INACTIVE_COLOUR,
	COW_VIEW_CFG_TITLEBAR_ACTIVE_GRADIENT,
	COW_VIEW_CFG_TITLEBAR_INACTIVE_GRADIENT,
	COW_VIEW_CFG_TITLEBAR_ACTIVE_IMAGE,
	COW_VIEW_CFG_TITLEBAR_INACTIVE_IMAGE,
	COW_VIEW_CFG_TITLEBAR_FG_ACTIVE,
	COW_VIEW_CFG_TITLEBAR_FG_INACTIVE,
	COW_VIEW_CFG_TITLEBAR_HEIGHT,
	COW_VIEW_CFG_TITLEBAR_POSITION,
	COW_VIEW_CFG_TITLEBAR_SQUEEZE,
	COW_VIEW_CFG_TITLEBAR_SQUEEZE_JUSTIFY,
	COW_VIEW_CFG_TITLEBAR_SQUEEZE_WIDTH,
	COW_VIEW_CFG_TITLEBAR_SQUEEZE_OFFSET,
	COW_VIEW_CFG_TITLEBAR_SQUEEZE_MIN_WIDTH,
	COW_VIEW_CFG_TITLEBAR_BUTTON_0_VECTOR,
	COW_VIEW_CFG_TITLEBAR_BUTTON_0_ACTION,
	COW_VIEW_CFG_TITLEBAR_BUTTON_0_IMAGE,
	COW_VIEW_CFG_TITLEBAR_BUTTON_1_VECTOR,
	COW_VIEW_CFG_TITLEBAR_BUTTON_1_ACTION,
	COW_VIEW_CFG_TITLEBAR_BUTTON_1_IMAGE,
	COW_VIEW_CFG_TITLEBAR_BUTTON_2_VECTOR,
	COW_VIEW_CFG_TITLEBAR_BUTTON_2_ACTION,
	COW_VIEW_CFG_TITLEBAR_BUTTON_2_IMAGE,
	COW_VIEW_CFG_TITLEBAR_BUTTON_3_VECTOR,
	COW_VIEW_CFG_TITLEBAR_BUTTON_3_ACTION,
	COW_VIEW_CFG_TITLEBAR_BUTTON_3_IMAGE,
	COW_VIEW_CFG_TITLEBAR_BUTTON_4_VECTOR,
	COW_VIEW_CFG_TITLEBAR_BUTTON_4_ACTION,
	COW_VIEW_CFG_TITLEBAR_BUTTON_4_IMAGE,
	COW_VIEW_CFG_TITLEBAR_BUTTON_5_VECTOR,
	COW_VIEW_CFG_TITLEBAR_BUTTON_5_ACTION,
	COW_VIEW_CFG_TITLEBAR_BUTTON_5_IMAGE,
	COW_VIEW_CFG_TITLEBAR_BUTTON_6_VECTOR,
	COW_VIEW_CFG_TITLEBAR_BUTTON_6_ACTION,
	COW_VIEW_CFG_TITLEBAR_BUTTON_6_IMAGE,
	COW_VIEW_CFG_TITLEBAR_BUTTON_7_VECTOR,
	COW_VIEW_CFG_TITLEBAR_BUTTON_7_ACTION,
	COW_VIEW_CFG_TITLEBAR_BUTTON_7_IMAGE,
	COW_VIEW_CFG_TITLEBAR_BUTTON_8_VECTOR,
	COW_VIEW_CFG_TITLEBAR_BUTTON_8_ACTION,
	COW_VIEW_CFG_TITLEBAR_BUTTON_8_IMAGE,
	COW_VIEW_CFG_TITLEBAR_BUTTON_9_VECTOR,
	COW_VIEW_CFG_TITLEBAR_BUTTON_9_ACTION,
	COW_VIEW_CFG_TITLEBAR_BUTTON_9_IMAGE,
	COW_VIEW_CFG_CONTAINER_TAB_ACTIVE_COLOUR,
	COW_VIEW_CFG_CONTAINER_TAB_INACTIVE_COLOUR,
	COW_VIEW_CFG_CONTAINER_TAB_ACTIVE_GRADIENT,
	COW_VIEW_CFG_CONTAINER_TAB_INACTIVE_GRADIENT,
	COW_VIEW_CFG_CONTAINER_TAB_ACTIVE_IMAGE,
	COW_VIEW_CFG_CONTAINER_TAB_INACTIVE_IMAGE,
	COW_VIEW_CFG_CONTAINER_TAB_FG_ACTIVE,
	COW_VIEW_CFG_CONTAINER_TAB_FG_INACTIVE,
	COW_VIEW_CFG_CONTAINER_TAB_SUNKEN,
	COW_VIEW_CFG_FONT,
	COW_VIEW_CFG_TITLE_FORMAT,
	COW_VIEW_CFG_ICON_ENABLED,
	COW_VIEW_CFG_ICON_SIZE,
	COW_VIEW_CFG_ICON_IMAGE,
	COW_VIEW_CFG_ICON_BACKGROUND,
	COW_VIEW_CFG_ICON_FOREGROUND,
	COW_VIEW_CFG_ICON_TITLE_RELIEF,
	COW_VIEW_CFG_OPTION_COUNT,
};

#define COW_VIEW_CFG_OVERRIDE_BITS_PER_WORD (sizeof(uint64_t) * CHAR_BIT)
#define COW_VIEW_CFG_OVERRIDE_WORDS \
	((COW_VIEW_CFG_OPTION_COUNT + COW_VIEW_CFG_OVERRIDE_BITS_PER_WORD - 1) / \
	    COW_VIEW_CFG_OVERRIDE_BITS_PER_WORD)

/* Per-view visual configuration snapshot. */
struct cow_view_cfg {
	uint64_t	      overrides[COW_VIEW_CFG_OVERRIDE_WORDS];
	int		      border;
	int		      corner_length;
	bool		      handles;
	enum cow_border_style border_style;

	struct cow_border_colour border_active;
	struct cow_border_colour border_inactive;

	struct cow_titlebar_config  titlebar;
	struct cow_container_config container;

	char *font_name;
	char *title_format;

	bool icon_enabled;
	int icon_size;
	char *icon_image;
	float icon_background[4];
	float icon_foreground[4];
	bool icon_title_relief;
};

/* Effective application/override image used by icons and module clients. */
const char *cow_view_icon_image(const struct cow_view *);

/* Initialise/free/update a visual snapshot. */
void cow_view_cfg_init_from_global(struct cow_view_cfg *);
void cow_view_cfg_apply_global(struct cow_view_cfg *);
void cow_view_cfg_apply_global_option(struct cow_view_cfg *,
    enum cow_view_cfg_option);
void cow_view_cfg_set_override(struct cow_view_cfg *, enum cow_view_cfg_option);
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
	char		*application_icon;
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
	uint32_t presentation_hint;

	/* State tracking */
	bool closed;
	bool configured; /* true after first propose_dimensions */
	bool use_ssd;	 /* true if we want server-side decorations */
	bool fullscreen;
	bool fullscreen_applied; /* true while River is placing fullscreen */
	/* Unified and per-axis maximized state. */
	bool maximized;
	bool maximized_h;
	bool maximized_v;
	bool expanded; /* window-expand restore/toggle state */
	uint32_t tiled_edges; /* river_window_v1 tiled edge hint */
	bool pending_tiled_edges;
	uint32_t pending_tiled_edges_value;
	bool pending_maximized_hint;
	bool pending_maximized_hint_value;
	bool pending_fullscreen_state;
	bool pending_fullscreen;
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
	int  pressed_button; /* -1 = none, otherwise titlebar button number */
	int  icon_inverted_button; /* -1 = none, otherwise titlebar button number */

	/* New window needs focus on next manage cycle */
	bool needs_focus;

	/* Saved geometry for maximize/restore */
	int saved_x, saved_y;
	int saved_width, saved_height;

	/* Saved geometry for window-expand restore. */
	int expanded_saved_x, expanded_saved_y;
	int expanded_saved_width, expanded_saved_height;

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
	int dimension_bound_width;
	int dimension_bound_height;

	/* Pending geometry change (applied during manage cycle) */
	bool pending_resize;
	int  pending_x, pending_y;
	int  pending_width, pending_height;

	/* Avoid showing a shrinking window at its new top-left too early. */
	bool deferred_position;
	int  deferred_x, deferred_y;
	bool deferred_anchor_right, deferred_anchor_bottom;
	int  deferred_anchor_right_x, deferred_anchor_bottom_y;

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
	bool			       icon_manual_position;
	bool			       icon_auto_position_valid;

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

int  cow_view_page_col(const struct cow_view *);
int  cow_view_page_row(const struct cow_view *);
bool cow_view_current_desk(const struct cow_view *);
bool cow_view_current_output(const struct cow_view *);
bool cow_view_current_page(const struct cow_view *);
void cow_view_set_page_sticky(struct cow_view *, bool);
bool cow_view_focusable(const struct cow_view *);
bool cow_view_circulatable(const struct cow_view *);

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

/* Set/clear the client's maximized state hint during the manage cycle. */
void cow_view_set_pending_maximized_hint(struct cow_view *, bool);

/* Inform the client of fullscreen state during a manage sequence. */
void cow_view_inform_fullscreen_state(struct cow_view *, bool);

/* Set/clear the client's tiled edge hint. */
void cow_view_set_tiled_edges(struct cow_view *, uint32_t);

/* Clear window-expand state without restoring geometry. */
void cow_view_clear_expanded_state(struct cow_view *);

/* Save/restore window-expand geometry. */
void cow_view_save_expanded_geometry(struct cow_view *);
void cow_view_restore_expanded_geometry(struct cow_view *);

/*
 * Set pending geometry change (deferred to manage callback).
 * Also marks decoration dirty and requests a manage cycle.
 */
void cow_view_set_pending_geometry(struct cow_view *, int, int, int, int);
void cow_view_set_pending_geometry_anchored(struct cow_view *, int, int, int,
    int, bool, bool);

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
void cow_view_reflow_maximized(struct cow_view *);

/*
 * Shade/unshade a view.
 */
void cow_view_shaded_content_size(const struct cow_view *, int, int *, int *);
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
void cow_view_icon_refresh(struct cow_view *);

/*
 * Recompute icon positions for all iconified views on an output.
 */
void cow_view_layout_icons_on_output(struct cow_output *);
void cow_view_invalidate_auto_icon_positions(struct cow_output *);

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
