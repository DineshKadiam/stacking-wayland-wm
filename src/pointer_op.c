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
#include "cow/pointer_op.h"

#include <stdlib.h>

#include "cow/compat.h"
#include "cow/config.h"
#include "cow/output.h"
#include "cow/seat.h"
#include "cow/status.h"
#include "cow/view.h"
#include "cow/wm.h"

#include "river-window-management-v1-client-protocol.h"

/*
 * Snap a coordinate to nearby edges.  Checks usable area boundaries,
 * output boundaries, and other visible window frame edges.
 *
 * frame_left/top/right/bottom are the frame edges of the window
 * being moved/resized.  We snap each edge independently.
 */
static void snap_edges(struct cow_view *view, int *frame_left, int *frame_top,
                       int *frame_right, int *frame_bottom, bool snap_size) {
  int sd = cow_config ? cow_config->snap_distance : 0;
  if (sd <= 0)
    return;

  int fw = *frame_right - *frame_left;
  int fh = *frame_bottom - *frame_top;

  /* Collect snap candidates for each edge */
  int best_dl = sd + 1, best_dr = sd + 1;
  int best_dt = sd + 1, best_db = sd + 1;
  int snap_l = *frame_left, snap_r = *frame_right;
  int snap_t = *frame_top, snap_b = *frame_bottom;

  /* Check all output boundaries and usable areas */
  struct cow_output *out;
  wl_list_for_each(out, &cow_wm.outputs, wm_link) {
    int ol = out->x;
    int ot = out->y;
    int or_ = out->x + out->width;
    int ob = out->y + out->height;

    /* Output edges */
    int d;
    d = abs(*frame_left - ol);
    if (d < best_dl) {
      best_dl = d;
      snap_l = ol;
    }
    d = abs(*frame_right - or_);
    if (d < best_dr) {
      best_dr = d;
      snap_r = or_;
    }
    d = abs(*frame_top - ot);
    if (d < best_dt) {
      best_dt = d;
      snap_t = ot;
    }
    d = abs(*frame_bottom - ob);
    if (d < best_db) {
      best_db = d;
      snap_b = ob;
    }

    /* Usable area edges */
    struct cow_box *ua = &out->usable_area;
    int ul = ua->x;
    int ut = ua->y;
    int ur = ua->x + ua->width;
    int ub = ua->y + ua->height;

    d = abs(*frame_left - ul);
    if (d < best_dl) {
      best_dl = d;
      snap_l = ul;
    }
    d = abs(*frame_right - ur);
    if (d < best_dr) {
      best_dr = d;
      snap_r = ur;
    }
    d = abs(*frame_top - ut);
    if (d < best_dt) {
      best_dt = d;
      snap_t = ut;
    }
    d = abs(*frame_bottom - ub);
    if (d < best_db) {
      best_db = d;
      snap_b = ub;
    }
  }

  /* Check other visible window edges */
  struct cow_view *other;
  wl_list_for_each(other, &cow_wm.views, wm_link) {
    if (other == view || other->closed || !other->has_dimensions ||
        (other->flags & COW_VIEW_ICONIFIED_FLAG) ||
        (other->flags & COW_VIEW_ICONIFIED_FLAG))
      continue;

    struct cow_box of = cow_view_frame_box(other);
    int ol = of.x;
    int ot = of.y;
    int or_ = of.x + of.width;
    int ob = of.y + of.height;

    int d;
    /* Snap our left to their right, our right to their left */
    d = abs(*frame_left - or_);
    if (d < best_dl) {
      best_dl = d;
      snap_l = or_;
    }
    d = abs(*frame_left - ol);
    if (d < best_dl) {
      best_dl = d;
      snap_l = ol;
    }
    d = abs(*frame_right - ol);
    if (d < best_dr) {
      best_dr = d;
      snap_r = ol;
    }
    d = abs(*frame_right - or_);
    if (d < best_dr) {
      best_dr = d;
      snap_r = or_;
    }

    /* Snap our top to their bottom, our bottom to their top */
    d = abs(*frame_top - ob);
    if (d < best_dt) {
      best_dt = d;
      snap_t = ob;
    }
    d = abs(*frame_top - ot);
    if (d < best_dt) {
      best_dt = d;
      snap_t = ot;
    }
    d = abs(*frame_bottom - ot);
    if (d < best_db) {
      best_db = d;
      snap_b = ot;
    }
    d = abs(*frame_bottom - ob);
    if (d < best_db) {
      best_db = d;
      snap_b = ob;
    }
  }

  /* Apply snaps */
  if (snap_size) {
    /* Resize: snap individual edges */
    if (best_dl <= sd)
      *frame_left = snap_l;
    if (best_dr <= sd)
      *frame_right = snap_r;
    if (best_dt <= sd)
      *frame_top = snap_t;
    if (best_db <= sd)
      *frame_bottom = snap_b;
  } else {
    /* Move: snap the whole window (prefer closest edge) */
    if (best_dl <= sd && best_dl <= best_dr) {
      *frame_left = snap_l;
      *frame_right = snap_l + fw;
    } else if (best_dr <= sd) {
      *frame_right = snap_r;
      *frame_left = snap_r - fw;
    }
    if (best_dt <= sd && best_dt <= best_db) {
      *frame_top = snap_t;
      *frame_bottom = snap_t + fh;
    } else if (best_db <= sd) {
      *frame_bottom = snap_b;
      *frame_top = snap_b - fh;
    }
  }
}

void cow_pointer_op_start_move(struct cow_seat *seat, struct cow_view *view) {
  if (view == NULL || view->closed || seat->op_in_progress)
    return;

  seat->op.type = COW_POINTER_OP_MOVE;
  seat->op.view = view;
  seat->op.start_x = view->x;
  seat->op.start_y = view->y;
  seat->op_in_progress = true;
  seat->op_released = false;
  seat->op_dx = 0;
  seat->op_dy = 0;

  river_seat_v1_op_start_pointer(seat->river_seat);
}

void cow_pointer_op_start_resize(struct cow_seat *seat, struct cow_view *view,
                                 uint32_t edges) {
  if (view == NULL || view->closed || seat->op_in_progress)
    return;

  seat->op.type = COW_POINTER_OP_RESIZE;
  seat->op.view = view;
  seat->op.start_x = view->x;
  seat->op.start_y = view->y;
  seat->op.start_width = view->content_width;
  seat->op.start_height = view->content_height;
  seat->op.resize_edges = edges;
  seat->op_in_progress = true;
  seat->op_released = false;
  seat->op_dx = 0;
  seat->op_dy = 0;

  river_window_v1_inform_resize_start(view->river_window);
  river_seat_v1_op_start_pointer(seat->river_seat);
}

void cow_pointer_op_process(struct cow_seat *seat) {
  if (!seat->op_in_progress)
    return;

  struct cow_view *view = seat->op.view;
  if (view == NULL || view->closed) {
    /* View was destroyed during the op, end it */
    river_seat_v1_op_end(seat->river_seat);
    seat->op_in_progress = false;
    seat->op.type = COW_POINTER_OP_NONE;
    return;
  }

  int dx = seat->op_dx;
  int dy = seat->op_dy;

  switch (seat->op.type) {
  case COW_POINTER_OP_MOVE: {
    int bw = view->dec.border_width;
    int tt = view->dec.title_thickness;

    int cx = seat->op.start_x + dx;
    int cy = seat->op.start_y + dy;
    int fl = cx - bw;
    int ft = cy - bw - tt;
    int fr = cx + view->content_width + bw;
    int fb = cy + view->content_height + bw;
    snap_edges(view, &fl, &ft, &fr, &fb, false);

    if (seat->container_drag) {
      /* Container drag (-C): keep source in place, move ghost */
      cow_wm.drag_ghost_x = seat->pointer_x;
      cow_wm.drag_ghost_y = seat->pointer_y;
    } else {
      /* Normal move */
      view->x = fl + bw;
      view->y = ft + bw + tt;
      cow_view_update_output(view);
      cow_view_sync_container_pos(view);
    }
    break;
  }

  case COW_POINTER_OP_RESIZE: {
    uint32_t edges = seat->op.resize_edges;
    int new_w = seat->op.start_width;
    int new_h = seat->op.start_height;
    int new_x = seat->op.start_x;
    int new_y = seat->op.start_y;

    if (edges & COW_EDGE_RIGHT)
      new_w += dx;
    if (edges & COW_EDGE_LEFT) {
      new_w -= dx;
      new_x += dx;
    }
    if (edges & COW_EDGE_BOTTOM)
      new_h += dy;
    if (edges & COW_EDGE_TOP) {
      new_h -= dy;
      new_y += dy;
    }

    /* Enforce minimum size */
    if (new_w < 50) {
      if (edges & COW_EDGE_LEFT)
        new_x -= (50 - new_w);
      new_w = 50;
    }
    if (new_h < 50) {
      if (edges & COW_EDGE_TOP)
        new_y -= (50 - new_h);
      new_h = 50;
    }

    /* Respect dimension hints */
    if (view->min_width > 0 && new_w < view->min_width)
      new_w = view->min_width;
    if (view->min_height > 0 && new_h < view->min_height)
      new_h = view->min_height;
    if (view->max_width > 0 && new_w > view->max_width)
      new_w = view->max_width;
    if (view->max_height > 0 && new_h > view->max_height)
      new_h = view->max_height;

    /* Snap frame edges during resize */
    {
      int rbw = view->dec.border_width;
      int rtt = view->dec.title_thickness;
      int fl = new_x - rbw;
      int ft = new_y - rbw - rtt;
      int fr = new_x + new_w + rbw;
      int fb = new_y + new_h + rbw;
      snap_edges(view, &fl, &ft, &fr, &fb, true);
      if (edges & COW_EDGE_LEFT) {
        new_x = fl + rbw;
        new_w = fr - fl - 2 * rbw;
      }
      if (edges & COW_EDGE_RIGHT)
        new_w = fr - fl - 2 * rbw;
      if (edges & COW_EDGE_TOP) {
        new_y = ft + rbw + rtt;
        new_h = fb - ft - rbw - rtt - rbw;
      }
      if (edges & COW_EDGE_BOTTOM)
        new_h = fb - ft - rbw - rtt - rbw;
      if (new_w < 50)
        new_w = 50;
      if (new_h < 50)
        new_h = 50;
    }

    view->x = new_x;
    view->y = new_y;
    view->proposed_width = new_w;
    view->proposed_height = new_h;
    river_window_v1_propose_dimensions(view->river_window, new_w, new_h);

    /*
     * Sync container position and dimensions so that
     * river_node_v1_set_position uses the updated values and lx/ly
     * hit-detection remains correct.  Use the already-computed new_x/y/w/h
     * directly because view->content_width/height haven't been updated
     * yet (they're updated when the dimensions event arrives).
     */
    cow_view_sync_container_geometry(view, new_x, new_y, new_w, new_h);
    break;
  }

  case COW_POINTER_OP_NONE:
    break;
  }

  /* End the op if the pointer was released */
  if (seat->op_released) {
    if (seat->op.type == COW_POINTER_OP_RESIZE) {
      river_window_v1_inform_resize_end(view->river_window);
    }

    /*
     * Container drag (-C) completion.
     *
     * Source already in a container: always detach, regardless of
     * where the pointer lands.  No target highlight needed.
     *
     * Source NOT in a container: group with whatever window is
     * under the pointer (if any).
     */
    if (seat->container_drag && seat->op.type == COW_POINTER_OP_MOVE &&
        view->container_member != NULL) {
      /* Uncontainerise: detach, restore pre-container size, place at pointer */
      if (cow_wm.drag_drop_target != NULL) {
        cow_wm.drag_drop_target->dec.dirty = true;
        cow_wm.drag_drop_target = NULL;
      }
      int bw = view->dec.border_width;
      int tt = view->dec.title_thickness;
      /* cow_container_remove_view restores pre-container dimensions */
      cow_container_remove_view(view);
      /* Use restored (proposed) size for centring on pointer */
      int fw = (view->proposed_width  > 0 ? view->proposed_width  : view->content_width)  + 2 * bw;
      int fh = (view->proposed_height > 0 ? view->proposed_height : view->content_height) + bw + tt + bw;
      view->x = seat->pointer_x - fw / 2 + bw;
      view->y = seat->pointer_y - fh / 2 + bw + tt;
      view->dec.dirty = true;
      cow_wm.needs_manage_dirty = true;
    } else if (seat->container_drag &&
               seat->op.type == COW_POINTER_OP_MOVE &&
               cow_wm.drag_drop_target != NULL &&
               !cow_wm.drag_drop_target->closed &&
               cow_wm.drag_drop_target != view) {
      /* Containerise: group source with target */
      struct cow_view *target = cow_wm.drag_drop_target;
      cow_wm.drag_drop_target = NULL;
      target->dec.dirty = true;
      if (target->container_member != NULL)
        cow_container_add(target->container_member->container, view);
      else
        cow_container_create(target, view);
    } else {
      /* Normal drop: clear any lingering indicator */
      if (cow_wm.drag_drop_target != NULL) {
        cow_wm.drag_drop_target->dec.dirty = true;
        cow_wm.drag_drop_target = NULL;
      }
    }
    seat->container_drag = false;
    cow_wm.drag_drop_source = NULL;
    cow_drag_ghost_destroy();

    /* After move/resize, update the view's output assignment
     * based on where the window ended up. */
    cow_view_update_output(view);

    /* Restore titlebar visuals if this was a title drag */
    if (seat->titlebar_press == COW_TITLEBAR_PRESS_TITLE &&
        seat->pressed_view != NULL && !seat->pressed_view->closed) {
      seat->pressed_view->title_pressed = false;
      seat->pressed_view->dec.dirty = true;
    }
    seat->titlebar_press = COW_TITLEBAR_PRESS_NONE;
    seat->pressed_view = NULL;

    river_seat_v1_op_end(seat->river_seat);
    seat->op_in_progress = false;
    seat->op_released = false;
    seat->op.type = COW_POINTER_OP_NONE;
  }
}
