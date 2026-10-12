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
#include "cow/frame.h"
#include "cow/container.h"
#include "cow/titlebar.h"
#include "cow/view.h"
#include "cow/wm.h"

void
cow_frame_extend(struct cow_box *box, uint32_t edge, bool resize, int distance)
{
	switch (edge) {
	case COW_EDGE_LEFT:
		box->x -= distance;
		if (resize)
			box->width += distance;
		break;
	case COW_EDGE_RIGHT:
		if (resize)
			box->width += distance;
		else
			box->x += distance;
		break;
	case COW_EDGE_TOP:
		box->y -= distance;
		if (resize)
			box->height += distance;
		break;
	case COW_EDGE_BOTTOM:
		if (resize)
			box->height += distance;
		else
			box->y += distance;
		break;
	}
}

static bool
intersects(const struct cow_box *a, const struct cow_box *b)
{
	return a->x < b->x + b->width && a->x + a->width > b->x &&
	    a->y < b->y + b->height && a->y + a->height > b->y;
}

/* Test the whole path, not just its destination: an offset titlebar can
 * cross a neighbour and end on its far side during expansion.
 */
static bool
frame_path_clear(const struct cow_view *view, const struct cow_box *frame,
    const struct cow_box start[2], int count, const struct cow_box *obstacle,
    uint32_t edge, int distance)
{
	struct cow_box end = *frame, rects[2];
	cow_frame_extend(&end, edge, true, distance);
	cow_titlebar_frame_rects(view, &end, rects);
	for (int i = 0; i < count; i++) {
		/* Expansion does not attempt to resolve existing overlaps. */
		if (intersects(&start[i], obstacle))
			continue;
		struct cow_box swept;
		cow_box_union(&start[i], &rects[i], &swept);
		if (intersects(&swept, obstacle))
			return false;
	}
	return true;
}

/* Only border segments overlapping the movement path are targets. */
static int
frame_snap_distance(const struct cow_box *frame, const struct cow_box *target,
    uint32_t edge, int distance)
{
	bool horizontal = edge == COW_EDGE_LEFT || edge == COW_EDGE_RIGHT;
	if (horizontal ? frame->y >= target->y + target->height ||
		frame->y + frame->height <= target->y :
		frame->x >= target->x + target->width ||
		frame->x + frame->width <= target->x)
		return distance;
	int gap;
	switch (edge) {
	case COW_EDGE_LEFT:
		gap = frame->x - target->x - target->width;
		break;
	case COW_EDGE_RIGHT:
		gap = target->x - frame->x - frame->width;
		break;
	case COW_EDGE_TOP:
		gap = frame->y - target->y - target->height;
		break;
	default:
		gap = target->y - frame->y - frame->height;
		break;
	}
	return gap > 0 && gap < distance ? gap : distance;
}

int
cow_frame_travel(const struct cow_view *view, const struct cow_box *frame,
    uint32_t edge, bool resize, int distance)
{
	if (distance <= 0)
		return 0;
	struct cow_box start[2];
	int count = cow_titlebar_frame_rects(view, frame, start);
	struct cow_container *group = cow_container_top_group(view);
	struct cow_view	     *other;
	wl_list_for_each(other, &cow_wm.views, wm_link)
	{
		if (other == view || other->closed || !other->has_dimensions ||
		    other->output != view->output ||
		    !cow_view_visible(other, view->output) ||
		    (!resize && other->snap_skip) ||
		    (group != NULL && cow_container_top_group(other) == group))
			continue;
		struct cow_box other_frame = cow_view_frame_box(other),
			       obstacles[2];
		int n = cow_titlebar_frame_rects(other, &other_frame,
		    obstacles);
		if (!resize) {
			/* A repeated move must pass a neighbour already reached,
			 * rather than stop at another part of that same window.
			 */
			bool reached = false;
			for (int j = 0; j < count; j++) {
				struct cow_box next = start[j];
				cow_frame_extend(&next, edge, false, 1);
				for (int i = 0; i < n; i++)
					reached |= intersects(&next, &obstacles[i]);
			}
			if (reached)
				continue;
			for (int j = 0; j < count; j++)
				for (int i = 0; i < n; i++)
					distance = frame_snap_distance(&start[j],
					    &obstacles[i], edge, distance);
			/* Project only the exposed body edge beside a squeezed
			 * title. This lets a window align with the gap before
			 * sliding into it, without projecting the body's sides.
			 */
			if (n == 2) {
				struct cow_box body = obstacles[0];
				bool vertical = cow_titlebar_is_vertical(
				    other->cfg.titlebar.position);
				bool exposed = vertical ?
				    obstacles[1].height < other_frame.height :
				    obstacles[1].width < other_frame.width;
				bool facing = vertical ?
				    ((edge == COW_EDGE_RIGHT && body.x > other_frame.x) ||
					(edge == COW_EDGE_LEFT && obstacles[1].x > body.x)) :
				    ((edge == COW_EDGE_BOTTOM && body.y > other_frame.y) ||
					(edge == COW_EDGE_TOP && obstacles[1].y > body.y));
				if (exposed && facing) {
					if (vertical) {
						body.y = frame->y;
						body.height = frame->height;
					} else {
						body.x = frame->x;
						body.width = frame->width;
					}
					distance = frame_snap_distance(frame, &body,
					    edge, distance);
				}
			}
			continue;
		}
		for (int i = 0; i < n; i++) {
			if (frame_path_clear(view, frame, start, count,
				&obstacles[i], edge, distance))
				continue;
			/* The swept area grows monotonically with travel. */
			int low = 0, high = distance;
			while (high - low > 1) {
				int mid = low + (high - low) / 2;
				if (frame_path_clear(view, frame, start, count,
					&obstacles[i], edge, mid))
					low = mid;
				else
					high = mid;
			}
			distance = low;
		}
	}
	return distance;
}
