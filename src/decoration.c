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
#include "cow/decoration.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <wayland-client.h>

#include "cow/border.h"
#include "cow/config.h"
#include "cow/font.h"
#include "cow/memory.h"
#include "cow/shm.h"
#include "cow/titlebar.h"
#include "cow/view.h"
#include "cow/wm.h"

#include "river-window-management-v1-client-protocol.h"

#define COW_MIN_ID_PREFIX 8

/* Global font for titlebar rendering.
 * Rebuilt whenever the font name changes (tracked via last_font_name). */
static struct cow_font titlebar_font;
static char	      *last_font_name = NULL;

static struct cow_font *
get_titlebar_font(void)
{
	const char *name = (cow_config && cow_config->font_name) ?
	    cow_config->font_name :
	    "monospace 10";

	/* Reinitialise whenever the configured font name changes. */
	if (last_font_name == NULL || strcmp(last_font_name, name) != 0) {
		if (last_font_name != NULL)
			cow_font_fini(&titlebar_font);
		cow_font_init(&titlebar_font, name);
		xfree(last_font_name);
		last_font_name = xstrdup(name);
	}

	return &titlebar_font;
}

/*
 * Build a display title of the form "[<short-id>] <title>".
 * The ID prefix is the shortest prefix of view->identifier (minimum
 * COW_MIN_ID_PREFIX characters) that is not a prefix of any other
 * live view's identifier.  Returns a heap-allocated string; caller
 * must free it.  Falls back to view->title if no identifier is set.
 */
static char *
make_display_title(const struct cow_view *view)
{
	const char *id	  = view->identifier;
	const char *title = view->title ? view->title : "";

	if (id == NULL || id[0] == '\0') {
		return xstrdup(title);
	}

	size_t id_len	  = strlen(id);
	size_t prefix_len = COW_MIN_ID_PREFIX;
	if (prefix_len > id_len)
		prefix_len = id_len;

	/* Extend prefix until no other view shares it */
	while (prefix_len < id_len) {
		bool		 unique = true;
		struct cow_view *other;
		wl_list_for_each(other, &cow_wm.views, wm_link)
		{
			if (other == view || other->closed)
				continue;
			if (other->identifier == NULL)
				continue;
			if (strncmp(id, other->identifier, prefix_len) == 0) {
				unique = false;
				break;
			}
		}
		if (unique)
			break;
		prefix_len++;
	}

	char *out = NULL;
	xasprintf(&out, "[%.*s] %s", (int)prefix_len, id, title);
	return out;
}

void
cow_decoration_init(struct cow_view_decoration *dec)
{
	memset(dec, 0, sizeof(*dec));
	dec->dirty	  = true;
	dec->border_width = cow_config ? cow_config->border :
					 COW_CFG_DEFAULT_BORDER;

	/* Enable titlebar when config says so */
	if (cow_config != NULL && cow_config->titlebar.enabled) {
		struct cow_font *font = get_titlebar_font();
		int		 h    = cow_config->titlebar.height;
		dec->title_thickness  = (h > 0) ?
		     h :
		     cow_titlebar_compute_thickness(font);
	} else {
		dec->title_thickness = 0;
	}
}

void
cow_decoration_fini(struct cow_view_decoration *dec)
{
	if (dec->buffer != NULL) {
		cow_shm_buffer_destroy(dec->buffer);
		dec->buffer = NULL;
	}
}

void
cow_decoration_update(struct cow_view *view, bool active)
{
	struct cow_view_decoration *dec = &view->dec;

	if (!view->use_ssd || !view->has_dimensions)
		return;

	/* Apply per-view style overrides */
	if (view->style.resolved) {
		dec->border_width = view->style.border;
		if (view->style.titlebar_enabled) {
			if (dec->title_thickness == 0) {
				struct cow_font *font = get_titlebar_font();
				int		 h    = cow_config ?
						    cow_config->titlebar.height :
						    0;
				dec->title_thickness  = (h > 0) ?
				     h :
				     cow_titlebar_compute_thickness(font);
			}
		} else {
			dec->title_thickness = 0;
		}
	}

	int bw	       = dec->border_width;
	int tt	       = dec->title_thickness;
	int top_border = bw + tt;

	/* When shaded, use saved dimensions for frame size but zero content */
	int  cw, ch;
	bool shaded = (view->sd != COW_SHADE_NONE);

	if (shaded) {
		int sw = view->shade_saved_width;
		int sh = view->shade_saved_height;

		switch (view->sd) {
		case COW_SHADE_NORTH:
			cw = sw;
			ch = 0;
			break;
		case COW_SHADE_SOUTH:
			cw = sw;
			ch = 0;
			break;
		case COW_SHADE_EAST:
			cw = 0;
			ch = sh;
			break;
		case COW_SHADE_WEST:
			cw = 0;
			ch = sh;
			break;
		case COW_SHADE_NW:
		case COW_SHADE_NE:
		case COW_SHADE_SW:
		case COW_SHADE_SE:
			cw = 0;
			ch = 0;
			break;
		default:
			cw = sw;
			ch = 0;
			break;
		}
	} else {
		cw = view->content_width;
		ch = view->content_height;
	}

	int frame_w = cw + 2 * bw;
	int frame_h = ch + top_border + bw;

	if (frame_w <= 0 || frame_h <= 0)
		return;

	/* Check if we need to regenerate */
	bool needs_regen = dec->dirty || active != dec->last_active ||
	    cw != dec->last_content_width || ch != dec->last_content_height;

	if (!needs_regen && dec->buffer != NULL)
		goto commit;

	/* Free old buffer if dimensions changed or it's busy */
	if (dec->buffer != NULL &&
	    (dec->buffer->width != frame_w || dec->buffer->height != frame_h ||
		dec->buffer->busy)) {
		cow_shm_buffer_destroy(dec->buffer);
		dec->buffer = NULL;
	}

	/* Allocate buffer if needed */
	if (dec->buffer == NULL) {
		dec->buffer = cow_shm_buffer_create(frame_w, frame_h);
		if (dec->buffer == NULL) {
			fprintf(stderr,
			    "cow: failed to create decoration buffer\n");
			return;
		}
	}

	/* Render the frame using the view's resolved style */
	{
		int stride_px = dec->buffer->stride / 4;

		/* Use per-view style colors if overridden by a rule,
		 * otherwise fall back to global config per-edge colors. */
		const struct cow_border_colour *bc;
		if (view->style.resolved && view->style.has_border_override)
			bc = active ? &view->style.border_active :
				      &view->style.border_inactive;
		else
			bc = active ? &cow_config->border_active :
				      &cow_config->border_inactive;

		/* Render border: fill_border_base always runs for
		 * per-edge active/inactive colors; FVWM/MWM add 3D
		 * relief on top; NONE = flat fill only. */
		int cl = (cow_config->handles &&
			     cow_config->corner_length > 0) ?
		    cow_config->corner_length :
		    0;
		if (cl > 0 && tt > 0 && cl < bw + tt)
			cl = bw + tt;
		/*
		 * When this view is a container drop target, override the
		 * entire frame (borders + titlebar) with solid orange so the
		 * user sees a clear highlight indicating where the drop will
		 * land.
		 */
		bool is_drop_target = cow_view_is_drop_target(view);
		struct cow_border_colour drop_bc;
		if (is_drop_target) {
			float orange[4] = { 1.0f, 0.53f, 0.0f, 1.0f };
			memcpy(drop_bc.n, orange, sizeof(float) * 4);
			memcpy(drop_bc.s, orange, sizeof(float) * 4);
			memcpy(drop_bc.e, orange, sizeof(float) * 4);
			memcpy(drop_bc.w, orange, sizeof(float) * 4);
			memcpy(drop_bc.nw, orange, sizeof(float) * 4);
			memcpy(drop_bc.ne, orange, sizeof(float) * 4);
			memcpy(drop_bc.sw, orange, sizeof(float) * 4);
			memcpy(drop_bc.se, orange, sizeof(float) * 4);
			drop_bc.has_nw = drop_bc.has_ne = true;
			drop_bc.has_sw = drop_bc.has_se = true;
			drop_bc.has_corners		= true;
			bc				= &drop_bc;
		}

		cow_border_style_render(dec->buffer->data, stride_px, frame_w,
		    frame_h, bc, bw, cl, cow_config->border_style);

		/* Render titlebar if enabled */
		if (tt > 0) {
			struct cow_font *font = get_titlebar_font();
			/* Use per-view titlebar colors if overridden */
			const float *tb_colour = NULL;
			const float *tb_fg     = NULL;
			if (view->style.resolved &&
			    view->style.has_titlebar_override) {
				tb_colour = active ?
				    view->style.titlebar_active :
				    view->style.titlebar_inactive;
			}
			if (view->style.resolved &&
			    view->style.has_titlebar_fg_override) {
				tb_fg = active ?
				    view->style.titlebar_fg_active :
				    view->style.titlebar_fg_inactive;
			}
			bool is_sticky = (view->flags & COW_VIEW_STICKY_FLAG) !=
			    0;

			/* Override titlebar to orange when this is a drop
			 * target */
			if (is_drop_target) {
				static float orange[4] = { 1.0f, 0.53f, 0.0f,
					1.0f };
				static float white[4]  = { 1.0f, 1.0f, 1.0f,
					 1.0f };
				tb_colour	       = orange;
				tb_fg		       = white;
			}

			/* Build tab info if this view is in a container */
			const char	    **tab_titles = NULL;
			int		      n_tabs	 = 0;
			int		      active_tab = 0;
			struct cow_container *ctr = cow_view_container(view);
			if (ctr != NULL && ctr->n_members > 1) {
				n_tabs	   = ctr->n_members;
				tab_titles = xcalloc(n_tabs, sizeof(char *));
				int			     idx = 0;
				struct cow_container_member *mem;
				wl_list_for_each(mem, &ctr->members, link)
				{
					const char *lbl = mem->view->app_id;
					if (lbl == NULL)
						lbl = mem->view->title;
					if (lbl == NULL)
						lbl = "?";
					tab_titles[idx] = lbl;
					if (mem == ctr->active)
						active_tab = idx;
					idx++;
				}
			}

			char *display_title = make_display_title(view);
			/*
			 * For unsqueezed views this is the full-width strip;
			 * for squeezed ones it is the cow_titlebar_squeezed
			 * rect.  The renderer clears any strip pixels
			 * outside the rect so they fall through to whatever
			 * is below.
			 */
			struct cow_box tb_rect = cow_titlebar_squeezed_rect(
			    view);
			cow_titlebar_render(dec->buffer->data, stride_px,
			    frame_w, bw, tb_rect, display_title, active, font,
			    view->pressed_button, view->icon_inverted_button,
			    view->title_pressed, tb_colour, tb_fg, is_sticky,
			    tab_titles, n_tabs, active_tab, is_drop_target);
			xfree(display_title);
			xfree(tab_titles);
		}
	}

	dec->frame_width	 = frame_w;
	dec->frame_height	 = frame_h;
	dec->dirty		 = false;
	dec->last_active	 = active;
	dec->last_content_width	 = cw;
	dec->last_content_height = ch;

commit:
	/* Ensure we have a decoration surface */
	if (view->decoration_surface == NULL) {
		view->decoration_surface = wl_compositor_create_surface(
		    cow_wm.compositor);
		if (view->decoration_surface == NULL)
			return;
	}

	if (view->decoration == NULL) {
		view->decoration = river_window_v1_get_decoration_below(
		    view->river_window, view->decoration_surface);
		if (view->decoration == NULL)
			return;
	}

	/* Set offset: position the frame so the client content area
	 * aligns with the window's top-left corner. */
	river_decoration_v1_set_offset(view->decoration, -bw, -top_border);

	/*
	 * Input region: set to the union of the borders and the
	 * (possibly squeezed) titlebar rectangle, so pointer events
	 * landing in the cleared pixels of the titlebar strip pass
	 * through the decoration surface to whatever is beneath.
	 *
	 * Regions live entirely in the decoration buffer coordinate
	 * system where (0, 0) is the top-left of the outer frame.
	 */
	struct wl_region *region = wl_compositor_create_region(
	    cow_wm.compositor);
	if (region != NULL) {
		/* Top border (full-width strip above the titlebar). */
		if (bw > 0)
			wl_region_add(region, 0, 0, frame_w, bw);
		/* Titlebar: the squeezed rectangle only. */
		if (tt > 0) {
			struct cow_box tbr = cow_titlebar_squeezed_rect(view);
			if (tbr.width > 0 && tbr.height > 0)
				wl_region_add(region, tbr.x, tbr.y, tbr.width,
				    tbr.height);
		}
		/* Left/right side borders next to content. */
		if (bw > 0) {
			wl_region_add(region, 0, top_border, bw,
			    frame_h - top_border - bw);
			wl_region_add(region, frame_w - bw, top_border, bw,
			    frame_h - top_border - bw);
		}
		/* Bottom border. */
		if (bw > 0)
			wl_region_add(region, 0, frame_h - bw, frame_w, bw);

		wl_surface_set_input_region(view->decoration_surface, region);
		wl_region_destroy(region);
	}

	/* Attach buffer and commit */
	if (dec->buffer != NULL) {
		wl_surface_attach(view->decoration_surface, dec->buffer->buffer,
		    0, 0);
		wl_surface_damage_buffer(view->decoration_surface, 0, 0,
		    frame_w, frame_h);
		river_decoration_v1_sync_next_commit(view->decoration);
		wl_surface_commit(view->decoration_surface);
		dec->buffer->busy = true;
	}
}
