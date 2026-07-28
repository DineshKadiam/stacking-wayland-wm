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

/* Desk limits. */
#define COW_MAX_DESK_COUNT 32

#define COW_DEFAULT_DESK_COUNT 10

/* Compatibility alias. */
#define COW_NR_OF_DESKS COW_DEFAULT_DESK_COUNT

struct river_output_v1;

struct cow_desk {
	uint8_t		   nr;
	char		  *name;
	struct wl_list	   views;	/* cow_view.desk_views */
	struct wl_list	   set_link;	/* in cow_desk_set.desks */
	struct cow_output *output;

	int page_vx;
	int page_vy;

	int  scroll_from_vx, scroll_from_vy;
	int  scroll_to_vx, scroll_to_vy;
	long scroll_start_ms;
	bool scrolling;
};

struct cow_desk_set {
	struct wl_list desks;	/* cow_desk.set_link, ordered by nr */
	uint8_t	       count;
	uint8_t	       next_nr;
	bool	       user_named;
};

/* A managed river output. */
struct cow_output {
	struct river_output_v1 *river_output;
	struct wl_list		wm_link; /* cow_wm.outputs */

	/* Output geometry in compositor logical coordinates */
	int x, y;
	int width, height;

	/* Output area after layer-shell/reserved exclusions. */
	struct cow_box usable_area;
	struct cow_box layer_usable_area;
	bool	       has_layer_usable_area;

	/* Corresponding wl_output global name (for wl_registry binding) */
	uint32_t wl_output_name;

	/* Bound wl_output object (for name/description events) */
	struct wl_output *wl_output;

	/* Human-readable output name (e.g. "DP-1", "HDMI-A-1") */
	char *name;

	/* Stable 1-based raster-order ordinal. */
	int number;

	/* Per-output background colour (overrides global if set) */
	float bg_colour[4];
	bool  has_bg_colour;

	struct cow_desk_set desk_set;
	struct cow_desk	   *current_desk;

	/* Desk collect overlay bitmask. */
	uint32_t collected_desks;

	/* Views currently visible on this output */
	struct wl_list visible_views; /* cow_view.output_views */

	bool removed;

	/* Background layer-shell surface (filled with background colour) */
	struct wl_surface	     *bg_surface;
	struct zwlr_layer_surface_v1 *bg_layer_surface;
	struct cow_shm_buffer	     *bg_buffer;
	bool			      bg_configured;
	bool			      bg_dirty;

	/* Per-output layer-shell state (opaque to consumers). */
	struct cow_layer_shell_output *layer_shell_state;

	/* Edge-scroll dwell state. */
	int  pan_edge;
	int  pan_resist_motion;
	bool pan_resist_done;
	long pan_arm_ms;
	long pan_giveup_until_ms;
	long pan_last_fire_ms;

	/* Pan frame input surfaces.  Index: 0=top 1=bottom 2=left 3=right. */
	struct wl_surface	     *pan_surf[4];
	struct zwlr_layer_surface_v1 *pan_lsurf[4];
	struct cow_shm_buffer	     *pan_buf_clear[4];
	struct cow_shm_buffer	     *pan_buf_orange[4];
	struct cow_shm_buffer	     *pan_buf_fill[4][5]; /* 20/40/60/80/100% */
	int   pan_fill_level[4];     /* 0..5 currently displayed */
	void *pan_ctx[4];	     /* pan_frame_ctx*, freed on destroy */
	bool  pan_entered[4];	     /* pointer is over this frame */
};

/* Create/destroy output wrappers. */
struct cow_output *cow_output_create(struct river_output_v1 *);
void cow_output_destroy(struct cow_output *);
void cow_output_set_current(struct cow_output *);

/* Called once the human-readable output name is known. */
void cow_output_name_known(struct cow_output *);

/* Recompute output->usable_area from River layer-shell state and reserved area. */
bool cow_output_recompute_usable_area(struct cow_output *);

/* Background surface lifecycle. */
void cow_output_prepare_background(struct cow_output *);
void cow_output_render_background(struct cow_output *);

struct cow_output *cow_output_at(int, int);
struct cow_output *cow_output_find(struct river_output_v1 *);
struct cow_output *cow_output_find_by_name(const char *);
struct cow_output *cow_output_find_by_spec(const char *);

/* Bottom edge of the output union. */
int cow_outputs_max_bottom(void);

void cow_output_update_current(void);

/* Recompute output->number in raster-scan order. */
void cow_outputs_renumber(void);

/* Desk management. */
void		 cow_desk_set_init(struct cow_desk_set *);
void		 cow_desk_set_fini(struct cow_desk_set *);
struct cow_desk *cow_desk_set_append_default(struct cow_output *);
void		 cow_desk_set_populate_defaults(struct cow_output *);

struct cow_desk *cow_desk_by_nr(struct cow_output *, uint8_t nr);
struct cow_desk *cow_desk_by_name(struct cow_output *, const char *);
struct cow_desk *cow_desk_first(struct cow_output *);
struct cow_desk *cow_desk_last(struct cow_output *);
struct cow_desk *cow_desk_next(struct cow_output *, struct cow_desk *);
struct cow_desk *cow_desk_prev(struct cow_output *, struct cow_desk *);
uint8_t		 cow_desk_count(const struct cow_output *);

bool cow_desk_name_valid(const char *, char **cause);
bool cow_desk_set_name(struct cow_output *, uint8_t , const char *, char **);
bool cow_desk_set_delete(struct cow_output *, uint8_t, char **);
void cow_desk_set_reset(struct cow_output *);
bool cow_desk_set_resize(struct cow_output *, uint8_t count, char **cause);
bool cow_desk_sets_resize_all(uint8_t count, char **cause);

#endif
