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
/*
 * Command: scroll -- pan the virtual desk's viewport.
 *
 * usage: scroll [-t output] [-w] [-p] horizontal vertical
 *
 *   -t output    target output by name (default: current output)
 *   -w           wrap at desk edges (default: clamp)
 *   -p           horizontal/vertical are pixels (default: percent of page)
 *
 * Modeled on fvwm3's `Scroll` command.  Unlike `page`, which jumps in
 * whole-page units, `scroll` shifts the viewport by an arbitrary
 * offset, allowing a window to straddle two pages.  Negative values
 * scroll left/up.
 *
 * Examples:
 *   scroll 100 0           # one full page right
 *   scroll 50 25           # half page right, quarter page down
 *   scroll -100 0          # one full page left
 *   scroll -p 200 0        # 200 pixels right
 *   scroll -w 100000 0     # one page right, wrapping at desk edge
 *                          # (the fvwm 'multiply by 1000' wrap idiom);
 *                          # equivalent to: scroll -w 100 0
 */

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "cow/cmd.h"
#include "cow/compat.h"
#include "cow/config.h"
#include "cow/desk_ops.h"
#include "cow/output.h"
#include "cow/wm.h"

static enum cmd_retval cmd_scroll_exec(struct cmd *, struct cmd_q *);

struct cmd_entry cmd_scroll_entry = {
	"scroll",
	"t:wp",
	2,
	2,
	"[-t output] [-w] [-p] horizontal vertical",
	cmd_scroll_exec,
};

static enum cmd_retval
cmd_scroll_exec(struct cmd *self, struct cmd_q *cmdq)
{
	struct args	  *args = self->args;
	struct cow_output *output;

	/* Resolve target output (-t name or current). */
	const char *out_name = args_get(args, 't');
	if (out_name != NULL) {
		output = NULL;
		struct cow_output *o;
		wl_list_for_each(o, &cow_wm.outputs, wm_link)
		{
			if (o->name != NULL && strcmp(o->name, out_name) == 0) {
				output = o;
				break;
			}
		}
		if (output == NULL) {
			cmdq_error(cmdq, "scroll: unknown output: %s",
			    out_name);
			return (CMD_RETURN_ERROR);
		}
	} else {
		output = cow_wm.current_output;
	}
	if (output == NULL) {
		cmdq_error(cmdq, "scroll: no output");
		return (CMD_RETURN_ERROR);
	}

	if (cow_config == NULL ||
	    (cow_config->page_cols <= 1 && cow_config->page_rows <= 1)) {
		/* 1x1 grid -- nothing to scroll. */
		return (CMD_RETURN_NORMAL);
	}

	int w = output->usable_area.width;
	int h = output->usable_area.height;
	if (w <= 0 || h <= 0)
		return (CMD_RETURN_NORMAL);

	bool pixels = args_has(args, 'p');
	bool wrap   = args_has(args, 'w');

	long long hh, vv;
	/*
	 * Accept the full int range here: the actual bounds (clamp/wrap)
	 * are applied by cow_page_scroll_to once we have output dims.
	 * INT_MIN / INT_MAX are wide enough for any sensible scroll arg
	 * including the fvwm legacy `100000`-style multiply-by-1000 wrap
	 * idiom.
	 */
	if (!cmd_parse_int(cmdq, "scroll", "horizontal", args->argv[0], INT_MIN,
		INT_MAX, &hh))
		return (CMD_RETURN_ERROR);
	if (!cmd_parse_int(cmdq, "scroll", "vertical", args->argv[1], INT_MIN,
		INT_MAX, &vv))
		return (CMD_RETURN_ERROR);

	/*
	 * Compute the pixel deltas.  In percent mode, 100 == one full
	 * page in that axis (i.e. output width / height).  Trickle the
	 * percent through as integer arithmetic to avoid pulling in
	 * floats: dx_px = h * w / 100.  At fvwm-3-compat magnitudes
	 * (multiply-by-1000 to wrap) the input is always a multiple of
	 * 100, so integer division is exact in the cases users hit.
	 */
	long long dx_px, dy_px;
	if (pixels) {
		dx_px = hh;
		dy_px = vv;
	} else {
		/*
		 * Legacy fvwm-compat: when -w is NOT given, a percent
		 * value with magnitude >= 1000 implicitly enables wrap
		 * (and the actual scroll amount is value mod 1000
		 * percent).  fvwm uses this multiply-by-1000 idiom
		 * because its `Scroll` command has no flag form; cow
		 * users are encouraged to use `-w` plus a percent in
		 * [-100, 100] instead.  Skip the implicit form when
		 * -w is already explicit -- otherwise `scroll -w 100000`
		 * would mod down to 0 and do nothing.
		 */
		if (!wrap &&
		    (hh >= 1000 || hh <= -1000 || vv >= 1000 || vv <= -1000)) {
			wrap = true;
			hh %= 1000;
			vv %= 1000;
		}
		dx_px = hh * w / 100;
		dy_px = vv * h / 100;
	}

	struct cow_desk *desk	= output->current_desk;
	long long	 new_vx = (long long)desk->page_vx + dx_px;
	long long	 new_vy = (long long)desk->page_vy + dy_px;

	/* Saturate to int range before the call; cow_page_scroll_to
	 * then clamps or wraps into the valid desk extent. */
	if (new_vx > INT_MAX)
		new_vx = INT_MAX;
	if (new_vx < INT_MIN)
		new_vx = INT_MIN;
	if (new_vy > INT_MAX)
		new_vy = INT_MAX;
	if (new_vy < INT_MIN)
		new_vy = INT_MIN;

	cow_page_scroll_to(output, (int)new_vx, (int)new_vy, wrap);
	return (CMD_RETURN_NORMAL);
}
