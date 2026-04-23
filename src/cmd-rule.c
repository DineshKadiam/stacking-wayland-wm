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
 * Command: rule -- define a window rule.
 *
 * usage: rule [-T] [-t output:desk] pattern [key value ...]
 *
 * During startup, rules are deferred until outputs are available.
 */

#include <fnmatch.h>
#include <stdlib.h>
#include <string.h>

#include "cow/compat.h"
#include "cow/cmd.h"
#include "cow/colour.h"
#include "cow/config.h"
#include "cow/ipc.h"
#include "cow/memory.h"
#include "cow/output.h"
#include "cow/view.h"
#include "cow/wm.h"

static enum cmd_retval cmd_rule_exec(struct cmd *, struct cmd_q *);
static void	       do_rule(struct cmd *, struct cmd_q *);
static bool	       has_glob_chars(const char *);
static bool	       parse_colour(const char *, float[4]);

struct cmd_entry cmd_rule_entry = {
	"rule",
	"t:s:T",
	1,
	-1,
	"rule [-T] [-t spec] pattern [key value ...]",
	cmd_rule_exec,
};

/* Deferred rule list for startup mode. */
static char **deferred_rules;
static int    num_deferred_rules;
static int    deferred_rules_cap;

static enum cmd_retval
cmd_rule_exec(struct cmd *self, struct cmd_q *cmdq)
{
	struct args *args = self->args;

	/* Special case: "rule restart <command ...>" registers a lifecycle
	 * hook run synchronously before cow restarts on SIGUSR1. */
	if (args->argc >= 2 && strcmp(args->argv[0], "restart") == 0) {
		char *cmd = cmd_argv_to_string(args->argc, args->argv, 1);
		xfree(cow_config->hook_restart);
		cow_config->hook_restart = cmd;
		return (CMD_RETURN_NORMAL);
	}

	if (cow_ipc.startup_mode) {
		/* Defer: reconstruct the full command line and save it */
		char   line[2048];
		size_t pos = 0;

		/* Rebuild: rule [-T] [-t spec] args... */
		pos += snprintf(line + pos, sizeof(line) - pos, "rule ");
		if (args_has(self->args, 'T'))
			pos += snprintf(line + pos, sizeof(line) - pos, "-T ");
		const char *t_spec = args_get(self->args, 't');
		if (t_spec != NULL)
			pos += snprintf(line + pos, sizeof(line) - pos,
			    "-t '%s' ", t_spec);
		for (int i = 0; i < self->args->argc; i++) {
			if (i > 0)
				pos += snprintf(line + pos, sizeof(line) - pos,
				    " ");
			pos += snprintf(line + pos, sizeof(line) - pos, "%s",
			    self->args->argv[i]);
		}

		if (num_deferred_rules >= deferred_rules_cap) {
			deferred_rules_cap = deferred_rules_cap ?
			    deferred_rules_cap * 2 :
			    8;
			deferred_rules	   = xrealloc(deferred_rules,
				deferred_rules_cap, sizeof(char *));
		}
		deferred_rules[num_deferred_rules++] = xstrdup(line);

		return (CMD_RETURN_NORMAL);
	}

	do_rule(self, cmdq);
	return (CMD_RETURN_NORMAL);
}

static void
do_rule(struct cmd *self, struct cmd_q *cmdq)
{
	struct args *args	 = self->args;
	bool	     match_title = args_has(args, 'T');

	struct cow_view_config_entry *entry = xcalloc(1, sizeof(*entry));
	entry->assign_desk_nr		    = -1;
	entry->desk_nr			    = -1;

	const char *pattern = args->argv[0];
	entry->use_glob	    = has_glob_chars(pattern);

	if (match_title)
		entry->title = xstrdup(pattern);
	else
		entry->app_id = xstrdup(pattern);

	/* Target constraint from -t.
	 * Parse the spec string directly so that omitted parts (e.g. ":1"
	 * meaning no output constraint) are not filled with resolved
	 * defaults.  Format: [output]:[desk] */
	const char *t_spec = args_get(args, 't');
	if (t_spec != NULL) {
		char *spec  = xstrdup(t_spec);
		char *colon = strchr(spec, ':');
		if (colon != NULL) {
			*colon			= '\0';
			const char *output_part = spec;
			const char *desk_part	= colon + 1;
			if (*output_part != '\0') {
				struct cow_output *o = cow_output_find_by_name(
				    output_part);
				if (o != NULL && o->name != NULL)
					entry->output_name = xstrdup(o->name);
			}
			if (*desk_part != '\0') {
				char *end;
				long  nr = strtol(desk_part, &end, 10);
				if (end != desk_part && *end == '\0' &&
				    nr >= 0 && nr < COW_NR_OF_DESKS)
					entry->desk_nr = (int)nr;
			}
		} else {
			/* No colon: treat entire spec as output name */
			if (*spec != '\0') {
				struct cow_output *o = cow_output_find_by_name(
				    spec);
				if (o != NULL && o->name != NULL)
					entry->output_name = xstrdup(o->name);
			}
		}
		xfree(spec);
	}

	/* Parse key-value pairs from argv[1..] */
	for (int i = 1; i + 1 < args->argc; i += 2) {
		const char *key = args->argv[i];
		const char *val = args->argv[i + 1];

		if (strcmp(key, "desk") == 0) {
			const char *errstr;
			entry->assign_desk_nr = (int)strtonum(val, 0, 9,
			    &errstr);
			if (errstr != NULL)
				entry->assign_desk_nr = -1;
		} else if (strcmp(key, "sticky") == 0) {
			entry->sticky = (strcmp(val, "true") == 0);
		} else if (strcmp(key, "border") == 0) {
			const char *errstr;
			entry->border = (int)strtonum(val, 0, 100, &errstr);
			if (errstr == NULL)
				entry->has_border = true;
		} else if (strcmp(key, "colour.active") == 0) {
			/* Uniform active border colour (all edges) */
			float c[4];
			if (parse_colour(val, c)) {
				struct cow_border_colour *bc =
				    &entry->border_colour_active;
				memcpy(bc->n, c, sizeof(float) * 4);
				memcpy(bc->s, c, sizeof(float) * 4);
				memcpy(bc->e, c, sizeof(float) * 4);
				memcpy(bc->w, c, sizeof(float) * 4);
				entry->has_border_colour_active = true;
			}
		} else if (strcmp(key, "colour.inactive") == 0) {
			float c[4];
			if (parse_colour(val, c)) {
				struct cow_border_colour *bc =
				    &entry->border_colour_inactive;
				memcpy(bc->n, c, sizeof(float) * 4);
				memcpy(bc->s, c, sizeof(float) * 4);
				memcpy(bc->e, c, sizeof(float) * 4);
				memcpy(bc->w, c, sizeof(float) * 4);
				entry->has_border_colour_inactive = true;
			}
		} else if (strncmp(key, "colour.active.", 14) == 0) {
			/* Per-edge: colour.active.n, .s, .e, .w, .nw etc */
			const char *edge = key + 14;
			float	    c[4];
			if (!parse_colour(val, c))
				goto bad_key;
			struct cow_border_colour *bc =
			    &entry->border_colour_active;
			entry->has_border_colour_active = true;
			if (strcmp(edge, "n") == 0)
				memcpy(bc->n, c, sizeof(float) * 4);
			else if (strcmp(edge, "s") == 0)
				memcpy(bc->s, c, sizeof(float) * 4);
			else if (strcmp(edge, "e") == 0)
				memcpy(bc->e, c, sizeof(float) * 4);
			else if (strcmp(edge, "w") == 0)
				memcpy(bc->w, c, sizeof(float) * 4);
			else if (strcmp(edge, "nw") == 0) {
				memcpy(bc->nw, c, sizeof(float) * 4);
				bc->has_nw	= true;
				bc->has_corners = true;
			} else if (strcmp(edge, "ne") == 0) {
				memcpy(bc->ne, c, sizeof(float) * 4);
				bc->has_ne	= true;
				bc->has_corners = true;
			} else if (strcmp(edge, "sw") == 0) {
				memcpy(bc->sw, c, sizeof(float) * 4);
				bc->has_sw	= true;
				bc->has_corners = true;
			} else if (strcmp(edge, "se") == 0) {
				memcpy(bc->se, c, sizeof(float) * 4);
				bc->has_se	= true;
				bc->has_corners = true;
			} else
				goto bad_key;
		} else if (strncmp(key, "colour.inactive.", 16) == 0) {
			const char *edge = key + 16;
			float	    c[4];
			if (!parse_colour(val, c))
				goto bad_key;
			struct cow_border_colour *bc =
			    &entry->border_colour_inactive;
			entry->has_border_colour_inactive = true;
			if (strcmp(edge, "n") == 0)
				memcpy(bc->n, c, sizeof(float) * 4);
			else if (strcmp(edge, "s") == 0)
				memcpy(bc->s, c, sizeof(float) * 4);
			else if (strcmp(edge, "e") == 0)
				memcpy(bc->e, c, sizeof(float) * 4);
			else if (strcmp(edge, "w") == 0)
				memcpy(bc->w, c, sizeof(float) * 4);
			else
				goto bad_key;
		} else if (strcmp(key, "titlebar.active_colour") == 0) {
			if (parse_colour(val, entry->titlebar_active_colour))
				entry->has_titlebar_active = true;
		} else if (strcmp(key, "titlebar.inactive_colour") == 0) {
			if (parse_colour(val, entry->titlebar_inactive_colour))
				entry->has_titlebar_inactive = true;
		} else if (strcmp(key, "titlebar.fg_active") == 0) {
			if (parse_colour(val, entry->titlebar_fg_active))
				entry->has_titlebar_fg_active = true;
		} else if (strcmp(key, "titlebar.fg_inactive") == 0) {
			if (parse_colour(val, entry->titlebar_fg_inactive))
				entry->has_titlebar_fg_inactive = true;
		} else if (strcmp(key, "titlebar.enabled") == 0) {
			entry->has_titlebar_enabled = true;
			entry->titlebar_enabled = (strcmp(val, "true") == 0);
		} else {
		bad_key:
			cmdq_error(cmdq, "rule: unknown key: %s", key);
			xfree(entry->app_id);
			xfree(entry->title);
			xfree(entry->output_name);
			xfree(entry);
			return;
		}
	}

	/* Insert at tail so rules are checked in config file order.
	 * "Last match wins" means later rules override earlier ones. */
	wl_list_insert(cow_config->view_configs.prev, &entry->link);

	/* Re-resolve styles for all views so the new rule takes effect */
	struct cow_view *view;
	wl_list_for_each(view, &cow_wm.views, wm_link)
	{
		if (!view->closed) {
			cow_view_resolve_style(view);
			view->dec.dirty = true;
		}
	}
	cow_wm.needs_manage_dirty = true;
}

static bool
has_glob_chars(const char *s)
{
	return strchr(s, '*') != NULL || strchr(s, '?') != NULL ||
	    strchr(s, '[') != NULL;
}

static bool
parse_colour(const char *str, float out[4])
{
	char	     *end;
	unsigned long v = strtoul(str, &end, 0);
	if (end == str)
		return false;
	cow_colour_convert(out, (uint32_t)v);
	return true;
}

void
cow_rule_run_deferred(void)
{
	for (int i = 0; i < num_deferred_rules; i++) {
		struct cmd_list *cmdlist = NULL;
		char		*cause	 = NULL;

		if (cmd_string_parse(deferred_rules[i], &cmdlist, NULL, 0,
			&cause) == 0 &&
		    cmdlist != NULL) {
			struct cmd_q *cmdq = cmdq_new();
			cmdq->client_fd	   = -1;
			cmdq_run(cmdq, cmdlist);
			cmd_list_free(cmdlist);
			cmdq_free(cmdq);
		}
		xfree(cause);
		xfree(deferred_rules[i]);
	}
	xfree(deferred_rules);
	deferred_rules	   = NULL;
	num_deferred_rules = 0;
	deferred_rules_cap = 0;
}
