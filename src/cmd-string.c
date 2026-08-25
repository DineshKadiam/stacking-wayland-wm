/*
 * Copyright (c) 2008 Nicholas Marriott <nicm@users.sourceforge.net>
 * Copyright (c) 2026 Thomas Adam <thomas@xteddy.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF MIND, USE, DATA OR PROFITS, WHETHER
 * IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING
 * OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/types.h>

#include <errno.h>
#include <pwd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cow/cmd.h"
#include "cow/cmd_scan.h"

/*
 * Parse a command from a string.
 */

int   cmd_string_getc(const char *, size_t *);
void  cmd_string_ungetc(size_t *);
void  cmd_string_copy(char **, char *, size_t *);
char *cmd_string_string(const char *, size_t *, char, int);
static void cmd_string_add_arg(char ***, int *, char **, size_t *);

static void
cmd_string_add_arg(char ***argv, int *argc, char **buf, size_t *len)
{
	if (*buf == NULL)
		return;

	*buf = xrealloc(*buf, 1, *len + 1);
	(*buf)[*len] = '\0';

	*argv = xrealloc(*argv, *argc + 1, sizeof **argv);
	(*argv)[(*argc)++] = *buf;

	*buf = NULL;
	*len = 0;
}

int
cmd_string_getc(const char *s, size_t *p)
{
	const u_char *ucs = (const u_char *)s;

	if (ucs[*p] == '\0')
		return (EOF);
	return (ucs[(*p)++]);
}

void
cmd_string_ungetc(size_t *p)
{
	(*p)--;
}

/*
 * Parse command string. Returns -1 on error. If returning -1, cause is error
 * string, or NULL for empty command.
 */
int
cmd_string_parse(const char *s, struct cmd_list **cmdlist, const char *file,
    u_int line, char **cause)
{
	size_t p;
	int    ch, i, argc, rval;
	char **argv, *buf, *t;
	size_t len;

	argv = NULL;
	argc = 0;

	buf = NULL;
	len = 0;

	*cause = NULL;

	*cmdlist = NULL;
	rval	 = -1;

	p = 0;
	for (;;) {
		ch = cmd_string_getc(s, &p);
		switch (ch) {
		case '\'':
			if ((t = cmd_string_string(s, &p, '\'', 0)) == NULL)
				goto error;
			cmd_string_copy(&buf, t, &len);
			break;
		case '"':
			if ((t = cmd_string_string(s, &p, '"', 1)) == NULL)
				goto error;
			cmd_string_copy(&buf, t, &len);
			break;
		case '{': {
			size_t start = p - 1, end;

			/*
			 * A brace group starts a new argument even when it is
			 * adjacent to the preceding command name.
			 */
			cmd_string_add_arg(&argv, &argc, &buf, &len);
			if (cow_cmd_find_matching_brace(s + start,
				strlen(s + start), &end) !=
			    COW_CMD_SCAN_COMPLETE)
				goto error;
			buf = xrealloc(buf, 1, len + end + 1);
			memcpy(buf + len, s + start, end + 1);
			len += end + 1;
			p = start + end + 1;
			cmd_string_add_arg(&argv, &argc, &buf, &len);
			break;
		}
		case '#':
			/* Comment: discard rest of line. */
			while ((ch = cmd_string_getc(s, &p)) != EOF)
				;
			/* FALLTHROUGH */
		case EOF:
		case ' ':
		case '\t':
			cmd_string_add_arg(&argv, &argc, &buf, &len);

			if (ch != EOF)
				break;

			if (argc == 0)
				goto out;

			*cmdlist = cmd_list_parse(argc, argv, file, line,
			    cause);
			if (*cmdlist == NULL)
				goto out;

			rval = 0;
			goto out;
		default:
			if (len >= SIZE_MAX - 2)
				goto error;

			buf	   = xrealloc(buf, 1, len + 1);
			buf[len++] = ch;
			break;
		}
	}

error:
	xasprintf(cause, "invalid or unknown command: %s", s);

out:
	xfree(buf);

	if (argv != NULL) {
		for (i = 0; i < argc; i++)
			xfree(argv[i]);
		xfree(argv);
	}

	return (rval);
}

void
cmd_string_copy(char **dst, char *src, size_t *len)
{
	size_t srclen;

	srclen = strlen(src);

	*dst = xrealloc(*dst, 1, *len + srclen + 1);
	strlcpy(*dst + *len, src, srclen + 1);

	*len += srclen;
	xfree(src);
}

char *
cmd_string_string(const char *s, size_t *p, char endch, int esc)
{
	int    ch;
	char  *buf;
	size_t len;

	buf = NULL;
	len = 0;

	while ((ch = cmd_string_getc(s, p)) != endch) {
		switch (ch) {
		case EOF:
			goto error;
		case '\\':
			if (!esc)
				break;
			switch (ch = cmd_string_getc(s, p)) {
			case EOF:
				goto error;
			case 'r':
				ch = '\r';
				break;
			case 'n':
				ch = '\n';
				break;
			case 't':
				ch = '\t';
				break;
			}
			break;
		}

		if (len >= SIZE_MAX - 2)
			goto error;
		buf	   = xrealloc(buf, 1, len + 1);
		buf[len++] = ch;
	}

	buf	 = xrealloc(buf, 1, len + 1);
	buf[len] = '\0';
	return (buf);

error:
	xfree(buf);
	return (NULL);
}
