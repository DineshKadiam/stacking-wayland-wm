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

struct cow_view;
struct cow_desk;
struct cow_output;

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

/* -----------------------------------------------------------------------
 * Container API
 * ----------------------------------------------------------------------- */

/* Create a container from two uncontained views. */
struct cow_container *cow_container_create(struct cow_view *,
    struct cow_view *);

/* Add/remove a view from a container. */
bool cow_container_add(struct cow_container *, struct cow_view *);
void cow_container_remove_view(struct cow_view *);
void cow_container_forget_view(struct cow_view *);

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

/* Sync container members from the active member. */
void cow_view_sync_container_pos(struct cow_view *);

/* Sync container geometry from explicit values. */
void cow_view_sync_container_geometry(struct cow_view *, int, int, int, int);

#endif
