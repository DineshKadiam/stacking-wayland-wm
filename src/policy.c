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

#include <stdlib.h>
#include <string.h>

#include "cow/memory.h"
#include "cow/output.h"
#include "cow/policy.h"
#include "cow/view.h"

void
cow_policy_init(struct cow_policy *policy)
{
	memset(policy, 0, sizeof(*policy));
	policy->desk_nr = -1;
}

void
cow_policy_fini(struct cow_policy *policy)
{
	if (policy == NULL)
		return;
	xfree(policy->name);
	xfree(policy->output_name);
	xfree(policy->desk_name);
	xfree(policy->match_spec);
	cow_policy_init(policy);
}

bool
cow_policy_parse_scope(const char *spec, struct cow_policy *policy,
    char **cause)
{
	/* Store output and desk names verbatim.  Configuration is parsed before
	 * outputs exist, so both rules and styles resolve their scope when they
	 * are matched rather than when they are defined.
	 */
	if (spec == NULL || spec[0] == '\0') {
		xasprintf(cause, "empty -t spec");
		return (false);
	}

	char *dup = xstrdup(spec);
	char *colon = strchr(dup, ':');
	if (colon != NULL) {
		*colon = '\0';
		const char *output = dup;
		const char *desk = colon + 1;

		if (strchr(desk, ':') != NULL) {
			xasprintf(cause,
			    "-t %s: too many colons (expected output[:desk])",
			    spec);
			xfree(dup);
			return (false);
		}
		if (*output != '\0')
			policy->output_name = xstrdup(output);
		if (*desk != '\0') {
			char *end;
			long nr = strtol(desk, &end, 10);
			if (end != desk && *end == '\0' && nr >= 0 && nr <= 255)
				policy->desk_nr = (int)nr;
			else
				policy->desk_name = xstrdup(desk);
		}
	} else {
		policy->output_name = xstrdup(dup);
	}
	xfree(dup);
	return (true);
}

bool
cow_policy_scope_matches(const struct cow_policy *policy,
    const struct cow_desk *desk, const struct cow_output *output)
{
	if (policy->output_name != NULL &&
	    (output == NULL || output->name == NULL ||
		strcmp(policy->output_name, output->name) != 0))
		return (false);
	if (policy->desk_name != NULL) {
		if (desk == NULL || desk->name == NULL ||
		    strcmp(policy->desk_name, desk->name) != 0)
			return (false);
	} else if (policy->desk_nr >= 0 &&
	    (desk == NULL || desk->nr != policy->desk_nr)) {
		return (false);
	}
	return (true);
}

bool
cow_policy_view_matches(const struct cow_policy *policy,
    const struct cow_view *view)
{
	if (policy->match_spec == NULL)
		return (true);
	return (view != NULL && cow_view_matches_selector(view,
	    policy->match_spec));
}

bool
cow_policy_matches_view(const struct cow_policy *policy,
    const struct cow_view *view)
{
	if (view == NULL || view->closed)
		return (false);
	return (cow_policy_scope_matches(policy, view->desk, view->output) &&
	    cow_policy_view_matches(policy, view));
}
