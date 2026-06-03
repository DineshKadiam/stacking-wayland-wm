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

#include "cow/rule.h"

#include <string.h>

#include "cow/cmd.h"
#include "cow/memory.h"
#include "cow/output.h"
#include "cow/view.h"
#include "cow/wm.h"

/*
 * Lookup table mapping rule-type enum values to their canonical string
 * names.  Used by both directions of conversion (string <-> enum) so
 * the spelling lives in exactly one place.
 */
static const struct {
	enum cow_rule_type type;
	const char	  *name;
} rule_type_table[] = {
	{ COW_RULE_ON_MAP,	   "on-map"	    },
	{ COW_RULE_ON_MOVE,	    "on-move"	      },
	{ COW_RULE_ON_RESIZE,	      "on-resize"	  },
	{ COW_RULE_ON_SHADE,	     "on-shade"	},
	{ COW_RULE_ON_UNSHADE,      "on-unshade"	    },
	{ COW_RULE_ON_ICONIFY,      "on-iconify"	    },
	{ COW_RULE_ON_DEICONIFY,	 "on-deiconify"	},
	{ COW_RULE_ON_LAYER_CHANGE, "on-layer-change" },
	{ COW_RULE_ON_NEW_DESK,	"on-new-desk"     },
	{ COW_RULE_ON_NEW_PAGE,	"on-new-page"     },
	{ COW_RULE_ON_RESTART,      "on-restart"	    },
	{ COW_RULE_ON_TITLE_CHANGE, "on-title-change" },
	{ COW_RULE_ON_DESK,	    "on-desk"	      },
};

/*
 * Rules are stored in a file-scope wl_list rather than on cow_wm so
 * that registration during config parse (which happens before
 * cow_wm_init) is well-defined.  cow_wm_init's memset does not touch
 * this list.
 */
static struct wl_list rules;
static bool	      rules_initialised;

static void
rules_lazy_init(void)
{
	if (!rules_initialised) {
		wl_list_init(&rules);
		rules_initialised = true;
	}
}

void
cow_rule_init(void)
{
	rules_lazy_init();
}

static void
cow_rule_free(struct cow_rule *rule)
{
	if (rule == NULL)
		return;
	xfree(rule->name);
	xfree(rule->output_name);
	xfree(rule->desk_name);
	xfree(rule->match_spec);
	if (rule->cmdlist != NULL)
		cmd_list_free(rule->cmdlist);
	xfree(rule);
}

void
cow_rule_fini(void)
{
	if (!rules_initialised)
		return;
	struct cow_rule *r, *tmp;
	wl_list_for_each_safe(r, tmp, &rules, link)
	{
		wl_list_remove(&r->link);
		cow_rule_free(r);
	}
	rules_initialised = false;
}

bool
cow_rule_type_from_string(const char *s, enum cow_rule_type *out)
{
	if (s == NULL || out == NULL)
		return false;
	for (size_t i = 0;
	    i < sizeof(rule_type_table) / sizeof(rule_type_table[0]); i++) {
		if (strcmp(rule_type_table[i].name, s) == 0) {
			*out = rule_type_table[i].type;
			return true;
		}
	}
	return false;
}

const char *
cow_rule_type_name(enum cow_rule_type type)
{
	for (size_t i = 0;
	    i < sizeof(rule_type_table) / sizeof(rule_type_table[0]); i++) {
		if (rule_type_table[i].type == type)
			return rule_type_table[i].name;
	}
	return "unknown";
}

struct cow_rule *
cow_rule_find_by_name(const char *name)
{
	if (name == NULL)
		return NULL;
	rules_lazy_init();
	struct cow_rule *r;
	wl_list_for_each(r, &rules, link)
	{
		if (r->name != NULL && strcmp(r->name, name) == 0)
			return r;
	}
	return NULL;
}

const struct wl_list *
cow_rules_head(void)
{
	rules_lazy_init();
	return &rules;
}

void
cow_rule_add(struct cow_rule *rule)
{
	if (rule == NULL)
		return;
	rules_lazy_init();
	wl_list_insert(rules.prev, &rule->link);
}

bool
cow_rule_remove_by_name(const char *name)
{
	struct cow_rule *r = cow_rule_find_by_name(name);
	if (r == NULL)
		return false;
	wl_list_remove(&r->link);
	cow_rule_free(r);
	return true;
}

/*
 * Per-view selector check.  Used by both phases (a global rule may
 * still carry -s).
 */
static bool
rule_view_matches(const struct cow_rule *rule, const struct cow_view *view)
{
	if (rule->match_spec == NULL)
		return true;
	if (view == NULL)
		return false;
	return cow_view_matches_selector(view, rule->match_spec);
}

/*
 * Scope match for non-global rules.  Returns true when the event's
 * (output, desk) context falls within the rule's -t-derived
 * constraints.  Empty constraints mean "any".
 */
static bool
rule_scope_matches(const struct cow_rule *rule, const struct cow_desk *desk,
    const struct cow_output *output)
{
	if (rule->output_name != NULL) {
		if (output == NULL || output->name == NULL ||
		    strcmp(rule->output_name, output->name) != 0)
			return false;
	}
	if (rule->desk_name != NULL) {
		if (desk == NULL || desk->name == NULL ||
		    strcmp(rule->desk_name, desk->name) != 0)
			return false;
	} else if (rule->desk_nr >= 0) {
		if (desk == NULL || desk->nr != rule->desk_nr)
			return false;
	}
	return true;
}

/* Run a single rule's command body against the supplied context.
 * `event_json` (if non-NULL) is copied onto the cmd_q so commands
 * inside the body can read the per-firing payload (cmd-exec exposes
 * it via $COW_EVENT). */
static void
rule_run(struct cow_rule *rule, struct cow_view *view, struct cow_desk *desk,
    struct cow_output *output, const char *event_json)
{
	struct cmd_q *cmdq = cmdq_new();
	cmdq->client_fd	   = -1;
	cmdq_set_default_target(cmdq, view, desk, output);
	if (event_json != NULL)
		cmdq_set_event_json(cmdq, xstrdup(event_json));
	cmdq_run(cmdq, rule->cmdlist);
	cmdq_free(cmdq);
}

/*
 * True for events whose firing context already names a single view.
 * For these the rule body runs once with default_target.window set
 * to that view; no fan-out is performed.  Events not in this set
 * carry no inherent view (they fire on a desk/page/output transition
 * or globally) and the rule subsystem fans the body out across every
 * view its scope selects, see fanout_views_for_event.
 */
static bool
event_carries_view(enum cow_rule_type type)
{
	switch (type) {
	case COW_RULE_ON_MAP:
	case COW_RULE_ON_MOVE:
	case COW_RULE_ON_RESIZE:
	case COW_RULE_ON_SHADE:
	case COW_RULE_ON_UNSHADE:
	case COW_RULE_ON_ICONIFY:
	case COW_RULE_ON_DEICONIFY:
	case COW_RULE_ON_LAYER_CHANGE:
	case COW_RULE_ON_TITLE_CHANGE:
		return true;
	case COW_RULE_ON_NEW_DESK:
	case COW_RULE_ON_NEW_PAGE:
	case COW_RULE_ON_RESTART:
	case COW_RULE_ON_DESK:
	default:
		return false;
	}
}

/*
 * For a view-less event, decide whether 'view' should receive the
 * rule body when the rule fires.  The event's (output, desk)
 * context narrows the candidate set first; the rule's own scope
 * (output_name, desk_nr, match_spec) then filters per-view.
 *
 * The event context (ev_output, ev_desk) is the (output, desk) the
 * event originated from -- for on-desk this is the desk that just
 * became current, for on-new-page the desk whose page changed, etc.
 * NULL means "any" -- on-restart fires with no context at all.
 */
static bool
fanout_view_matches(const struct cow_rule *rule, const struct cow_view *view,
    const struct cow_desk *ev_desk, const struct cow_output *ev_output)
{
	if (view == NULL || view->closed)
		return false;

	/* Event-context narrowing: a desk-scoped event (e.g. on-desk
	 * fired from desk N becoming current on output O) only reaches
	 * views currently on that (desk, output).  on-restart passes
	 * NULL ev_desk / ev_output and reaches everything. */
	if (ev_desk != NULL && view->desk != ev_desk)
		return false;
	if (ev_output != NULL && view->output != ev_output)
		return false;

	/* Rule scope narrowing: the rule's own -t output_name / -t
	 * desk_nr further constrain the candidate set.  Use the view's
	 * (desk, output) for this match -- under fan-out the rule
	 * naturally sees the view as its target. */
	if (!rule_scope_matches(rule, view->desk, view->output))
		return false;

	/* Rule selector: -s %app, /title/, #id. */
	if (!rule_view_matches(rule, view))
		return false;

	return true;
}

void
cow_rule_fire(enum cow_rule_type type, struct cow_view *view,
    struct cow_desk *desk, struct cow_output *output)
{
	cow_rule_fire_json(type, view, desk, output, NULL);
}

void
cow_rule_fire_json(enum cow_rule_type type, struct cow_view *view,
    struct cow_desk *desk, struct cow_output *output, const char *event_json)
{
	if (type <= COW_RULE_NONE || type >= COW_RULE_TYPE_COUNT)
		return;
	if (!rules_initialised)
		return; /* no rules registered */

	struct cow_rule *rule;

	if (event_carries_view(type)) {
		/*
		 * View-bearing events: phase 1 (globals) then phase 2
		 * (scoped) both fire once with the event's view as the
		 * default target.  Unchanged from the original design.
		 */
		wl_list_for_each(rule, &rules, link)
		{
			if (rule->type != type || !rule->is_global)
				continue;
			if (!rule_view_matches(rule, view))
				continue;
			rule_run(rule, view, desk, output, event_json);
		}
		wl_list_for_each(rule, &rules, link)
		{
			if (rule->type != type || rule->is_global)
				continue;
			if (!rule_scope_matches(rule, desk, output))
				continue;
			if (!rule_view_matches(rule, view))
				continue;
			rule_run(rule, view, desk, output, event_json);
		}
		return;
	}

	/*
	 * View-less event (on-desk, on-new-desk, on-new-page,
	 * on-restart).  For each matching rule, fan the body out to
	 * every view that satisfies (a) the event's narrowing context
	 * and (b) the rule's own scope/selector.  Each view receives
	 * the body with cmdq->default_target.window pointing at it,
	 * so any `set` commands inside route into view->cfg via
	 * cfg_target() in cmd-set.
	 *
	 * Fan-out fires the *whole* body once per view -- side-effecting
	 * non-styling commands (e.g. desk-switch) will run N times.
	 * On-desk and friends are intended as styling hooks; a user who
	 * needs a once-per-event behavioural hook should attach it to a
	 * lifecycle event such as on-restart and not rely on fan-out.
	 */
	wl_list_for_each(rule, &rules, link)
	{
		if (rule->type != type)
			continue;

		struct cow_view *v;
		wl_list_for_each(v, &cow_wm.views, wm_link)
		{
			if (!fanout_view_matches(rule, v, desk, output))
				continue;
			/* Pass v->desk and v->output so any `set` commands
			 * the body issues that consult the desk / output
			 * context see the view's own (desk, output), not
			 * the event's narrower one. */
			rule_run(rule, v, v->desk, v->output, event_json);
		}
	}
}
