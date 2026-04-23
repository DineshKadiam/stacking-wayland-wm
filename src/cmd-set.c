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
 * Command: set -- update configuration fields at runtime.
 *
 * usage: set [-v] key value
 */

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cow/compat.h"
#include "cow/cmd.h"
#include "cow/colour.h"
#include "cow/config.h"
#include "cow/desk_ops.h"
#include "cow/font.h"
#include "cow/ipc.h"
#include "cow/memory.h"
#include "cow/output.h"
#include "cow/pan_frames.h"
#include "cow/shm.h"
#include "cow/status.h"
#include "cow/titlebar.h"
#include "cow/view.h"
#include "cow/wm.h"

static enum cmd_retval cmd_set_exec(struct cmd *, struct cmd_q *);
static void	       do_set_int(const char *, const char *, struct cmd_q *);
static void do_set_colour(const char *, const char *, struct cmd_q *);
static void do_set_border_style(const char *);
static void do_set_desktop_config(const char *, struct cmd_q *);
static void do_set_focus(const char *);
static void do_set_placement(const char *, struct cmd_q *);
static void do_set_font(const char *);
static void do_set_titlebar(const char *, const char *);
static void do_set_button(const char *, const char *);
static void refresh_visuals(void);
static bool parse_colour_value(const char *, float[4]);

struct cmd_entry cmd_set_entry = {
	"set",
	"v",
	2,
	-1,
	"set [-v] key value",
	cmd_set_exec,
};

static enum cmd_retval
cmd_set_exec(struct cmd *self, struct cmd_q *cmdq)
{
	struct args *args = self->args;

	if (args->argc < 2) {
		cmdq_error(cmdq, "usage: %s", self->entry->usage);
		return (CMD_RETURN_ERROR);
	}

	const char *key = args->argv[0];
	char	   *val = cmd_argv_to_string(args->argc, args->argv, 1);

	/* Integer config fields */
	if (strcmp(key, "border") == 0 || strcmp(key, "step") == 0 ||
	    strcmp(key, "corner_length") == 0 ||
	    strcmp(key, "reserved.top") == 0 ||
	    strcmp(key, "reserved.bottom") == 0 ||
	    strcmp(key, "reserved.left") == 0 ||
	    strcmp(key, "reserved.right") == 0 ||
	    strcmp(key, "icon.size") == 0 ||
	    strcmp(key, "snap_distance") == 0) {
		do_set_int(key, val, cmdq);
		xfree(val);
		return (CMD_RETURN_NORMAL);
	}

	/* Colour fields (per-edge, uniform, per-output background) */
	if (strncmp(key, "colour.", 7) == 0 ||
	    strncmp(key, "output.colour.", 14) == 0 ||
	    strcmp(key, "titlebar.active_colour") == 0 ||
	    strcmp(key, "titlebar.inactive_colour") == 0 ||
	    strcmp(key, "titlebar.fg_active") == 0 ||
	    strcmp(key, "titlebar.fg_inactive") == 0 ||
	    strncmp(key, "container.tab.", 14) == 0) {
		do_set_colour(key, val, cmdq);
		xfree(val);
		return (CMD_RETURN_NORMAL);
	}

	if (strcmp(key, "border_style") == 0) {
		do_set_border_style(val);
		xfree(val);
		return (CMD_RETURN_NORMAL);
	}

	if (strcmp(key, "desktop-configuration") == 0) {
		do_set_desktop_config(val, cmdq);
		xfree(val);
		return (CMD_RETURN_NORMAL);
	}

	if (strcmp(key, "desktop_size") == 0) {
		/* desktop_size N M: cols in argv[1], rows in argv[2]. */
		if (args->argc < 3) {
			cmdq_error(cmdq,
			    "set: desktop_size: expected N M (e.g. 3 2)");
			xfree(val);
			return (CMD_RETURN_ERROR);
		}
		const char *errstr;
		int	    cols = (int)strtonum(args->argv[1], 1, 99, &errstr);
		if (errstr != NULL) {
			cmdq_error(cmdq,
			    "set: desktop_size: bad column count: %s",
			    args->argv[1]);
			xfree(val);
			return (CMD_RETURN_ERROR);
		}
		int rows = (int)strtonum(args->argv[2], 1, 99, &errstr);
		if (errstr != NULL) {
			cmdq_error(cmdq, "set: desktop_size: bad row count: %s",
			    args->argv[2]);
			xfree(val);
			return (CMD_RETURN_ERROR);
		}
		if (cow_config != NULL) {
			cow_config->page_cols = cols;
			cow_config->page_rows = rows;
			/* Clamp windows/viewports outside the new grid at
			 * runtime. Skip during startup: outputs have not
			 * connected yet and cow_wm.outputs is empty, making the
			 * list iteration unsafe. */
			if (!cow_ipc.startup_mode) {
				struct cow_output *out;
				wl_list_for_each(out, &cow_wm.outputs, wm_link)
				    cow_page_clamp_views(out);
				cow_wm.needs_manage_dirty = true;
				pan_frames_update_all();
			}
		}
		xfree(val);
		return (CMD_RETURN_NORMAL);
	}

	if (strcmp(key, "focus") == 0) {
		do_set_focus(val);
		xfree(val);
		return (CMD_RETURN_NORMAL);
	}

	if (strcmp(key, "placement") == 0) {
		do_set_placement(val, cmdq);
		xfree(val);
		return (CMD_RETURN_NORMAL);
	}

	if (strcmp(key, "font") == 0) {
		do_set_font(val);
		xfree(val);
		return (CMD_RETURN_NORMAL);
	}
	if (strcmp(key, "titlebar.enabled") == 0 ||
	    strcmp(key, "handles") == 0) {
		do_set_titlebar(key, val);
		xfree(val);
		return (CMD_RETURN_NORMAL);
	}

	if (strcmp(key, "titlebar.height") == 0) {
		if (cow_config != NULL) {
			const char *errstr;
			long long   v = strtonum(val, 0, INT_MAX, &errstr);
			if (errstr != NULL) {
				cmdq_error(cmdq,
				    "set: titlebar.height: bad value: %s", val);
				xfree(val);
				return (CMD_RETURN_ERROR);
			}
			cow_config->titlebar.height = (int)v;
			refresh_visuals();
		}
		xfree(val);
		return (CMD_RETURN_NORMAL);
	}

	if (strcmp(key, "container.tab.sunken") == 0) {
		if (cow_config != NULL)
			cow_config->container.tab_sunken_inactive =
			    (strcmp(val, "true") == 0 || strcmp(val, "1") == 0);
		xfree(val);
		refresh_visuals();
		return (CMD_RETURN_NORMAL);
	}

	if (strcmp(key, "edge_scroll") == 0) {
		if (cow_config != NULL) {
			cow_config->edge_scroll = (strcmp(val, "true") == 0 ||
			    strcmp(val, "1") == 0);
			if (!cow_ipc.startup_mode)
				pan_frames_update_all();
		}
		xfree(val);
		return (CMD_RETURN_NORMAL);
	}

	if (strcmp(key, "edge_scroll_delay_ms") == 0) {
		const char *errstr;
		long long   v = strtonum(val, 0, 10000, &errstr);
		if (errstr != NULL) {
			cmdq_error(cmdq,
			    "set: edge_scroll_delay_ms: bad value: %s", val);
			xfree(val);
			return (CMD_RETURN_ERROR);
		}
		if (cow_config != NULL)
			cow_config->edge_scroll_delay_ms = (int)v;
		xfree(val);
		return (CMD_RETURN_NORMAL);
	}

	if (strcmp(key, "page_scroll_animate") == 0) {
		if (cow_config != NULL)
			cow_config->page_scroll_animate = (strcmp(val,
							       "true") == 0 ||
			    strcmp(val, "1") == 0);
		xfree(val);
		return (CMD_RETURN_NORMAL);
	}

	if (strcmp(key, "page_scroll_ms") == 0) {
		const char *errstr;
		long long   v = strtonum(val, 0, 5000, &errstr);
		if (errstr != NULL) {
			cmdq_error(cmdq, "set: page_scroll_ms: bad value: %s",
			    val);
			xfree(val);
			return (CMD_RETURN_ERROR);
		}
		if (cow_config != NULL)
			cow_config->page_scroll_ms = (int)v;
		xfree(val);
		return (CMD_RETURN_NORMAL);
	}

	if (strncmp(key, "titlebar.button_", 16) == 0) {
		do_set_button(key, val);
		xfree(val);
		return (CMD_RETURN_NORMAL);
	}

	cmdq_error(cmdq, "set: unknown key: %s", key);
	xfree(val);
	return (CMD_RETURN_ERROR);
}

static void
do_set_int(const char *key, const char *val, struct cmd_q *cmdq)
{
	struct {
		const char *name;
		int	   *ptr;
	} fields[] = {
		{ "border",	    &cow_config->border		},
		{ "step",		  &cow_config->step	    },
		{ "corner_length",   &cow_config->corner_length   },
		{ "reserved.top",	  &cow_config->reserved.top    },
		{ "reserved.bottom", &cow_config->reserved.bottom },
		{ "reserved.left",   &cow_config->reserved.left   },
		{ "reserved.right",  &cow_config->reserved.right	},
		{ "icon.size",       &cow_config->icon_size	      },
		{ "snap_distance",   &cow_config->snap_distance   },
		{ NULL,		NULL			     },
	};

	for (int i = 0; fields[i].name != NULL; i++) {
		if (strcmp(key, fields[i].name) != 0)
			continue;
		const char *errstr;
		long long   v = strtonum(val, 0, INT_MAX, &errstr);
		if (errstr != NULL) {
			cmdq_error(cmdq, "set: %s: bad value: %s", key, val);
			return;
		}
		*fields[i].ptr = (int)v;
		if (strcmp(key, "corner_length") == 0 && *fields[i].ptr == 0)
			cow_config->handles = false;
		refresh_visuals();
		return;
	}
}

static void
do_set_colour(const char *key, const char *val, struct cmd_q *cmdq)
{
	/* Per-output background: output.colour.background.<name> */
	if (strncmp(key, "output.colour.background.", 25) == 0) {
		const char *oname = key + 25;
		float	    c[4];
		if (!parse_colour_value(val, c)) {
			cmdq_error(cmdq, "set: invalid colour value");
			return;
		}
		struct cow_output *out;
		wl_list_for_each(out, &cow_wm.outputs, wm_link)
		{
			if (out->name != NULL &&
			    strcmp(out->name, oname) == 0) {
				memcpy(out->bg_colour, c, sizeof(float) * 4);
				out->has_bg_colour = true;
				out->bg_dirty	   = true;
				refresh_visuals();
				return;
			}
		}
		cmdq_error(cmdq, "set: unknown output: %s", oname);
		return;
	}

	/* Uniform colour shortcuts */
	if (strcmp(key, "colour.active") == 0 ||
	    strcmp(key, "colour.inactive") == 0) {
		float c[4];
		if (!parse_colour_value(val, c)) {
			cmdq_error(cmdq, "set: invalid colour value");
			return;
		}
		struct cow_border_colour *bc = (strcmp(key, "colour.active") ==
						   0) ?
		    &cow_config->border_active :
		    &cow_config->border_inactive;
		memcpy(bc->n, c, sizeof(float) * 4);
		memcpy(bc->s, c, sizeof(float) * 4);
		memcpy(bc->e, c, sizeof(float) * 4);
		memcpy(bc->w, c, sizeof(float) * 4);
		refresh_visuals();
		return;
	}

	/* Titlebar colors */
	struct {
		const char *name;
		float	   *ptr;
	} tb_fields[] = {
		{ "titlebar.active_colour",
		    cow_config->titlebar.active_colour			       },
		{ "titlebar.inactive_colour",
		    cow_config->titlebar.inactive_colour				 },
		{ "titlebar.fg_active",	cow_config->titlebar.fg_active   },
		{ "titlebar.fg_inactive",	  cow_config->titlebar.fg_inactive },
		{ NULL,			NULL			     },
	};
	for (int i = 0; tb_fields[i].name != NULL; i++) {
		if (strcmp(key, tb_fields[i].name) == 0) {
			if (!parse_colour_value(val, tb_fields[i].ptr)) {
				cmdq_error(cmdq, "set: invalid colour value");
				return;
			}
			refresh_visuals();
			return;
		}
	}

	/* Container tab colors */
	struct {
		const char *name;
		float	   *ptr;
		bool	   *has;
	} ct_fields[] = {
		{ "container.tab.active_colour",
		    cow_config->container.tab_active,
		    &cow_config->container.has_tab_active	  },
		{ "container.tab.inactive_colour",
		    cow_config->container.tab_inactive,
		    &cow_config->container.has_tab_inactive	    },
		{ "container.tab.fg_active",
		    cow_config->container.tab_fg_active,
		    &cow_config->container.has_tab_fg_active     },
		{ "container.tab.fg_inactive",
		    cow_config->container.tab_fg_inactive,
		    &cow_config->container.has_tab_fg_inactive   },
		{ NULL,			    NULL, NULL },
	};
	for (int i = 0; ct_fields[i].name != NULL; i++) {
		if (strcmp(key, ct_fields[i].name) == 0) {
			if (!parse_colour_value(val, ct_fields[i].ptr)) {
				cmdq_error(cmdq, "set: invalid colour value");
				return;
			}
			*ct_fields[i].has = true;
			refresh_visuals();
			return;
		}
	}

	/* Per-edge border colors */
	struct {
		const char *name;
		float	   *ptr;
	} colour_fields[] = {
		{ "colour.active.n",	     cow_config->border_active.n	 },
		{ "colour.active.s",	     cow_config->border_active.s	 },
		{ "colour.active.e",	     cow_config->border_active.e	 },
		{ "colour.active.w",	     cow_config->border_active.w	 },
		{ "colour.active.nw",	      cow_config->border_active.nw   },
		{ "colour.active.ne",	      cow_config->border_active.ne   },
		{ "colour.active.sw",	      cow_config->border_active.sw   },
		{ "colour.active.se",	      cow_config->border_active.se   },
		{ "colour.inactive.n",	       cow_config->border_inactive.n  },
		{ "colour.inactive.s",	       cow_config->border_inactive.s  },
		{ "colour.inactive.e",	       cow_config->border_inactive.e  },
		{ "colour.inactive.w",	       cow_config->border_inactive.w  },
		{ "colour.inactive.nw",	cow_config->border_inactive.nw },
		{ "colour.inactive.ne",	cow_config->border_inactive.ne },
		{ "colour.inactive.sw",	cow_config->border_inactive.sw },
		{ "colour.inactive.se",	cow_config->border_inactive.se },
		{ "output.colour.background", cow_config->output_bg_colour   },
		{ NULL,			NULL			     },
	};

	for (int i = 0; colour_fields[i].name != NULL; i++) {
		if (strcmp(key, colour_fields[i].name) != 0)
			continue;
		if (!parse_colour_value(val, colour_fields[i].ptr)) {
			cmdq_error(cmdq, "set: invalid colour value");
			return;
		}
		if (strcmp(key, "output.colour.background") == 0)
			cow_config->has_output_bg_colour = true;
		if (strcmp(key, "colour.active.nw") == 0)
			cow_config->border_active.has_nw = true;
		else if (strcmp(key, "colour.active.ne") == 0)
			cow_config->border_active.has_ne = true;
		else if (strcmp(key, "colour.active.sw") == 0)
			cow_config->border_active.has_sw = true;
		else if (strcmp(key, "colour.active.se") == 0)
			cow_config->border_active.has_se = true;
		else if (strcmp(key, "colour.inactive.nw") == 0)
			cow_config->border_inactive.has_nw = true;
		else if (strcmp(key, "colour.inactive.ne") == 0)
			cow_config->border_inactive.has_ne = true;
		else if (strcmp(key, "colour.inactive.sw") == 0)
			cow_config->border_inactive.has_sw = true;
		else if (strcmp(key, "colour.inactive.se") == 0)
			cow_config->border_inactive.has_se = true;
		if (cow_config->border_active.has_nw ||
		    cow_config->border_active.has_ne ||
		    cow_config->border_active.has_sw ||
		    cow_config->border_active.has_se)
			cow_config->border_active.has_corners = true;
		if (cow_config->border_inactive.has_nw ||
		    cow_config->border_inactive.has_ne ||
		    cow_config->border_inactive.has_sw ||
		    cow_config->border_inactive.has_se)
			cow_config->border_inactive.has_corners = true;
		refresh_visuals();
		return;
	}
}

static void
do_set_border_style(const char *val)
{
	if (strcmp(val, "fvwm") == 0)
		cow_config->border_style = COW_BORDER_STYLE_FVWM;
	else if (strcmp(val, "mwm") == 0)
		cow_config->border_style = COW_BORDER_STYLE_MWM;
	else
		cow_config->border_style = COW_BORDER_STYLE_NONE;
	refresh_visuals();
}

static void
do_set_desktop_config(const char *val, struct cmd_q *cmdq)
{
	if (strcmp(val, "global") == 0) {
		cow_config->desktop_config = COW_DESKTOP_GLOBAL;

		if (cow_ipc.startup_mode)
			return;

		struct cow_output *cur = cow_wm.current_output;
		if (cur != NULL) {
			uint8_t		   desk = cur->current_desk->nr;
			struct cow_output *o;
			wl_list_for_each(o, &cow_wm.outputs, wm_link)
			{
				if (o != cur)
					cow_desk_switch(o, desk);
			}
		}
	} else if (strcmp(val, "per-output") == 0) {
		cow_config->desktop_config = COW_DESKTOP_PER_OUTPUT;
	} else if (strcmp(val, "shared") == 0) {
		/* Resolve desk conflicts before enabling shared mode. */
		cow_config->desktop_config = COW_DESKTOP_PER_OUTPUT;

		struct cow_output *o;
		wl_list_for_each(o, &cow_wm.outputs, wm_link)
		{
			struct cow_output *other;
			wl_list_for_each(other, &cow_wm.outputs, wm_link)
			{
				if (other == o)
					continue;
				if (other->current_desk->nr !=
				    o->current_desk->nr)
					continue;
				for (int d = 0; d < COW_NR_OF_DESKS; d++) {
					bool		   used = false;
					struct cow_output *chk;
					wl_list_for_each(chk, &cow_wm.outputs,
					    wm_link)
					{
						if (chk == other)
							continue;
						if (chk->current_desk->nr ==
						    d) {
							used = true;
							break;
						}
					}
					if (!used) {
						cow_desk_switch(other, d);
						break;
					}
				}
			}
		}
		cow_config->desktop_config = COW_DESKTOP_SHARED;
	} else {
		cmdq_error(cmdq,
		    "set: desktop-configuration: expected "
		    "global|per-output|shared");
		return;
	}
	cow_wm.needs_manage_dirty = true;
	cow_status_emit();
}

static void
do_set_focus(const char *val)
{
	if (strcmp(val, "sloppy") == 0)
		cow_config->focus_mode = COW_FOCUS_MODE_SLOPPY;
	else
		cow_config->focus_mode = COW_FOCUS_MODE_CLICK;
}

static void
do_set_placement(const char *val, struct cmd_q *cmdq)
{
	if (strcmp(val, "cascade") == 0)
		cow_config->placement = COW_PLACEMENT_CASCADE;
	else if (strcmp(val, "undermouse") == 0)
		cow_config->placement = COW_PLACEMENT_UNDERMOUSE;
	else if (strcmp(val, "centre") == 0 || strcmp(val, "center") == 0)
		cow_config->placement = COW_PLACEMENT_CENTRE;
	else
		cmdq_error(cmdq, "set: placement: unknown mode: %s", val);
}

static void
do_set_font(const char *val)
{
	xfree(cow_config->font_name);
	cow_config->font_name = xstrdup(val);
	refresh_visuals();
}

static void
do_set_titlebar(const char *key, const char *val)
{
	if (strcmp(key, "titlebar.enabled") == 0) {
		cow_config->titlebar.enabled = (strcmp(val, "true") == 0 ||
		    strcmp(val, "1") == 0);
	} else if (strcmp(key, "handles") == 0) {
		cow_config->handles = (strcmp(val, "true") == 0 ||
		    strcmp(val, "1") == 0);
	}
	refresh_visuals();
}

static void
do_set_button(const char *key, const char *val)
{
	struct {
		const char *vector_key;
		const char *action_key;
		int	    idx;
	} btn_fields[] = {
		{ "titlebar.button_left.vector",	 "titlebar.button_left.action",
		    0								   },
		{ "titlebar.button_right.vector",
		    "titlebar.button_right.action",				    1  },
		{ "titlebar.button_right2.vector",
		    "titlebar.button_right2.action",				     2  },
		{ NULL,			    NULL,			      -1 },
	};

	for (int i = 0; btn_fields[i].vector_key != NULL; i++) {
		if (strcmp(key, btn_fields[i].vector_key) == 0) {
			cow_parse_vector_button_string(
			    &cow_config->titlebar.buttons[btn_fields[i].idx]
				.vector,
			    val);
			refresh_visuals();
			return;
		}
		if (strcmp(key, btn_fields[i].action_key) == 0) {
			if (strcmp(val, "close") == 0)
				cow_config->titlebar.buttons[btn_fields[i].idx]
				    .action = COW_BUTTON_ACTION_CLOSE;
			else if (strcmp(val, "maximize") == 0)
				cow_config->titlebar.buttons[btn_fields[i].idx]
				    .action = COW_BUTTON_ACTION_MAXIMIZE;
			else if (strcmp(val, "iconify") == 0)
				cow_config->titlebar.buttons[btn_fields[i].idx]
				    .action = COW_BUTTON_ACTION_ICONIFY;
			else
				cow_config->titlebar.buttons[btn_fields[i].idx]
				    .action = COW_BUTTON_ACTION_NONE;
			return;
		}
	}
}

static bool
parse_colour_value(const char *str, float out[4])
{
	char	     *end;
	unsigned long v = strtoul(str, &end, 0);
	if (end == str)
		return false;
	cow_colour_convert(out, (uint32_t)v);
	return true;
}

static void
refresh_visuals(void)
{
	if (cow_ipc.startup_mode)
		return;

	int new_tt = 0;
	if (cow_config->titlebar.enabled) {
		int h = cow_config->titlebar.height;
		if (h > 0) {
			new_tt = h;
		} else {
			struct cow_font tb_font;
			const char     *fname = cow_config->font_name ?
				cow_config->font_name :
				"monospace 10";
			cow_font_init(&tb_font, fname);
			new_tt = cow_titlebar_compute_thickness(&tb_font);
		}
	}

	struct cow_view *view;
	wl_list_for_each(view, &cow_wm.views, wm_link)
	{
		if (!view->closed) {
			view->dec.dirty		      = true;
			view->dec.border_width	      = cow_config->border;
			view->dec.title_thickness     = new_tt;
			view->dec.last_content_width  = -1;
			view->dec.last_content_height = -1;
			if (view->dec.buffer != NULL) {
				cow_shm_buffer_destroy(view->dec.buffer);
				view->dec.buffer = NULL;
			}
		}
	}

	struct cow_output *output;
	wl_list_for_each(output, &cow_wm.outputs, wm_link)
	{
		output->usable_area.x	   = output->x;
		output->usable_area.y	   = output->y;
		output->usable_area.width  = output->width;
		output->usable_area.height = output->height;
		cow_output_apply_reserved_area(output);
		output->bg_dirty = true;
	}

	cow_wm_manage_dirty();
}
