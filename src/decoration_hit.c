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

#include "cow/pointer_op.h"
#include "cow/titlebar.h"
#include "cow/view.h"

struct dec_frame_hit {
	int bw;
	int cl;
	int frame_w;
	int frame_h;
	int fx;
	int fy;
};

int
cow_decoration_corner_length(const struct cow_view *view)
{
	int cl = (view->cfg.handles && view->cfg.corner_length > 0) ?
	    view->cfg.corner_length :
	    0;
	int bw = view->dec.border_width;
	int tt = view->dec.title_thickness;

	if (cl > 0 && tt > 0 && cl < bw + tt)
		cl = bw + tt;
	return (cl);
}

uint32_t
cow_decoration_edges_at(struct cow_view *view, int fx, int fy, int cw, int ch)
{
	struct dec_frame_hit hit = {
		.bw = view->dec.border_width,
		.cl = view->cfg.corner_length,
		.fx = fx,
		.fy = fy,
	};

	int content_x, content_y;
	cow_titlebar_frame_geometry(view, cw, ch, &hit.frame_w,
	    &hit.frame_h, &content_x, &content_y);

	if (hit.fx < 0 || hit.fx >= hit.frame_w ||
	    hit.fy < 0 || hit.fy >= hit.frame_h)
		return (0);
	if (hit.fx >= hit.bw && hit.fx < hit.frame_w - hit.bw &&
	    hit.fy >= hit.bw && hit.fy < hit.frame_h - hit.bw)
		return (0);
	if (cow_titlebar_hit_for(view, cw, ch, fx - content_x,
		fy - content_y))
		return (0);

	bool near_top	= hit.fy < hit.cl;
	bool near_bottom = hit.fy >= hit.frame_h - hit.cl;
	bool near_left	= hit.fx < hit.cl;
	bool near_right	= hit.fx >= hit.frame_w - hit.cl;

	if (near_top && near_left)
		return (COW_EDGE_TOP | COW_EDGE_LEFT);
	if (near_top && near_right)
		return (COW_EDGE_TOP | COW_EDGE_RIGHT);
	if (near_bottom && near_left)
		return (COW_EDGE_BOTTOM | COW_EDGE_LEFT);
	if (near_bottom && near_right)
		return (COW_EDGE_BOTTOM | COW_EDGE_RIGHT);
	if (hit.fy < hit.bw)
		return (COW_EDGE_TOP);
	if (hit.fy >= hit.frame_h - hit.bw)
		return (COW_EDGE_BOTTOM);
	if (hit.fx < hit.bw)
		return (COW_EDGE_LEFT);
	if (hit.fx >= hit.frame_w - hit.bw)
		return (COW_EDGE_RIGHT);
	return (0);
}
