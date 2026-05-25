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
#include <stddef.h>
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
#include "cow/stack.h"
#include "cow/status.h"
#include "cow/titlebar.h"
#include "cow/view.h"
#include "cow/wm.h"

static enum cmd_retval cmd_set_exec(struct cmd *, struct cmd_q *);
static void	       do_set_int(const char *, const char *, struct cmd_q *);
static void do_set_colour(const char *, const char *, struct cmd_q *);
static void do_set_border_style(const char *, struct cmd_q *);
static void do_set_desktop_config(const char *, struct cmd_q *);
static void do_set_focus(const char *);
static void do_set_placement(const char *, struct cmd_q *);
static void do_set_font(const char *, struct cmd_q *);
static void do_set_titlebar(const char *, const char *, struct cmd_q *);
static void do_set_button(const char *, const char *, struct cmd_q *);
static void refresh_visuals(void);

/*
 * When a `set` command runs with cmdq->target.window resolved (either
 * via -t :::W from the user, or via the rule subsystem fanning out an
 * event to a view), per-view-able style writes are diverted from
 * cow_config into the view's cfg snapshot.  Returns NULL when no
 * target window is set; callers fall back to writing cow_config in
 * that case.
 *
 * After a global write to cow_config (target NULL), callers stamp
 * every existing view's snapshot via cow_view_cfg_apply_global so
 * the change is reflected on already-mapped windows too.
 */
static struct cow_view_cfg *
cfg_target(struct cmd_q *cmdq)
{
	if (cmdq == NULL || cmdq->target.window == NULL)
		return NULL;
	return &cmdq->target.window->cfg;
}

/*
 * After a global write to cow_config, walk every view and re-stamp
 * its cfg snapshot.  Without this, existing views would keep stale
 * snapshots and CLI `set border 5` would only take effect on
 * not-yet-created views.
 *
 * Skip during startup: cow_wm.views has not been wl_list_init'd yet
 * when cow_ipc_load_config runs the user's config (cow_wm_init runs
 * later in main()).  No stamping is needed in that case -- views
 * created after startup snapshot the freshly-populated cow_config
 * via cow_view_cfg_init_from_global at create time.
 */
static void
stamp_all_views_from_global(void)
{
	if (cow_ipc.startup_mode)
		return;
	struct cow_view *view;
	wl_list_for_each(view, &cow_wm.views, wm_link)
	{
		if (!view->closed)
			cow_view_cfg_apply_global(&view->cfg);
	}
}

struct cmd_entry cmd_set_entry = {
	"set",
	"v",
	2,
	-1,
	"set [-v] key value",
	cmd_set_exec,
	0,
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
		do_set_border_style(val, cmdq);
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
				cow_wm_request_manage_dirty();
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
		do_set_font(val, cmdq);
		xfree(val);
		return (CMD_RETURN_NORMAL);
	}
	if (strcmp(key, "titlebar.enabled") == 0 ||
	    strcmp(key, "handles") == 0) {
		do_set_titlebar(key, val, cmdq);
		xfree(val);
		return (CMD_RETURN_NORMAL);
	}

	if (strcmp(key, "titlebar.height") == 0) {
		const char *errstr;
		long long   v = strtonum(val, 0, INT_MAX, &errstr);
		if (errstr != NULL) {
			cmdq_error(cmdq, "set: titlebar.height: bad value: %s",
			    val);
			xfree(val);
			return (CMD_RETURN_ERROR);
		}
		struct cow_view_cfg *cfg = cfg_target(cmdq);
		if (cfg != NULL) {
			cfg->titlebar.height = (int)v;
		} else if (cow_config != NULL) {
			cow_config->titlebar.height = (int)v;
			stamp_all_views_from_global();
		}
		refresh_visuals();
		xfree(val);
		return (CMD_RETURN_NORMAL);
	}

	if (strcmp(key, "titlebar.squeeze") == 0) {
		enum cow_squeeze_mode m;
		if (!cow_parse_squeeze_mode(val, &m)) {
			cmdq_error(cmdq,
			    "set: titlebar.squeeze: bad value: %s "
			    "(expected none|natural|fixed)",
			    val);
			xfree(val);
			return (CMD_RETURN_ERROR);
		}
		struct cow_view_cfg *cfg = cfg_target(cmdq);
		if (cfg != NULL) {
			cfg->titlebar.squeeze.mode = m;
		} else if (cow_config != NULL) {
			cow_config->titlebar.squeeze.mode = m;
			stamp_all_views_from_global();
		}
		refresh_visuals();
		xfree(val);
		return (CMD_RETURN_NORMAL);
	}

	if (strcmp(key, "titlebar.squeeze.justify") == 0) {
		enum cow_squeeze_just j;
		if (!cow_parse_squeeze_just(val, &j)) {
			cmdq_error(cmdq,
			    "set: titlebar.squeeze.justify: bad value: %s "
			    "(expected left|centre|right)",
			    val);
			xfree(val);
			return (CMD_RETURN_ERROR);
		}
		struct cow_view_cfg *cfg = cfg_target(cmdq);
		if (cfg != NULL) {
			cfg->titlebar.squeeze.justify = j;
		} else if (cow_config != NULL) {
			cow_config->titlebar.squeeze.justify = j;
			stamp_all_views_from_global();
		}
		refresh_visuals();
		xfree(val);
		return (CMD_RETURN_NORMAL);
	}

	if (strcmp(key, "titlebar.squeeze.width") == 0 ||
	    strcmp(key, "titlebar.squeeze.offset") == 0 ||
	    strcmp(key, "titlebar.squeeze.min_width") == 0) {
		const char *errstr;
		/* offset is signed; width/min_width are non-negative. */
		long long lo = (strcmp(key, "titlebar.squeeze.offset") == 0) ?
		    -32768 :
		    0;
		long long v  = strtonum(val, lo, 32767, &errstr);
		if (errstr != NULL) {
			cmdq_error(cmdq, "set: %s: bad value: %s", key, val);
			xfree(val);
			return (CMD_RETURN_ERROR);
		}
		struct cow_view_cfg	    *cfg = cfg_target(cmdq);
		struct cow_titlebar_squeeze *sq	 = NULL;
		if (cfg != NULL)
			sq = &cfg->titlebar.squeeze;
		else if (cow_config != NULL)
			sq = &cow_config->titlebar.squeeze;
		if (sq != NULL) {
			if (strcmp(key, "titlebar.squeeze.width") == 0)
				sq->width = (int)v;
			else if (strcmp(key, "titlebar.squeeze.offset") == 0)
				sq->offset = (int)v;
			else
				sq->min_width = (int)v;
			if (cfg == NULL)
				stamp_all_views_from_global();
			refresh_visuals();
		}
		xfree(val);
		return (CMD_RETURN_NORMAL);
	}

	if (strcmp(key, "container.tab.sunken") == 0) {
		bool b;
		if (!cmd_parse_bool(val, &b)) {
			xfree(val);
			return (CMD_RETURN_NORMAL);
		}
		struct cow_view_cfg *cfg = cfg_target(cmdq);
		if (cfg != NULL) {
			cfg->container.tab_sunken_inactive = b;
		} else if (cow_config != NULL) {
			cow_config->container.tab_sunken_inactive = b;
			stamp_all_views_from_global();
		}
		xfree(val);
		refresh_visuals();
		return (CMD_RETURN_NORMAL);
	}

	if (strcmp(key, "edge_scroll") == 0) {
		bool b;
		if (cow_config != NULL && cmd_parse_bool(val, &b)) {
			cow_config->edge_scroll = b;
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
		bool b;
		if (cow_config != NULL && cmd_parse_bool(val, &b))
			cow_config->page_scroll_animate = b;
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

	if (strcmp(key, "raise_on_focus") == 0) {
		bool b;
		if (cow_config != NULL && cmd_parse_bool(val, &b)) {
			cow_config->raise_on_focus = b;
		}
		xfree(val);
		return (CMD_RETURN_NORMAL);
	}

	if (strncmp(key, "titlebar.button_", 16) == 0) {
		do_set_button(key, val, cmdq);
		xfree(val);
		return (CMD_RETURN_NORMAL);
	}

	if (strcmp(key, "default_layer") == 0 ||
	    strcmp(key, "max_layer") == 0) {
		const char *errstr;
		long long   v = strtonum(val, 0, 255, &errstr);
		if (errstr != NULL) {
			cmdq_error(cmdq, "set: %s: bad value: %s", key, val);
			xfree(val);
			return (CMD_RETURN_ERROR);
		}
		if (cow_config != NULL) {
			if (strcmp(key, "default_layer") == 0) {
				int max = (int)cow_config->max_layer;
				if ((int)v > max)
					v = max;
				cow_config->default_layer = (uint8_t)v;
			} else {
				/*
				 * max_layer: clamp every existing view's
				 * layer to the new cap so the invariant
				 * (view->layer <= max_layer) holds for
				 * subsequent stacking passes.
				 */
				cow_config->max_layer = (uint8_t)v;
				if (cow_config->default_layer > (uint8_t)v)
					cow_config->default_layer = (uint8_t)v;
				struct cow_view *vw;
				bool		 any = false;
				wl_list_for_each(vw, &cow_wm.views, wm_link)
				{
					if (vw->layer > (uint8_t)v) {
						vw->layer = (uint8_t)v;
						any	  = true;
					}
				}
				if (any) {
					cow_stack_mark_dirty();
					cow_wm_request_manage_dirty();
				}
			}
		}
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
	const char *errstr;
	long long   v = strtonum(val, 0, INT_MAX, &errstr);
	if (errstr != NULL) {
		cmdq_error(cmdq, "set: %s: bad value: %s", key, val);
		return;
	}

	struct cow_view_cfg *cfg = cfg_target(cmdq);

	/*
	 * Per-view-able integer fields.  When a target window is
	 * resolved, write into the view's cfg snapshot only.  When no
	 * target is set, write cow_config and stamp every view's
	 * snapshot so the change is reflected on already-mapped
	 * windows.
	 */
	if (strcmp(key, "border") == 0) {
		if (cfg != NULL) {
			cfg->border = (int)v;
		} else {
			cow_config->border = (int)v;
			stamp_all_views_from_global();
		}
		refresh_visuals();
		return;
	}
	if (strcmp(key, "corner_length") == 0) {
		if (cfg != NULL) {
			cfg->corner_length = (int)v;
			if (cfg->corner_length == 0)
				cfg->handles = false;
		} else {
			cow_config->corner_length = (int)v;
			if (cow_config->corner_length == 0)
				cow_config->handles = false;
			stamp_all_views_from_global();
		}
		refresh_visuals();
		return;
	}
	if (strcmp(key, "icon.size") == 0) {
		if (cfg != NULL) {
			cfg->icon_size = (int)v;
		} else {
			cow_config->icon_size = (int)v;
			stamp_all_views_from_global();
		}
		refresh_visuals();
		return;
	}

	/*
	 * Global-only integer fields.  These describe behaviour or
	 * geometry that doesn't make sense per-view: reserved area is
	 * an output-level subtraction, step is the keyboard move/resize
	 * granularity, snap_distance is the snap floor for any drag.
	 */
	struct {
		const char *name;
		int	   *ptr;
	} fields[] = {
		{ "step",		  &cow_config->step	    },
		{ "reserved.top",	  &cow_config->reserved.top    },
		{ "reserved.bottom", &cow_config->reserved.bottom },
		{ "reserved.left",   &cow_config->reserved.left   },
		{ "reserved.right",  &cow_config->reserved.right	},
		{ "snap_distance",   &cow_config->snap_distance   },
		{ NULL,		NULL			     },
	};

	for (int i = 0; fields[i].name != NULL; i++) {
		if (strcmp(key, fields[i].name) != 0)
			continue;
		*fields[i].ptr = (int)v;
		refresh_visuals();
		return;
	}
}

static void
do_set_colour(const char *key, const char *val, struct cmd_q *cmdq)
{
	/* Per-output background: output.colour.background.<name>.
	 * Per-output, not per-view; no snapshot routing. */
	if (strncmp(key, "output.colour.background.", 25) == 0) {
		const char *oname = key + 25;
		float	    c[4];
		if (!cow_colour_parse(val, c)) {
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

	/* Per-view-able from here on.  Each branch resolves the target
	 * struct (view->cfg or cow_config) up front, then writes the
	 * parsed colour into it.  After a global write, all snapshots
	 * are restamped. */
	struct cow_view_cfg *cfg = cfg_target(cmdq);

	/* Uniform colour shortcuts */
	if (strcmp(key, "colour.active") == 0 ||
	    strcmp(key, "colour.inactive") == 0) {
		float c[4];
		if (!cow_colour_parse(val, c)) {
			cmdq_error(cmdq, "set: invalid colour value");
			return;
		}
		bool is_active = (strcmp(key, "colour.active") == 0);
		struct cow_border_colour *bc;
		if (cfg != NULL) {
			bc = is_active ? &cfg->border_active :
					 &cfg->border_inactive;
		} else {
			bc = is_active ? &cow_config->border_active :
					 &cow_config->border_inactive;
		}
		memcpy(bc->n, c, sizeof(float) * 4);
		memcpy(bc->s, c, sizeof(float) * 4);
		memcpy(bc->e, c, sizeof(float) * 4);
		memcpy(bc->w, c, sizeof(float) * 4);
		if (cfg == NULL)
			stamp_all_views_from_global();
		refresh_visuals();
		return;
	}

	/* Titlebar colours: foreground + background, active + inactive. */
	struct {
		const char *name;
		size_t	    cfg_off; /* offset into cow_view_cfg.titlebar */
	} tb_fields[] = {
		{ "titlebar.active_colour",
		    offsetof(struct cow_titlebar_config, active_colour) },
		{ "titlebar.inactive_colour",
		    offsetof(struct cow_titlebar_config, inactive_colour) },
		{ "titlebar.fg_active",
		    offsetof(struct cow_titlebar_config, fg_active) },
		{ "titlebar.fg_inactive",
		    offsetof(struct cow_titlebar_config, fg_inactive) },
		{ NULL, 0 },
	};
	for (int i = 0; tb_fields[i].name != NULL; i++) {
		if (strcmp(key, tb_fields[i].name) != 0)
			continue;
		struct cow_titlebar_config *tb = (cfg != NULL) ?
		    &cfg->titlebar :
		    &cow_config->titlebar;
		float *ptr = (float *)((char *)tb + tb_fields[i].cfg_off);
		if (!cow_colour_parse(val, ptr)) {
			cmdq_error(cmdq, "set: invalid colour value");
			return;
		}
		if (cfg == NULL)
			stamp_all_views_from_global();
		refresh_visuals();
		return;
	}

	/* Container tab colours */
	struct {
		const char *name;
		size_t	    fg_off; /* offset into cow_container_config */
		size_t	    has_off;
	} ct_fields[] = {
		{ "container.tab.active_colour",
		    offsetof(struct cow_container_config, tab_active),
		    offsetof(struct cow_container_config, has_tab_active) },
		{ "container.tab.inactive_colour",
		    offsetof(struct cow_container_config, tab_inactive),
		    offsetof(struct cow_container_config, has_tab_inactive) },
		{ "container.tab.fg_active",
		    offsetof(struct cow_container_config, tab_fg_active),
		    offsetof(struct cow_container_config, has_tab_fg_active) },
		{ "container.tab.fg_inactive",
		    offsetof(struct cow_container_config, tab_fg_inactive),
		    offsetof(struct cow_container_config,
			has_tab_fg_inactive) },
		{ NULL, 0, 0 },
	};
	for (int i = 0; ct_fields[i].name != NULL; i++) {
		if (strcmp(key, ct_fields[i].name) != 0)
			continue;
		struct cow_container_config *cc = (cfg != NULL) ?
		    &cfg->container :
		    &cow_config->container;
		float *ptr = (float *)((char *)cc + ct_fields[i].fg_off);
		bool  *has = (bool *)((char *)cc + ct_fields[i].has_off);
		if (!cow_colour_parse(val, ptr)) {
			cmdq_error(cmdq, "set: invalid colour value");
			return;
		}
		*has = true;
		if (cfg == NULL)
			stamp_all_views_from_global();
		refresh_visuals();
		return;
	}

	/* output.colour.background -- global-only (per-output bg lives
	 * on cow_output via the .<name> form handled above). */
	if (strcmp(key, "output.colour.background") == 0) {
		if (!cow_colour_parse(val, cow_config->output_bg_colour)) {
			cmdq_error(cmdq, "set: invalid colour value");
			return;
		}
		cow_config->has_output_bg_colour = true;
		refresh_visuals();
		return;
	}

	/* Per-edge border colours: colour.{active,inactive}.{n,s,e,w,
	 * nw,ne,sw,se}.  Resolve the cow_border_colour, then map the
	 * suffix to the matching field. */
	bool is_active	 = (strncmp(key, "colour.active.", 14) == 0);
	bool is_inactive = (strncmp(key, "colour.inactive.", 16) == 0);

	if (!is_active && !is_inactive)
		return;

	struct cow_border_colour *bc;
	if (cfg != NULL) {
		bc = is_active ? &cfg->border_active : &cfg->border_inactive;
	} else {
		bc = is_active ? &cow_config->border_active :
				 &cow_config->border_inactive;
	}

	const char *edge = key + (is_active ? 14 : 16);
	float	   *ptr;
	bool	   *has = NULL;
	if (strcmp(edge, "n") == 0)
		ptr = bc->n;
	else if (strcmp(edge, "s") == 0)
		ptr = bc->s;
	else if (strcmp(edge, "e") == 0)
		ptr = bc->e;
	else if (strcmp(edge, "w") == 0)
		ptr = bc->w;
	else if (strcmp(edge, "nw") == 0) {
		ptr = bc->nw;
		has = &bc->has_nw;
	} else if (strcmp(edge, "ne") == 0) {
		ptr = bc->ne;
		has = &bc->has_ne;
	} else if (strcmp(edge, "sw") == 0) {
		ptr = bc->sw;
		has = &bc->has_sw;
	} else if (strcmp(edge, "se") == 0) {
		ptr = bc->se;
		has = &bc->has_se;
	} else {
		return;
	}

	if (!cow_colour_parse(val, ptr)) {
		cmdq_error(cmdq, "set: invalid colour value");
		return;
	}
	if (has != NULL)
		*has = true;
	if (bc->has_nw || bc->has_ne || bc->has_sw || bc->has_se)
		bc->has_corners = true;
	if (cfg == NULL)
		stamp_all_views_from_global();
	refresh_visuals();
}

static void
do_set_border_style(const char *val, struct cmd_q *cmdq)
{
	enum cow_border_style style;
	if (strcmp(val, "fvwm") == 0)
		style = COW_BORDER_STYLE_FVWM;
	else if (strcmp(val, "mwm") == 0)
		style = COW_BORDER_STYLE_MWM;
	else
		style = COW_BORDER_STYLE_NONE;

	struct cow_view_cfg *cfg = cfg_target(cmdq);
	if (cfg != NULL) {
		cfg->border_style = style;
	} else {
		cow_config->border_style = style;
		stamp_all_views_from_global();
	}
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
	cow_wm_request_manage_dirty();
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
do_set_font(const char *val, struct cmd_q *cmdq)
{
	struct cow_view_cfg *cfg = cfg_target(cmdq);
	if (cfg != NULL) {
		xfree(cfg->font_name);
		cfg->font_name = xstrdup(val);
	} else {
		xfree(cow_config->font_name);
		cow_config->font_name = xstrdup(val);
		stamp_all_views_from_global();
	}
	refresh_visuals();
}

static void
do_set_titlebar(const char *key, const char *val, struct cmd_q *cmdq)
{
	bool b;
	if (!cmd_parse_bool(val, &b))
		return;

	struct cow_view_cfg *cfg = cfg_target(cmdq);

	if (strcmp(key, "titlebar.enabled") == 0) {
		if (cfg != NULL) {
			cfg->titlebar.enabled = b;
		} else {
			cow_config->titlebar.enabled = b;
			stamp_all_views_from_global();
		}
	} else if (strcmp(key, "handles") == 0) {
		if (cfg != NULL) {
			cfg->handles = b;
		} else {
			cow_config->handles = b;
			stamp_all_views_from_global();
		}
	}
	refresh_visuals();
}

static void
do_set_button(const char *key, const char *val, struct cmd_q *cmdq)
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

	struct cow_view_cfg	   *cfg = cfg_target(cmdq);
	struct cow_titlebar_config *tb	= (cfg != NULL) ? &cfg->titlebar :
							  &cow_config->titlebar;

	for (int i = 0; btn_fields[i].vector_key != NULL; i++) {
		if (strcmp(key, btn_fields[i].vector_key) == 0) {
			cow_parse_vector_button_string(
			    &tb->buttons[btn_fields[i].idx].vector, val);
			if (cfg == NULL)
				stamp_all_views_from_global();
			refresh_visuals();
			return;
		}
		if (strcmp(key, btn_fields[i].action_key) == 0) {
			enum cow_button_action a;
			if (strcmp(val, "close") == 0)
				a = COW_BUTTON_ACTION_CLOSE;
			else if (strcmp(val, "maximize") == 0)
				a = COW_BUTTON_ACTION_MAXIMIZE;
			else if (strcmp(val, "iconify") == 0)
				a = COW_BUTTON_ACTION_ICONIFY;
			else
				a = COW_BUTTON_ACTION_NONE;
			tb->buttons[btn_fields[i].idx].action = a;
			if (cfg == NULL)
				stamp_all_views_from_global();
			return;
		}
	}
}

static void
refresh_visuals(void)
{
	if (cow_ipc.startup_mode)
		return;

	/* Each view recomputes its decoration scaffolding from its
	 * own per-view cfg snapshot.  This runs after either a
	 * view-targeted set (only the targeted view's snapshot
	 * changed; other views read unchanged data) or a global set
	 * (every view's snapshot was just stamped from cow_config).
	 */
	struct cow_view *view;
	wl_list_for_each(view, &cow_wm.views, wm_link)
	{
		if (view->closed)
			continue;
		int new_tt = 0;
		if (view->cfg.titlebar.enabled) {
			int h = view->cfg.titlebar.height;
			if (h > 0) {
				new_tt = h;
			} else {
				struct cow_font tb_font;
				const char     *fname = view->cfg.font_name ?
					view->cfg.font_name :
					"monospace 10";
				cow_font_init(&tb_font, fname);
				new_tt = cow_titlebar_compute_thickness(
				    &tb_font);
			}
		}
		view->dec.dirty		      = true;
		view->dec.border_width	      = view->cfg.border;
		view->dec.title_thickness     = new_tt;
		view->dec.last_content_width  = -1;
		view->dec.last_content_height = -1;
		if (view->dec.buffer != NULL) {
			cow_shm_buffer_destroy(view->dec.buffer);
			view->dec.buffer = NULL;
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

	cow_wm_manage_dirty_now();
}
