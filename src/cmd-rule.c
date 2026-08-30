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
 * `rule` -- bind a chain of cow commands to a lifecycle event.
 *
 *   rule -g | -t output[:desk]   -n <name> -T <event> [-s <selector>] cmd...
 *   rule -d -n <name>
 *
 * -g and -t are mutually exclusive (one is required for adds).  -n
 * names the rule (unique across all rules).  -T selects the
 * lifecycle event (see cow_rule_type_from_string).  -s restricts
 * firing to views matching the selector grammar (%app | /title/ |
 * #id) -- only meaningful for view-bearing events.
 *
 * The remaining argv tokens are joined with single spaces and parsed
 * via cmd_string_parse, so semicolons inside (escaped or not) chain
 * commands as usual.
 *
 * Rules are inserted into the active list immediately, including
 * during config parse: the -t output[:desk] specifier is stored as
 * the literal output name and matched against output->name at fire
 * time, so registration does not need outputs to exist yet.
 */

#include <stdlib.h>
#include <string.h>

#include "cow/cmd.h"
#include "cow/memory.h"
#include "cow/output.h"
#include "cow/rule.h"
#include "cow/wm.h"

static enum cmd_retval cmd_rule_exec(struct cmd *, struct cmd_q *);
static bool	       do_rule_add(struct args *, struct cmd_q *);

struct cmd_entry cmd_rule_entry = {
	"rule",
	"+dgn:T:t:s:",
	0,
	-1,
	"rule -g | -t output[:desk] -n <name> -T <event> [-s sel] cmd...\n"
	"rule -d -n <name>",
	cmd_rule_exec,
	CMD_OWN_TS | CMD_CONFIG,
};

static enum cmd_retval
cmd_rule_exec(struct cmd *self, struct cmd_q *cmdq)
{
	struct args *args = self->args;

	/* Deletion: -d -n <name>. */
	if (args_has(args, 'd')) {
		const char *name = args_get(args, 'n');
		if (name == NULL) {
			cmdq_error(cmdq, "rule: -d requires -n <name>");
			return (CMD_RETURN_ERROR);
		}
		if (!cow_rule_remove_by_name(name)) {
			cmdq_error(cmdq, "rule: no rule named '%s'", name);
			return (CMD_RETURN_ERROR);
		}
		return (CMD_RETURN_NORMAL);
	}

	if (!do_rule_add(args, cmdq))
		return (CMD_RETURN_ERROR);
	return (CMD_RETURN_NORMAL);
}

static bool
do_rule_add(struct args *args, struct cmd_q *cmdq)
{
	const char *name  = args_get(args, 'n');
	const char *T_val = args_get(args, 'T');
	bool	    is_g  = args_has(args, 'g');
	const char *t_val = args_get(args, 't');
	const char *s_val = args_get(args, 's');

	if (name == NULL) {
		cmdq_error(cmdq, "rule: missing -n <name>");
		return false;
	}
	if (T_val == NULL) {
		cmdq_error(cmdq, "rule: missing -T <event>");
		return false;
	}
	if (is_g && t_val != NULL) {
		cmdq_error(cmdq, "rule: -g and -t are mutually exclusive");
		return false;
	}
	if (!is_g && t_val == NULL) {
		cmdq_error(cmdq, "rule: one of -g or -t is required");
		return false;
	}
	if (args->argc < 1) {
		cmdq_error(cmdq, "rule: missing command body");
		return false;
	}

	enum cow_rule_type type;
	if (!cow_rule_type_from_string(T_val, &type)) {
		cmdq_error(cmdq, "rule: unknown event: %s", T_val);
		return false;
	}
	if ((type == COW_RULE_ON_START || type == COW_RULE_ON_DESKS_CHANGED) &&
	    t_val != NULL) {
		cmdq_error(cmdq, "rule: -t is not valid for event %s", T_val);
		return false;
	}

	/* -s only meaningful for view-bearing events. */
	if (s_val != NULL &&
	    (type == COW_RULE_ON_NEW_DESK || type == COW_RULE_ON_NEW_PAGE ||
		type == COW_RULE_ON_START || type == COW_RULE_ON_RESTART ||
		type == COW_RULE_ON_OUTPUTS_CHANGED ||
		type == COW_RULE_ON_DESKS_CHANGED)) {
		cmdq_error(cmdq, "rule: -s is not valid for event %s", T_val);
		return false;
	}

	if (cow_rule_find_by_name(name) != NULL) {
		cmdq_error(cmdq, "rule: a rule named '%s' already exists",
		    name);
		return false;
	}

	/* Validate -t up front so malformed scope syntax is rejected before
	 * parsing the command body or allocating the rule.
	 */
	struct cow_policy policy;
	cow_policy_init(&policy);
	policy.name = xstrdup(name);
	policy.is_global = is_g;
	if (s_val != NULL)
		policy.match_spec = xstrdup(s_val);
	if (t_val != NULL) {
		char *t_cause = NULL;
		if (!cow_policy_parse_scope(t_val, &policy, &t_cause)) {
			cmdq_error(cmdq, "rule: %s",
			    t_cause != NULL ? t_cause : "bad -t spec");
			xfree(t_cause);
			cow_policy_fini(&policy);
			return false;
		}
	}

	/* Reconstruct + parse the command body. */
	char *body = cmd_argv_to_command_string(args->argc, args->argv, 0);
	struct cmd_list *cmdlist = NULL;
	char		*cause	 = NULL;
	if (cmd_string_parse(body, &cmdlist, NULL, 0, &cause) != 0 ||
	    cmdlist == NULL) {
		cmdq_error(cmdq, "rule: parse: %s",
		    cause != NULL ? cause : "(unknown)");
		xfree(cause);
		xfree(body);
		cow_policy_fini(&policy);
		return false;
	}
	xfree(cause);
	xfree(body);

	struct cow_rule *rule = xcalloc(1, sizeof(*rule));
	rule->policy	      = policy;
	rule->type	      = type;
	rule->cmdlist	      = cmdlist;

	cow_rule_add(rule);
	return true;
}
