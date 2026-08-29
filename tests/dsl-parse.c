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
#include "cow/dsl.h"

struct cmd_entry *
cmd_find_cmd(const char *name)
{
	static struct cmd_entry nop = { .name = "nop" };
	static struct cmd_entry bind = { .name = "bind" };

	if (strcmp(name, "nop") == 0)
		return (&nop);
	return (strcmp(name, "bind") == 0 ? &bind : NULL);
}

static void
check_valid(const char *source)
{
	struct dsl_error error = { 0 };
	struct dsl_ast  *ast = dsl_parse(source, NULL, &error);

	if (ast == NULL) {
		fprintf(stderr, "%s: %d:%d: %s\n", source, error.line,
		    error.col,
		    error.message != NULL ? error.message : "parse failed");
		exit(1);
	}
	dsl_free(ast);
	dsl_error_fini(&error);
}

static void
check_invalid(const char *source)
{
	struct dsl_error error = { 0 };
	struct dsl_ast  *ast = dsl_parse(source, NULL, &error);

	if (ast != NULL) {
		fprintf(stderr, "%s: unexpectedly parsed\n", source);
		exit(1);
	}
	dsl_error_fini(&error);
}

int
main(void)
{
	static const char *const valid[] = {
		"{ bind C+c,C+a nop }",
		"{ if self.output == \"HDMI-A-1\"{ nop } }",
		"{ if self.output == \"HDMI-A-1\" { nop } }",
		"{ if self.current_output and self.output == "
		    "\"HDMI-A-1\"{ nop } }",
		"{ if self.title matches \"cowiconman:*\" and "
		    "self.output == \"eDP-1\" { nop } }",
		"{ if self.title not matches \"cowiconman:*\" { nop } }",
		"{ for w in (windows where w.title matches \"cowiconman:*\") "
		    "{ nop } }",
	};

	for (size_t repeat = 0; repeat < 100; repeat++) {
		for (size_t i = 0; i < sizeof valid / sizeof valid[0]; i++)
			check_valid(valid[i]);
		check_invalid("{ if self.output == \"HDMI-A-1\"{");
	}
	return (0);
}
