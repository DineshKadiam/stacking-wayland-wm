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
 * DSL runtime value implementation.
 *
 * Reference-counted tagged union.  Lists and records own their
 * contained values via the ref/unref pair.  Strings are owned
 * heap pointers; numbers and bools are by-value.
 *
 * Containers grow geometrically (cap doubles, starting at 4) so a
 * snapshot of `windows` over ~hundreds of views is amortised O(N).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cow/compat.h"
#include "cow/memory.h"

#include "dsl-value.h"

static struct dsl_value *
value_new(enum dsl_value_type t)
{
	struct dsl_value *v = xcalloc(1, sizeof(*v));
	v->type		    = t;
	v->refcount	    = 1;
	return (v);
}

struct dsl_value *
dsl_value_nil(void)
{
	return (value_new(DSL_V_NIL));
}

struct dsl_value *
dsl_value_bool(bool b)
{
	struct dsl_value *v = value_new(DSL_V_BOOL);
	v->u.b		    = b;
	return (v);
}

struct dsl_value *
dsl_value_number(double n)
{
	struct dsl_value *v = value_new(DSL_V_NUMBER);
	v->u.n		    = n;
	return (v);
}

struct dsl_value *
dsl_value_string(const char *s)
{
	struct dsl_value *v = value_new(DSL_V_STRING);
	v->u.s		    = xstrdup(s == NULL ? "" : s);
	return (v);
}

struct dsl_value *
dsl_value_string_take(char *s)
{
	struct dsl_value *v = value_new(DSL_V_STRING);
	v->u.s		    = (s != NULL) ? s : xstrdup("");
	return (v);
}

struct dsl_value *
dsl_value_list(void)
{
	return (value_new(DSL_V_LIST));
}

struct dsl_value *
dsl_value_record(void)
{
	return (value_new(DSL_V_RECORD));
}

struct dsl_value *
dsl_value_ref(struct dsl_value *v)
{
	if (v != NULL)
		v->refcount++;
	return (v);
}

void
dsl_value_unref(struct dsl_value *v)
{
	if (v == NULL)
		return;
	if (--v->refcount > 0)
		return;

	switch (v->type) {
	case DSL_V_STRING:
		xfree(v->u.s);
		break;
	case DSL_V_LIST:
		for (size_t i = 0; i < v->u.list.n; i++)
			dsl_value_unref(v->u.list.items[i]);
		xfree(v->u.list.items);
		break;
	case DSL_V_RECORD:
		for (size_t i = 0; i < v->u.record.n; i++) {
			xfree(v->u.record.entries[i].key);
			dsl_value_unref(v->u.record.entries[i].val);
		}
		xfree(v->u.record.entries);
		break;
	case DSL_V_NIL:
	case DSL_V_BOOL:
	case DSL_V_NUMBER:
		break;
	}
	xfree(v);
}

static void
list_grow(struct dsl_value *list)
{
	size_t ncap	   = list->u.list.cap ? list->u.list.cap * 2 : 4;
	list->u.list.items = xrealloc(list->u.list.items, ncap,
	    sizeof(*list->u.list.items));
	list->u.list.cap   = ncap;
}

void
dsl_list_append(struct dsl_value *list, struct dsl_value *item)
{
	if (list->u.list.n == list->u.list.cap)
		list_grow(list);
	list->u.list.items[list->u.list.n++] = dsl_value_ref(item);
}

static struct dsl_record_entry *
record_find(const struct dsl_value *record, const char *key)
{
	for (size_t i = 0; i < record->u.record.n; i++) {
		if (strcmp(record->u.record.entries[i].key, key) == 0)
			return (
			    &((struct dsl_value *)record)->u.record.entries[i]);
	}
	return (NULL);
}

static void
record_grow(struct dsl_value *record)
{
	size_t ncap = record->u.record.cap ? record->u.record.cap * 2 : 4;
	record->u.record.entries = xrealloc(record->u.record.entries, ncap,
	    sizeof(*record->u.record.entries));
	record->u.record.cap	 = ncap;
}

void
dsl_record_set(struct dsl_value *record, const char *key, struct dsl_value *val)
{
	struct dsl_record_entry *e = record_find(record, key);
	if (e != NULL) {
		dsl_value_unref(e->val);
		e->val = dsl_value_ref(val);
		return;
	}
	if (record->u.record.n == record->u.record.cap)
		record_grow(record);
	struct dsl_record_entry *slot =
	    &record->u.record.entries[record->u.record.n++];
	slot->key = xstrdup(key);
	slot->val = dsl_value_ref(val);
}

struct dsl_value *
dsl_record_get(const struct dsl_value *record, const char *key)
{
	struct dsl_record_entry *e = record_find(record, key);
	return ((e != NULL) ? e->val : NULL);
}

bool
dsl_value_truthy(const struct dsl_value *v)
{
	if (v == NULL)
		return (false);
	switch (v->type) {
	case DSL_V_NIL:
		return (false);
	case DSL_V_BOOL:
		return (v->u.b);
	case DSL_V_NUMBER:
		return (v->u.n != 0.0);
	case DSL_V_STRING:
		return (v->u.s != NULL && v->u.s[0] != '\0');
	case DSL_V_LIST:
		return (v->u.list.n > 0);
	case DSL_V_RECORD:
		return (true);
	}
	return (false);
}

char *
dsl_value_to_argv_string(const struct dsl_value *v)
{
	if (v == NULL)
		return (NULL);
	switch (v->type) {
	case DSL_V_STRING:
		return (xstrdup(v->u.s != NULL ? v->u.s : ""));
	case DSL_V_NUMBER: {
		char buf[64];
		/* Print as integer when the value is integral and fits
		 * cleanly, otherwise shortest %g. */
		double n = v->u.n;
		if (n == (long long)n && n >= -1e18 && n <= 1e18)
			snprintf(buf, sizeof(buf), "%lld", (long long)n);
		else
			snprintf(buf, sizeof(buf), "%.17g", n);
		return (xstrdup(buf));
	}
	case DSL_V_BOOL:
		return (xstrdup(v->u.b ? "true" : "false"));
	case DSL_V_NIL:
	case DSL_V_LIST:
	case DSL_V_RECORD:
		return (NULL);
	}
	return (NULL);
}
