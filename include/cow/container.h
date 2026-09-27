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
#if !defined(COW_CONTAINER_H)
#define COW_CONTAINER_H

#include <stdint.h>
#include <stdbool.h>
#include <wayland-util.h>
#include "cow/container_layout.h"

struct cow_view;
struct cow_desk;
struct cow_output;
struct wl_registry;

struct cow_container_frame_state {
	struct cow_box restore;
	uint32_t       flags;
	int	       layer, shade;
	bool	       maximized_h, maximized_v;
	bool	       ignore_h, ignore_v, keep_h, keep_v;
	bool	       pending;
};

enum cow_container_zoom {
	COW_CONTAINER_ZOOM_NONE,
	COW_CONTAINER_ZOOM_COLUMN,
	COW_CONTAINER_ZOOM_ALL,
};

struct cow_container_pane {
	enum cow_container_zoom		  zoom;
	char				 *name;
	struct cow_container_window	 *placeholder;
	int			     id;
	struct cow_container_layout  layout;
	struct wl_list		     tabs;
	struct cow_container_member *active;
	int			     n_tabs;
	bool			     title_hidden;
	/* Module-owned slot, relative to the host's content origin. */
	struct cow_box attachment;
	bool	       close_with_host;
};

/* Tabbed container membership. */
struct cow_container_member {
	struct wl_list		   link;      /* cow_container.members */
	struct wl_list		   pane_link; /* cow_container_pane.tabs */
	struct cow_container_pane *pane;
	struct cow_view		  *view;
	struct cow_container	  *container;
	/* Geometry saved when the view joined, restored on detach */
	int pre_x, pre_y;
	int pre_width, pre_height;
};

bool cow_container_is_attached_client(const struct cow_view *);
int	 cow_container_hidden_count(const struct cow_container *);
uint32_t cow_container_hidden_border_edges(const struct cow_view *);
bool	 cow_container_hidden_menu(struct cow_container *);

struct cow_container {
	char *name;
	char
	    *restore_identifier; /* Previous outer-window id, during restart. */
	struct cow_container_window *frame_window;
	struct cow_view		    *frame_view;
	/* A single outer tab awaiting its own native pane-layout frame. */
	struct cow_view	       *split_target, *split_source;
	enum cow_container_axis split_axis;
	bool			split_before;
	bool split_empty;
	bool persistent; /* Explicit layout: retain empty panes. */
	bool framed;
	bool attached; /* Existing client hosts fixed slots, not a native frame.
			*/
	bool syncing_frame;
	struct cow_container_frame_state frame_state;
	struct wl_list		     wm_link; /* cow_wm.containers */
	struct wl_list		     members; /* cow_container_member.link */
	int			     id;
	int			     n_members;
	struct cow_container_member *active;
	struct cow_container_pane   *root;

	/* Geometry (authoritative for all members) */
	int x, y;
	int content_width, content_height;
	/* Pane layout bounds; inside the outer window when framed is true. */
	struct cow_box frame;
	struct cow_box restore_frame; /* virtual coordinates, before maximize */
	bool	       maximized_h, maximized_v;
	bool	       shaded;

	struct cow_desk	  *desk;
	struct cow_output *output;
	uint32_t	   flags; /* sticky etc., shared across members */
};

/* -----------------------------------------------------------------------
 * Container API
 * ----------------------------------------------------------------------- */

/* Create a container from uncontained views; second may be NULL for naming. */
struct cow_container *cow_container_create(struct cow_view *,
    struct cow_view *);
struct cow_container *cow_container_create_empty(const char *, const char *);
struct cow_container_pane *cow_container_pane_next(const struct cow_container *,
    struct cow_container_pane *);
bool			   cow_container_split_empty(struct cow_container *,
			  struct cow_container_pane *, const char *, enum cow_container_axis, bool);
bool cow_container_insert(struct cow_container *, struct cow_container_pane *,
    struct cow_view *);
void cow_container_manage_windows(void);
bool cow_container_complete_tab_split(struct cow_container *);
void cow_container_render_windows(void);
void cow_container_window_bind(struct wl_registry *, uint32_t);
void cow_container_window_fini(void);
void cow_container_window_destroy(struct cow_container *);
void cow_container_set_frame(struct cow_container *, struct cow_view *);
void cow_container_window_geometry(struct cow_container *);
struct cow_container *cow_container_group(const struct cow_view *);
/* Outermost tab group, including panes carried by a tabbed frame. */
struct cow_container *cow_container_top_group(const struct cow_view *);
struct cow_view	     *cow_container_next_view(const struct cow_container *,
	 struct cow_view *);
bool		      cow_container_can_tab(const struct cow_container *,
		     const struct cow_view *);
int		      cow_container_window_count(const struct cow_container *);
int  cow_container_pane_window_count(const struct cow_container_pane *);
bool cow_container_add_placeholder(struct cow_container *,
    struct cow_container_pane *, struct cow_view *);
void cow_container_remove_empty_pane(struct cow_container *,
    struct cow_container_pane *);
void cow_container_window_identify(struct cow_view *);
void cow_container_window_forget(struct cow_view *);
bool cow_container_window_view(const struct cow_view *);
struct cow_container *cow_container_window_owner(const struct cow_view *);
void cow_container_placeholder_destroy(struct cow_container_pane *);
struct cow_container	  *cow_container_empty_at(int, int, struct cow_view *,
	 struct cow_container_pane **);
struct cow_view *cow_container_drop_target_at(int, int, struct cow_view *);
struct cow_view *cow_container_pane_drop_target_at(int, int, struct cow_view *);
bool		 cow_container_rearrange(struct cow_view *, struct cow_view *,
		enum cow_container_axis, bool);
struct cow_container	  *cow_container_find(const char *);
struct cow_container_pane *cow_container_pane_find(struct cow_container *,
    const char *);
bool cow_container_set_name(struct cow_view *, const char *);
bool cow_container_pane_set_name(struct cow_view *, const char *);
bool cow_container_rename(struct cow_container *, const char *);
bool cow_container_pane_rename(struct cow_container *,
    struct cow_container_pane *, const char *);

/* Add/remove a view from a container. */
bool cow_container_add(struct cow_container *, struct cow_view *);
bool cow_container_add_to(struct cow_view *, struct cow_view *);
/*
 * Split a tab away from another tab in the same pane. Both must already
 * belong to the container. Failure leaves membership and geometry intact.
 */
bool	       cow_container_split(struct cow_view *, struct cow_view *,
	      enum cow_container_axis, bool);
bool	       cow_container_arrange(struct cow_container *, struct cow_box);
bool	       cow_container_refresh(struct cow_view *);
bool	       cow_container_is_split(const struct cow_view *);
struct cow_box cow_container_frame_at(const struct cow_view *, int, int);
void cow_container_sync_frame(struct cow_view *);
bool cow_container_maximize(struct cow_view *, bool, bool, bool, bool, bool);
void cow_container_reflow(struct cow_view *);
bool cow_container_resize(struct cow_view *, uint32_t, struct cow_box,
    struct cow_box, int, int);
void	       cow_container_resize_save(struct cow_view *, uint32_t, int[2],
	      unsigned[2]);
bool cow_container_resize_restore(struct cow_view *, uint32_t, struct cow_box,
    const int[2], const unsigned[2]);
bool cow_container_collapse(struct cow_view *);
bool cow_container_title_visible(const struct cow_view *);
bool cow_container_title_hide(struct cow_view *);
bool cow_container_titles_restore(struct cow_view *);
bool cow_container_titles_toggle(struct cow_view *, bool);
bool cow_container_zoom(struct cow_view *, enum cow_container_zoom);
bool cow_container_pane_visible(struct cow_container *,
    struct cow_container_pane *, bool);
bool cow_container_attach(struct cow_view *, struct cow_view *, const char *,
    struct cow_box, bool, bool);
void cow_container_attachment_sync(struct cow_container *);
bool cow_container_attach_wait(struct cow_view *, const char *, const char *,
    const char *, struct cow_box, bool, bool);
void cow_container_attach_cancel(struct cow_view *, const char *);
void cow_container_attach_mapped(struct cow_view *);
bool cow_container_shade(struct cow_view *, bool);
bool cow_container_shaded_size(const struct cow_view *, int *, int *);
bool cow_container_equalize(struct cow_view *);
bool cow_container_rotate(struct cow_container *, unsigned);
struct cow_view *cow_container_neighbor(struct cow_view *, uint32_t);
void		 cow_container_sync_flags(struct cow_view *, uint32_t);
void		 cow_container_remove_view(struct cow_view *);
void		 cow_container_forget_view(struct cow_view *);

/* Activate a container tab. */
void cow_container_activate(struct cow_container *,
    struct cow_container_member *);

void cow_container_activate_next(struct cow_container *);
void cow_container_activate_prev(struct cow_container *);

/* Move the active tab within the tab strip. */
void cow_container_swap_next(struct cow_container *);
void cow_container_swap_prev(struct cow_container *);

struct cow_container	  *cow_view_container(const struct cow_view *);
/* Find a contained window through transient parents, without joining it. */
struct cow_view *cow_container_ancestor(struct cow_view *, unsigned *);
struct cow_container_pane *cow_view_pane(const struct cow_view *);
bool			   cow_view_is_active_tab(const struct cow_view *);
bool cow_container_hides_view(const struct cow_view *);
/* Borders supplied by the neighbouring pane to the left or above. */
uint32_t cow_container_shared_edges(const struct cow_view *);
struct cow_view *cow_container_border_title_target(struct cow_view *, int, int);
uint32_t cow_container_outer_edges(const struct cow_view *);

/* Destroy a container and detach its members. */
void cow_container_destroy(struct cow_container *);

/* Restart state. Restoring an invalid record leaves live views unchanged. */
cJSON		     *cow_container_save(const struct cow_container *);
struct cow_container *cow_container_restore(const cJSON *);
bool		      cow_container_restore_defer(const cJSON *);
void		      cow_container_restore_pending(void);
void		      cow_container_restore_clear(void);

/* Sync container members from the active member. */
void cow_view_sync_container_pos(struct cow_view *);

/* Sync container geometry from explicit values. */
void cow_view_sync_container_geometry(struct cow_view *, int, int, int, int);

#endif
