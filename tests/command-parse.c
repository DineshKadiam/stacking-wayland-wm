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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cow/cmd.h"
#include "cow/cmd_scan.h"
#include "cow/memory.h"

static int    captured_argc;
static char **captured_argv;

struct cmd_list *
cmd_list_parse(int argc, char **argv, __unused const char *file,
    __unused u_int line, __unused char **cause)
{
	captured_argc = argc;
	captured_argv = xcalloc((size_t)argc, sizeof *captured_argv);
	for (int i = 0; i < argc; i++)
		captured_argv[i] = xstrdup(argv[i]);

	struct cmd_list *list = xcalloc(1, sizeof *list);
	list->references = 1;
	TAILQ_INIT(&list->list);
	return (list);
}

static void
captured_free(void)
{
	for (int i = 0; i < captured_argc; i++)
		xfree(captured_argv[i]);
	xfree(captured_argv);
	captured_argc = 0;
	captured_argv = NULL;
}

static void
check(const char *command, int expected_argc, const char *const *expected)
{
	struct cmd_list *list = NULL;
	char		*error = NULL;

	if (cmd_string_parse(command, &list, NULL, 0, &error) != 0) {
		fprintf(stderr, "%s: %s\n", command,
		    error != NULL ? error : "parse failed");
		exit(1);
	}
	if (captured_argc != expected_argc) {
		fprintf(stderr, "%s: expected %d arguments, got %d\n",
		    command, expected_argc, captured_argc);
		exit(1);
	}
	for (int i = 0; i < expected_argc; i++) {
		if (strcmp(captured_argv[i], expected[i]) != 0) {
			fprintf(stderr, "%s: argument %d: expected '%s', got "
			    "'%s'\n", command, i, expected[i], captured_argv[i]);
			exit(1);
		}
	}

	xfree(error);
	xfree(list);
	captured_free();
}

static void
check_terminator(const char *command, enum cow_cmd_scan_result expected)
{
	size_t end = 0;
	enum cow_cmd_scan_result result = cow_cmd_find_terminator(command,
	    strlen(command), &end);

	if (result != expected) {
		fprintf(stderr, "expected scan result %d, got %d for:\n%s",
		    expected, result, command);
		exit(1);
	}
}

int
main(void)
{
	static const char *const script[] = { "script", "{}" };
	static const char *const adjacent[] = { "script", "{}", "nop" };
	static const char *const rule[] = {
		"rule", "-g", "-Ton-map", "-nonfoot", "-s", "/foot/",
		"script", "{ if self.output == \"HDMI-A-1\"{ } }",
	};
	static const char *const chord[] = {
		"bind", "C+c,C+a", "window-move", "-x", "0",
	};
	static const char *const rule_dump[] = {
		"rule", "-g", "-n", "desk-number-bindings", "-T",
		"on-outputs-changed", "script",
		"{\nscript {\n    bind \"L+#{d.nr}\" desk -d #{d.nr}\n}\n}",
	};
	static const char *const style[] = {
		"style", "-g", "-n", "foot", "-s", "%foot",
		("{\n    decor {\n        border.width 4\n    }\n"
		 "    layer top\n}"),
	};
	static const char *const script_style[] = {
		"script",
		"{\n"
		"    for o in (outputs) {\n"
		"        style -g -n cp-#{o.name} -s /cowpager:#{o.name}/ {\n"
		"            placement.output #{o.name}\n"
		"            placement.anchor top-right\n"
		"        }\n"
		"        exec cowpager -m #{o.name}\n"
		"    }\n"
		"}",
	};

	check("script{}", 2, script);
	check("script{}nop", 3, adjacent);
	check("rule -g -Ton-map -nonfoot -s /foot/ "
	    "script{ if self.output == \"HDMI-A-1\"{ } }",
	    8, rule);
	check("bind C+c,C+a window-move -x 0", 5, chord);
	check("rule -g -n desk-number-bindings -T on-outputs-changed "
	    "script {\nscript {\n    bind \"L+#{d.nr}\" desk -d #{d.nr}\n}\n}",
	    8, rule_dump);
	check("style -g -n foot -s %foot {\n"
	    "    decor {\n        border.width 4\n    }\n"
	    "    layer top\n}", 7, style);
	check("script {\n"
	    "    for o in (outputs) {\n"
	    "        style -g -n cp-#{o.name} -s /cowpager:#{o.name}/ {\n"
	    "            placement.output #{o.name}\n"
	    "            placement.anchor top-right\n"
	    "        }\n"
	    "        exec cowpager -m #{o.name}\n"
	    "    }\n"
	    "}", 2, script_style);
	check_terminator("script {\n"
	    "    for o in (outputs) {\n"
	    "        style -g -n cp-#{o.name} -s /cowpager:#{o.name}/ {\n"
	    "            placement.output #{o.name}\n"
	    "        }\n"
	    "    }\n",
	    COW_CMD_SCAN_INCOMPLETE);
	check_terminator("script {\n"
	    "    for o in (outputs) {\n"
	    "        style -g -n cp-#{o.name} -s /cowpager:#{o.name}/ {\n"
	    "            placement.output #{o.name}\n"
	    "        }\n"
	    "    }\n"
	    "}\n",
	    COW_CMD_SCAN_COMPLETE);
	return (0);
}
