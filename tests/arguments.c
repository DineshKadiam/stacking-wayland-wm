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

#include <string.h>
#include <unistd.h>

/* Exercise musl's unchanged optarg behaviour on every test platform.
 */
static int
stale_getopt(int argc, char *const argv[], const char *options)
{
	char *previous = optarg;
	int ch = getopt(argc, argv, options);
	const char *flag = ch > 0 ? strchr(options, ch) : NULL;
	if (ch == -1 || (flag != NULL && flag[1] != ':'))
		optarg = previous;
	return ch;
}

#define getopt stale_getopt
#include "../src/arguments.c"
#undef getopt

#define CHECK(expr) do { \
	if (!(expr)) { \
		fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); \
		abort(); \
	} \
} while (0)

int
main(void)
{
	char *first[] = { "command", "-t", "previous-window", NULL };
	struct args *args = args_parse("t:", 3, first);
	CHECK(args != NULL);
	CHECK(strcmp(args_get(args, 't'), "previous-window") == 0);
	args_free(args);
	/* A new command and a valued option in the same command both leave
	 * stale optarg behind. Neither may give -s or -S a value.
	 */
	char *commands[][5] = {
		{ "winops", "-Ss", NULL },
		{ "winops", "-t", "current-window", "-sS", NULL },
		{ "winops", "-S", "-s", NULL },
	};
	const int counts[] = { 2, 4, 3 };
	for (size_t i = 0; i < 3; i++) {
		args = args_parse("F:t:crlsiSL:C:", counts[i], commands[i]);
		CHECK(args != NULL);
		CHECK(args_has(args, 's') && args_has(args, 'S'));
		CHECK(args_get(args, 's') == NULL);
		CHECK(args_get(args, 'S') == NULL);
		if (i == 1)
			CHECK(strcmp(args_get(args, 't'), "current-window") == 0);
		args_free(args);
	}
	return 0;
}
