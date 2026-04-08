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
#include "cow/desk_ops.h"

#include <stdbool.h>
#include <stdio.h>

#include "cow/compat.h"
#include "cow/config.h"
#include "cow/output.h"
#include "cow/seat.h"
#include "cow/status.h"
#include "cow/view.h"
#include "cow/wm.h"

#include "river-window-management-v1-client-protocol.h"

/*
 * A view is visible on the given output's current desk if:
 * - It is on the current desk, OR
 * - It has the STICKY flag set
 * AND it is not iconified.
 */
bool cow_view_visible(const struct cow_view *view,
                      const struct cow_output *output) {
  if (view->closed)
    return false;
  if (view->flags & COW_VIEW_ICONIFIED_FLAG)
    return false;
  if (view->output != output)
    return false;
  /* Non-active container members are always hidden */
  if (view->container_member != NULL &&
      view->container_member->container->active != view->container_member)
    return false;
  if (view->desk == NULL)
    return true;
  if (view->desk == output->current_desk)
    return true;
  if (view->flags & COW_VIEW_STICKY_FLAG)
    return true;
  /* Visible if on a collected desk */
  if (view->desk != NULL && (output->collected_desks & (1u << view->desk->nr)))
    return true;
  return false;
}

/* Backward compat wrapper */
int cow_view_visible_on_current(struct cow_view *view,
                                struct cow_output *output) {
  return cow_view_visible(view, output) ? 1 : 0;
}

/*
 * Switch a single output to a new desk (internal helper).
 */
static void switch_output_desk(struct cow_output *output, uint8_t desk_nr) {
  struct cow_desk *target = &output->desks[desk_nr];

  if (target == output->current_desk)
    return;

  /* Restore geometry for any collected windows before clearing */
  if (output->collected_desks != 0) {
    struct cow_view *cv;
    wl_list_for_each(cv, &cow_wm.views, wm_link) {
      if (cv->output != output || cv->closed)
        continue;
      if (!cv->collected_geo_saved)
        continue;
      cv->x = cv->collected_saved_x;
      cv->y = cv->collected_saved_y;
      cow_view_set_pending_geometry(
          cv, cv->collected_saved_x, cv->collected_saved_y,
          cv->collected_saved_width, cv->collected_saved_height);
      cv->collected_geo_saved = false;
    }
  }

  output->current_desk = target;
  output->collected_desks = 0;

  /* Re-resolve styles for views on this output */
  struct cow_view *view;
  wl_list_for_each(view, &cow_wm.views, wm_link) {
    if (view->output == output && !view->closed) {
      cow_view_resolve_style(view);
      view->dec.dirty = true;
    }
  }

  /* Clear focused view if it is no longer visible on this output. */
  if (cow_wm.focused_view != NULL &&
      !cow_view_visible(cow_wm.focused_view, output)) {
    cow_wm.focused_view = NULL;
    struct cow_seat *seat;
    wl_list_for_each(seat, &cow_wm.seats, wm_link) {
      if (seat->focused_view != NULL &&
          !cow_view_visible(seat->focused_view, output))
        seat->focused_view = NULL;
    }
  }
}

void cow_desk_switch(struct cow_output *output, uint8_t desk_nr) {
  if (desk_nr >= COW_NR_OF_DESKS)
    return;

  int mode = cow_config ? cow_config->desktop_config : COW_DESKTOP_GLOBAL;

  switch (mode) {
  case COW_DESKTOP_GLOBAL:
    /* All outputs switch to the same desk */
    {
      struct cow_output *out;
      wl_list_for_each(out, &cow_wm.outputs, wm_link) {
        switch_output_desk(out, desk_nr);
      }
    }
    break;

  case COW_DESKTOP_PER_OUTPUT:
    /* Only this output switches */
    switch_output_desk(output, desk_nr);
    break;

  case COW_DESKTOP_SHARED:
    /* Each output views a different desk number.  If another
     * output is already showing the target desk, swap with it. */
    {
      struct cow_output *other;
      wl_list_for_each(other, &cow_wm.outputs, wm_link) {
        if (other == output)
          continue;
        if (other->current_desk->nr == desk_nr) {
          /* Swap: other gets our current desk */
          uint8_t our_nr = output->current_desk->nr;
          switch_output_desk(other, our_nr);
          break;
        }
      }
      switch_output_desk(output, desk_nr);
    }
    break;
  }

  cow_status_emit();
  cow_wm.needs_manage_dirty = true;
}

void cow_view_assign_desk(struct cow_view *view, uint8_t desk_nr) {
  if (desk_nr >= COW_NR_OF_DESKS || view == NULL || view->closed)
    return;

  struct cow_output *output = view->output;
  if (output == NULL)
    return;

  struct cow_desk *target = &output->desks[desk_nr];

  if (view->desk == target)
    return;

  /* Remove from old desk's view list */
  wl_list_remove(&view->desk_views);

  /* Add to new desk */
  view->desk = target;
  wl_list_insert(&target->views, &view->desk_views);

  /* Re-resolve style since desk-constrained rules may differ */
  cow_view_resolve_style(view);
  view->dec.dirty = true;

  /* Visibility recomputed in render callback */
  cow_wm.needs_manage_dirty = true;
}
