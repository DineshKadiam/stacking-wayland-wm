/*
 * Copyright (c) 2010 Nicholas Marriott <nicm@users.sourceforge.net>
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

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cow/compat.h"
#include "cow/cmd.h"

/*
 * Manipulate command arguments.
 */

struct args_entry *args_find(struct args *, u_char);

RB_GENERATE(args_tree, args_entry, entry, args_cmp);

/* Arguments tree comparison function. */
int
args_cmp(struct args_entry *a1, struct args_entry *a2)
{
	return (a1->flag - a2->flag);
}

/* Create an arguments set with no flags. */
struct args *
args_create(int argc, ...)
{
	struct args *args;
	va_list	     ap;
	int	     i;

	args = xcalloc(1, sizeof *args);

	args->argc = argc;
	if (argc == 0)
		args->argv = NULL;
	else
		args->argv = xcalloc(argc, sizeof *args->argv);

	va_start(ap, argc);
	for (i = 0; i < argc; i++)
		args->argv[i] = xstrdup(va_arg(ap, char *));
	va_end(ap);

	return (args);
}

/* Find a flag in the arguments tree. */
struct args_entry *
args_find(struct args *args, u_char ch)
{
	struct args_entry entry;

	entry.flag = ch;
	return (RB_FIND(args_tree, &args->tree, &entry));
}

/* Parse an argv and argc into a new argument set. */
struct args *
args_parse(const char *template, int argc, char **argv)
{
	struct args *args;
	const char  *ptr;
	int	     opt;

	args = xcalloc(1, sizeof *args);

	if (template[0] != '\0') {
		/*
		 * Reset getopt's internal state between calls.
		 *
		 * BSDs (and any libc with optreset) want optind=1 plus
		 * optreset=1 -- optind=1 skips argv[0] (which we pass as
		 * the command name), optreset forces a fresh scan.
		 *
		 * glibc has no optreset.  Its documented reset marker is
		 * optind=0 (which triggers a full state reset and also
		 * re-reads argv[0]/POSIXLY_CORRECT); optind=1 does NOT
		 * reset state, so repeated calls see stale flags and
		 * misparse argv, producing the 'keybinding only fires
		 * every few presses' symptom.
		 */
#if HAVE_OPTRESET
		optind	 = 1;
		optreset = 1;
#else
		optind = 0;
#endif

		while ((opt = getopt(argc, argv, template)) != -1) {
			if (opt < 0)
				continue;
			if (opt == '?' ||
			    (ptr = strchr(template, opt)) == NULL) {
				args_free(args);
				return (NULL);
			}
			/* musl leaves optarg unchanged for argument-free flags.
			 * It may still point into a previous command's freed argv.
			 */
			args_set(args, opt, ptr[1] == ':' ? optarg : NULL);
		}
		argc -= optind;
		argv += optind;
	} else {
		/* Empty template: skip getopt, all args are positional.
		 * argv[0] is the command name, skip it. */
		argc--;
		argv++;
	}

	args->argc = argc;
	args->argv = cmd_copy_argv(argc, argv);

	return (args);
}

/* Free an arguments set. */
void
args_free(struct args *args)
{
	struct args_entry *entry;
	struct args_entry *entry1;

	cmd_free_argv(args->argc, args->argv);

	RB_FOREACH_SAFE (entry, args_tree, &args->tree, entry1) {
		RB_REMOVE(args_tree, &args->tree, entry);
		xfree(entry->value);
		xfree(entry);
	}

	xfree(args);
}

/* Print a set of arguments. */
size_t
args_print(struct args *args, char *buf, size_t len)
{
	size_t		   off;
	int		   i;
	const char	  *quotes;
	struct args_entry *entry;

	/* Return the number of bytes stored, excluding the terminator. */
	if (len == 0)
		return (0);
	buf[0] = '\0';
	if (len == 1)
		return (0);
	off = 0;

	/* Process the flags first. */
	buf[off++] = '-';
	RB_FOREACH (entry, args_tree, &args->tree) {
		if (entry->value != NULL)
			continue;

		if (off == len - 1) {
			buf[off] = '\0';
			return (len - 1);
		}
		buf[off++] = entry->flag;
		buf[off]   = '\0';
	}
	if (off == 1)
		buf[--off] = '\0';

	/* Then the flags with arguments. */
	RB_FOREACH (entry, args_tree, &args->tree) {
		if (entry->value == NULL)
			continue;

		if (off >= len) {
			/* snprintf will have zero terminated. */
			return (len - 1);
		}

		if (strchr(entry->value, ' ') != NULL)
			quotes = "\"";
		else
			quotes = "";
		off += snprintf(buf + off, len - off, "%s-%c %s%s%s",
		    off != 0 ? " " : "", entry->flag, quotes, entry->value,
		    quotes);
	}

	/* And finally the argument vector. */
	for (i = 0; i < args->argc; i++) {
		if (off >= len) {
			/* snprintf will have zero terminated. */
			return (len - 1);
		}

		if (strchr(args->argv[i], ' ') != NULL)
			quotes = "\"";
		else
			quotes = "";
		off += snprintf(buf + off, len - off, "%s%s%s%s",
		    off != 0 ? " " : "", quotes, args->argv[i], quotes);
	}

	return (off < len ? off : len - 1);
}

/* Return if an argument is present. */
int
args_has(struct args *args, u_char ch)
{
	return (args_find(args, ch) == NULL ? 0 : 1);
}

/* Set argument value in the arguments tree. */
void
args_set(struct args *args, u_char ch, const char *value)
{
	struct args_entry *entry;

	/* Replace existing argument. */
	if ((entry = args_find(args, ch)) != NULL) {
		xfree(entry->value);
		if (value != NULL)
			entry->value = xstrdup(value);
		else
			entry->value = NULL;
		return;
	}

	entry	    = xcalloc(1, sizeof *entry);
	entry->flag = ch;
	if (value != NULL)
		entry->value = xstrdup(value);

	RB_INSERT(args_tree, &args->tree, entry);
}

/* Get argument value. Will be NULL if it isn't present. */
char *
args_get(struct args *args, u_char ch)
{
	struct args_entry *entry;

	if ((entry = args_find(args, ch)) == NULL)
		return (NULL);
	return (entry->value);
}

/* Convert an argument value to a number. */
long long
args_strtonum(struct args *args, u_char ch, long long minval, long long maxval,
    char **cause)
{
	const char	  *errstr;
	long long	   ll;
	struct args_entry *entry;

	if ((entry = args_find(args, ch)) == NULL) {
		*cause = xstrdup("missing");
		return (0);
	}

	ll = strtonum(entry->value, minval, maxval, &errstr);
	if (errstr != NULL) {
		*cause = xstrdup(errstr);
		return (0);
	}

	*cause = NULL;
	return (ll);
}
