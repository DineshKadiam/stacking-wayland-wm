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

#include "cow/util.h"

static void
check(size_t argc, const char *const *argv, const char *expected)
{
	char *command = cow_command_from_argv(argc, argv);

	if (command == NULL || strcmp(command, expected) != 0) {
		fprintf(stderr, "expected: %s\nactual:   %s\n", expected,
		    command != NULL ? command : "(null)");
		free(command);
		exit(1);
	}
	free(command);
}

int
main(void)
{
	check(0, NULL, "");

	const char *plain[] = { "window-list", "-n", "-p" };
	check(3, plain, "window-list -n -p");

	const char *filter[] = {
		"window-list", "-F", "self.id in [\"abc\"]",
	};
	check(3, filter, "window-list -F \"self.id in [\\\"abc\\\"]\"");

	const char *special[] = { "", "two words", "a\\b", "a\nb", "#id" };
	check(5, special, "\"\" \"two words\" \"a\\\\b\" \"a\\nb\" \"#id\"");

	return (0);
}
