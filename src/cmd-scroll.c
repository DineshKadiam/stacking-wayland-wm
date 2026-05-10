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
 *        scroll [-t output] [-R] -r
 *
 *   -t output    target output by name (default: current output)
 *   -w           wrap at desk edges (default: clamp)
 *   -p           horizontal/vertical are pixels (default: percent of page)
 *   -r           start an interactive drag-the-viewport pan; the
 *                viewport follows the pointer until the triggering
 *                mouse button is released.  This is the cow analogue
 *                of fvwm3's `Scroll reverse` -- bind it to a mouse
 *                button to "grab and drag" the desk.  Wrap (-w) and
 *                percent/pixel (-p) flags are ignored with -r since
 *                interactive drag works in raw pixels and never wraps.
 *   -R           paired with -r, makes the viewport follow the
 *                pointer in the SAME direction instead of opposite.
 *                Without -R, -r behaves like fvwm `reverse` (grab and
 *                drag).
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
 *   mouse 1 R CM scroll -r # Ctrl+Meta+Button1 grabs and drags the
 *                          # desk viewport
 */

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "cow/cmd.h"
#include "cow/compat.h"
#include "cow/config.h"
#include "cow/desk_ops.h"
#include "cow/output.h"
#include "cow/pointer_op.h"
#include "cow/seat.h"
#include "cow/wm.h"

static enum cmd_retval cmd_scroll_exec(struct cmd *, struct cmd_q *);

struct cmd_entry cmd_scroll_entry = {
	"scroll",
	"t:wprR",
	0,
	2,
	"[-t output] [-w] [-p] horizontal vertical | [-t output] [-R] -r",
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

	/*
	 * Interactive drag-the-viewport mode.  -r alone is the
	 * "reverse" form (grab-and-drag, fvwm's default behaviour);
	 * -r -R reverses the direction so the viewport tracks the
	 * pointer in the SAME direction.  Either way no positional
	 * args are expected.
	 */
	if (args_has(args, 'r')) {
		if (args->argc != 0) {
			cmdq_error(cmdq,
			    "scroll: -r takes no positional arguments");
			return (CMD_RETURN_ERROR);
		}
		struct cow_seat *seat = cow_seat_primary();
		if (seat == NULL) {
			cmdq_error(cmdq, "scroll: no seat");
			return (CMD_RETURN_ERROR);
		}
		if (seat->op_in_progress) {
			/* Another op is already running; ignore.  The
			 * mouse binding that triggered us already
			 * grabbed the pointer for the other op. */
			return (CMD_RETURN_NORMAL);
		}
		bool reverse = !args_has(args, 'R');
		cow_pointer_op_start_scroll(seat, output, reverse);
		return (CMD_RETURN_NORMAL);
	}

	/* Non-interactive path requires both positional args. */
	if (args->argc != 2) {
		cmdq_error(cmdq, "usage: %s", cmd_scroll_entry.usage);
		return (CMD_RETURN_ERROR);
	}

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
