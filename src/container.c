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
#include "cow/view.h"
#include "cow/decoration.h"
#include "cow/memory.h"
#include "cow/output.h"
#include "cow/update.h"
#include "cow/wm.h"

#include "river-window-management-v1-client-protocol.h"

static int next_container_id;

struct cow_container *
cow_container_create(struct cow_view *first, struct cow_view *second)
{
	struct cow_container *c = xcalloc(1, sizeof(*c));

	wl_list_init(&c->members);
	c->n_members = 0;
	c->id	     = next_container_id++;

	/* Geometry, desk and output come from the drop target (first) */
	c->x		  = first->x;
	c->y		  = first->y;
	c->content_width  = first->content_width;
	c->content_height = first->content_height;
	c->desk		  = first->desk;
	c->output	  = first->output;
	c->flags	  = first->flags;

	wl_list_insert(&cow_wm.containers, &c->wm_link);

	cow_container_add(c, first);
	cow_container_add(c, second);

	/* first (the drop target) is the active tab */
	c->active = wl_container_of(c->members.next, c->active, link);

	cow_update_request(COW_UPDATE_MANAGE);
	return c;
}

bool
cow_container_add(struct cow_container *c, struct cow_view *view)
{
	if (view->container_member != NULL)
		return false;

	struct cow_container_member *m = xcalloc(1, sizeof(*m));
	m->view			       = view;
	m->container		       = c;
	wl_list_insert(c->members.prev, &m->link); /* append */
	c->n_members++;

	view->container_member = m;

	/* Save pre-container geometry for restoration on detach */
	m->pre_x      = view->x;
	m->pre_y      = view->y;
	m->pre_width  = view->content_width;
	m->pre_height = view->content_height;

	/* Non-active members must not draw their own frame */
	if (c->active != NULL && c->active != m) {
		view->use_ssd = false;
		cow_view_drop_decoration(view);
	}

	/* Sync dimensions to container */
	cow_view_set_pending_geometry(view, c->x, c->y, c->content_width,
	    c->content_height);
	return true;
}

void
cow_container_activate(struct cow_container *c,
    struct cow_container_member		    *member)
{
	if (c->active == member)
		return;

	/* Deactivate the current active member.
	 * Do NOT destroy the decoration here: the old surface must remain
	 * alive until the render cycle so that window_interaction events
	 * from rapid clicks are still routed to a valid decoration surface.
	 * The render cycle's deferred cleanup (cow_view_apply_render) will
	 * destroy it once the new active member's decoration is ready. */
	if (c->active != NULL) {
		struct cow_view *old = c->active->view;
		old->use_ssd	     = false;
		old->dec.dirty	     = true;
		old->needs_raise     = false;
	}

	c->active		  = member;
	member->view->use_ssd	  = true;
	member->view->dec.dirty	  = true;
	member->view->needs_raise = true;

	/*
	 * Re-sync the new active member's geometry to the container's
	 * authoritative position.  Without this, a member whose vx/vy
	 * was set on a different page (e.g. the container was scrolled
	 * to another page after the member joined) would be deemed
	 * off-page by cow_view_visible() and silently hidden, making
	 * the tab click appear to do nothing while the user-visible
	 * focus appears to land on the wrong window.
	 */
	struct cow_view *nv = member->view;
	if (nv->output != NULL && nv->desk != NULL) {
		nv->x  = c->x;
		nv->y  = c->y;
		nv->vx = (nv->x - nv->output->x) + nv->desk->page_vx;
		nv->vy = (nv->y - nv->output->y) + nv->desk->page_vy;
	}

	cow_wm.pending_focus = member->view;
	cow_update_request(COW_UPDATE_MANAGE);
}

static void
container_redraw(struct cow_container *c)
{
	if (c->active != NULL && c->active->view->use_ssd)
		c->active->view->dec.dirty = true;
	cow_update_request(COW_UPDATE_MANAGE);
}

void
cow_container_swap_next(struct cow_container *c)
{
	if (c->active == NULL)
		return;
	struct wl_list *cur  = &c->active->link;
	struct wl_list *next = cur->next;
	if (next == &c->members)
		return; /* already the last tab */

	/* Remove cur from its current position then re-insert after next */
	wl_list_remove(cur);
	wl_list_insert(next, cur);
	container_redraw(c);
}

void
cow_container_swap_prev(struct cow_container *c)
{
	if (c->active == NULL)
		return;
	struct wl_list *cur  = &c->active->link;
	struct wl_list *prev = cur->prev;
	if (prev == &c->members)
		return; /* already the first tab */

	/* Remove cur then re-insert before prev (i.e. after prev->prev) */
	struct wl_list *before_prev = prev->prev;
	wl_list_remove(cur);
	wl_list_insert(before_prev, cur);
	container_redraw(c);
}

void
cow_container_activate_next(struct cow_container *c)
{
	struct cow_container_member *m;

	if (c->active == NULL) {
		if (wl_list_empty(&c->members))
			return;
		m = wl_container_of(c->members.next, m, link);
		cow_container_activate(c, m);
		return;
	}

	struct wl_list *next = c->active->link.next;
	if (next == &c->members)
		next = next->next; /* wrap */
	m = wl_container_of(next, m, link);
	cow_container_activate(c, m);
}

void
cow_container_activate_prev(struct cow_container *c)
{
	struct cow_container_member *m;

	if (c->active == NULL) {
		if (wl_list_empty(&c->members))
			return;
		m = wl_container_of(c->members.prev, m, link);
		cow_container_activate(c, m);
		return;
	}

	struct wl_list *prev = c->active->link.prev;
	if (prev == &c->members)
		prev = prev->prev; /* wrap */
	m = wl_container_of(prev, m, link);
	cow_container_activate(c, m);
}

void
cow_container_remove_view(struct cow_view *view)
{
	struct cow_container_member *m = view->container_member;
	if (m == NULL)
		return;

	struct cow_container *c		 = m->container;
	bool		      was_active = (c->active == m);

	wl_list_remove(&m->link);
	c->n_members--;
	view->container_member = NULL;
	int saved_pre_x	       = m->pre_x;
	int saved_pre_y	       = m->pre_y;
	int saved_pre_width    = m->pre_width;
	int saved_pre_height   = m->pre_height;
	xfree(m);
	m = NULL;

	/* Restore independent decoration */
	view->use_ssd = true;
	cow_decoration_fini(&view->dec);
	cow_decoration_init(view);

	/* Restore pre-container geometry */
	if (saved_pre_width > 0 && saved_pre_height > 0) {
		view->x = saved_pre_x;
		view->y = saved_pre_y;
		river_window_v1_propose_dimensions(view->river_window,
		    saved_pre_width, saved_pre_height);
		view->proposed_width  = saved_pre_width;
		view->proposed_height = saved_pre_height;
	} else {
		view->x = c->x + 30;
		view->y = c->y + 30;
	}
	view->dec.dirty = true;

	if (c->n_members <= 1) {
		/* Disband: release the last member too */
		if (!wl_list_empty(&c->members)) {
			struct cow_container_member *last =
			    wl_container_of(c->members.next, last, link);
			struct cow_view *lv = last->view;
			wl_list_remove(&last->link);
			c->n_members--;
			lv->container_member = NULL;
			xfree(last);
			lv->use_ssd = true;
			cow_decoration_fini(&lv->dec);
			cow_decoration_init(lv);
			lv->dec.dirty = true;
		}
		wl_list_remove(&c->wm_link);
		xfree(c);
	} else if (was_active) {
		/* Activate the first remaining member */
		struct cow_container_member *next =
		    wl_container_of(c->members.next, next, link);
		c->active = NULL; /* clear first so activate doesn't skip */
		cow_container_activate(c, next);
	}

	cow_update_request(COW_UPDATE_MANAGE);
}

void
cow_container_destroy(struct cow_container *c)
{
	struct cow_container_member *m, *tmp;
	wl_list_for_each_safe(m, tmp, &c->members, link)
	{
		m->view->container_member = NULL;
		m->view->use_ssd	  = true;
		wl_list_remove(&m->link);
		xfree(m);
	}
	wl_list_remove(&c->wm_link);
	xfree(c);
}


/*
 * Detach 'view' from its container without cascading (callers are
 * tearing the view down).  Frees the container if this was the last
 * member; otherwise re-activates a sibling and marks it for repaint.
 */
void
cow_container_forget_view(struct cow_view *view)
{
	if (view->container_member == NULL)
		return;
	struct cow_container_member *m		= view->container_member;
	struct cow_container	    *c		= m->container;
	bool			     was_active = (c->active == m);
	wl_list_remove(&m->link);
	c->n_members--;
	view->container_member = NULL;
	xfree(m);
	if (c->n_members == 0) {
		wl_list_remove(&c->wm_link);
		xfree(c);
	} else if (was_active) {
		c->active = wl_container_of(c->members.next, c->active, link);
		c->active->view->use_ssd   = true;
		c->active->view->dec.dirty = true;
	}
}


struct cow_container *
cow_view_container(const struct cow_view *view)
{
	if (view->container_member == NULL)
		return NULL;
	return view->container_member->container;
}

bool
cow_view_is_active_tab(const struct cow_view *view)
{
	if (view->container_member == NULL)
		return true; /* uncontained views are always "active" */
	return view->container_member->container->active ==
	    view->container_member;
}


void
cow_view_sync_container_pos(struct cow_view *view)
{
	if (view->container_member == NULL)
		return;

	struct cow_container	    *c = view->container_member->container;
	struct cow_container_member *m;

	c->x = view->x;
	c->y = view->y;

	wl_list_for_each(m, &c->members, link)
	{
		if (m->view == view)
			continue;
		m->view->x = c->x;
		m->view->y = c->y;

		/*
		 * Keep the sibling's virtual (vx/vy) coords in step with
		 * its global ones.  Without this, cow_view_apply_render
		 * later recomputes x/y from the stale vx/vy
		 */
		cow_view_sync_vxy_from_global(m->view);
		m->view->dec.dirty = true;
	}
}

void
cow_view_sync_container_geometry(struct cow_view *view, int x, int y, int w,
    int h)
{
	if (view->container_member == NULL)
		return;

	struct cow_container	    *c = view->container_member->container;
	struct cow_container_member *m;

	c->x		  = x;
	c->y		  = y;
	c->content_width  = w;
	c->content_height = h;

	wl_list_for_each(m, &c->members, link)
	{
		if (m->view == view)
			continue;
		m->view->x = x;
		m->view->y = y;

		cow_view_sync_vxy_from_global(m->view);
		m->view->dec.dirty = true;
		cow_view_set_pending_geometry(m->view, x, y, w, h);
	}
}
