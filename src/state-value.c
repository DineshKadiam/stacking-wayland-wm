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
 * Shared state value implementation.
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

#include <cJSON.h>

#include "cow/compat.h"
#include "cow/memory.h"

#include "state-value.h"

static struct state_value *
value_new(enum state_value_type t)
{
	struct state_value *v = xcalloc(1, sizeof(*v));
	v->type		    = t;
	v->refcount	    = 1;
	return (v);
}

struct state_value *
state_value_nil(void)
{
	return (value_new(STATE_V_NIL));
}

struct state_value *
state_value_bool(bool b)
{
	struct state_value *v = value_new(STATE_V_BOOL);
	v->u.b		    = b;
	return (v);
}

struct state_value *
state_value_number(double n)
{
	struct state_value *v = value_new(STATE_V_NUMBER);
	v->u.n		    = n;
	return (v);
}

struct state_value *
state_value_string(const char *s)
{
	struct state_value *v = value_new(STATE_V_STRING);
	v->u.s		    = xstrdup(s == NULL ? "" : s);
	return (v);
}

struct state_value *
state_value_string_take(char *s)
{
	struct state_value *v = value_new(STATE_V_STRING);
	v->u.s		    = (s != NULL) ? s : xstrdup("");
	return (v);
}

struct state_value *
state_value_list(void)
{
	return (value_new(STATE_V_LIST));
}

struct state_value *
state_value_record(void)
{
	return (value_new(STATE_V_RECORD));
}

struct state_value *
state_value_ref(struct state_value *v)
{
	if (v != NULL)
		v->refcount++;
	return (v);
}

void
state_value_unref(struct state_value *v)
{
	if (v == NULL)
		return;
	if (--v->refcount > 0)
		return;

	switch (v->type) {
	case STATE_V_STRING:
		xfree(v->u.s);
		break;
	case STATE_V_LIST:
		for (size_t i = 0; i < v->u.list.n; i++)
			state_value_unref(v->u.list.items[i]);
		xfree(v->u.list.items);
		break;
	case STATE_V_RECORD:
		for (size_t i = 0; i < v->u.record.n; i++) {
			xfree(v->u.record.entries[i].key);
			state_value_unref(v->u.record.entries[i].val);
		}
		xfree(v->u.record.entries);
		break;
	case STATE_V_NIL:
	case STATE_V_BOOL:
	case STATE_V_NUMBER:
		break;
	}
	xfree(v);
}

static void
list_grow(struct state_value *list)
{
	size_t ncap	   = list->u.list.cap ? list->u.list.cap * 2 : 4;
	list->u.list.items = xrealloc(list->u.list.items, ncap,
	    sizeof(*list->u.list.items));
	list->u.list.cap   = ncap;
}

void
state_list_append(struct state_value *list, struct state_value *item)
{
	if (list->u.list.n == list->u.list.cap)
		list_grow(list);
	list->u.list.items[list->u.list.n++] = state_value_ref(item);
}

static struct state_record_entry *
record_find(const struct state_value *record, const char *key)
{
	for (size_t i = 0; i < record->u.record.n; i++) {
		if (strcmp(record->u.record.entries[i].key, key) == 0)
			return (
			    &((struct state_value *)record)->u.record.entries[i]);
	}
	return (NULL);
}

static void
record_grow(struct state_value *record)
{
	size_t ncap = record->u.record.cap ? record->u.record.cap * 2 : 4;
	record->u.record.entries = xrealloc(record->u.record.entries, ncap,
	    sizeof(*record->u.record.entries));
	record->u.record.cap	 = ncap;
}

void
state_record_set(struct state_value *record, const char *key,
    struct state_value *val)
{
	struct state_record_entry *e = record_find(record, key);
	if (e != NULL) {
		state_value_unref(e->val);
		e->val = state_value_ref(val);
		return;
	}
	if (record->u.record.n == record->u.record.cap)
		record_grow(record);
	struct state_record_entry *slot =
	    &record->u.record.entries[record->u.record.n++];
	slot->key = xstrdup(key);
	slot->val = state_value_ref(val);
}

struct state_value *
state_record_get(const struct state_value *record, const char *key)
{
	struct state_record_entry *e = record_find(record, key);
	return ((e != NULL) ? e->val : NULL);
}

bool
state_value_truthy(const struct state_value *v)
{
	if (v == NULL)
		return (false);
	switch (v->type) {
	case STATE_V_NIL:
		return (false);
	case STATE_V_BOOL:
		return (v->u.b);
	case STATE_V_NUMBER:
		return (v->u.n != 0.0);
	case STATE_V_STRING:
		return (v->u.s != NULL && v->u.s[0] != '\0');
	case STATE_V_LIST:
		return (v->u.list.n > 0);
	case STATE_V_RECORD:
		return (true);
	}
	return (false);
}

char *
state_value_to_argv_string(const struct state_value *v)
{
	if (v == NULL)
		return (NULL);
	switch (v->type) {
	case STATE_V_STRING:
		return (xstrdup(v->u.s != NULL ? v->u.s : ""));
	case STATE_V_NUMBER: {
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
	case STATE_V_BOOL:
		return (xstrdup(v->u.b ? "true" : "false"));
	case STATE_V_NIL:
	case STATE_V_LIST:
	case STATE_V_RECORD:
		return (NULL);
	}
	return (NULL);
}

struct state_value *
state_value_from_json(const cJSON *json)
{
	if (cJSON_IsString(json))
		return state_value_string(json->valuestring);
	if (cJSON_IsBool(json))
		return state_value_bool(cJSON_IsTrue(json));
	if (cJSON_IsNumber(json))
		return state_value_number(json->valuedouble);
	if (!cJSON_IsArray(json) && !cJSON_IsObject(json))
		return state_value_nil();

	bool array = cJSON_IsArray(json);
	struct state_value *value = array ? state_value_list() : state_value_record();
	const cJSON *child;
	cJSON_ArrayForEach(child, json) {
		struct state_value *item = state_value_from_json(child);
		if (array)
			state_list_append(value, item);
		else
			state_record_set(value, child->string, item);
		state_value_unref(item);
	}
	return value;
}

struct cJSON *
state_value_to_json(const struct state_value *v)
{
	if (v == NULL)
		return (cJSON_CreateNull());

	switch (v->type) {
	case STATE_V_NIL:
		return (cJSON_CreateNull());
	case STATE_V_BOOL:
		return (cJSON_CreateBool(v->u.b));
	case STATE_V_NUMBER:
		return (cJSON_CreateNumber(v->u.n));
	case STATE_V_STRING:
		return (cJSON_CreateString(v->u.s != NULL ? v->u.s : ""));
	case STATE_V_LIST: {
		cJSON *array = cJSON_CreateArray();
		for (size_t i = 0; i < v->u.list.n; i++)
			cJSON_AddItemToArray(array,
			    state_value_to_json(v->u.list.items[i]));
		return (array);
	}
	case STATE_V_RECORD: {
		cJSON *object = cJSON_CreateObject();
		for (size_t i = 0; i < v->u.record.n; i++) {
			const struct state_record_entry *entry =
			    &v->u.record.entries[i];
			cJSON_AddItemToObject(object, entry->key,
			    state_value_to_json(entry->val));
		}
		return (object);
	}
	}
	return (cJSON_CreateNull());
}
