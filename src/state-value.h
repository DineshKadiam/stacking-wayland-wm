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

#if !defined(COW_STATE_VALUE_H)
#define COW_STATE_VALUE_H

#include <stdbool.h>
#include <stddef.h>

/*
 * Shared state value.
 *
 * Tagged union shared between the evaluator and source adapters.
 * Records (per-window/desk/output snapshots) are flat name->value
 * maps; lists are vectors of values; scalars are number / string /
 * bool.  Numbers are doubles to keep parsing and comparison simple
 * -- the DSL never does real arithmetic.
 *
 * Memory: values are heap-allocated and reference-counted.  Lists
 * and records own their contained values via state_value_ref/_unref.
 */

enum state_value_type {
	STATE_V_NIL = 0,
	STATE_V_BOOL,
	STATE_V_NUMBER,
	STATE_V_STRING,
	STATE_V_LIST,
	STATE_V_RECORD,
};

struct state_value;
struct cJSON;

struct state_record_entry {
	char		 *key;
	struct state_value *val;
};

struct state_value {
	enum state_value_type type;
	unsigned	    refcount;
	union {
		bool   b;
		double n;
		char  *s;
		struct {
			struct state_value **items;
			size_t		   n;
			size_t		   cap;
		} list;
		struct {
			struct state_record_entry *entries;
			size_t			 n;
			size_t			 cap;
		} record;
	} u;
};

/*
 * Constructors.  Each returns a fresh ref (refcount == 1).
 */
struct state_value *state_value_nil(void);
struct state_value *state_value_bool(bool);
struct state_value *state_value_number(double);
struct state_value *state_value_string(const char *); /* copies */
struct state_value *state_value_string_take(char *);  /* takes  */
struct state_value *state_value_list(void);
struct state_value *state_value_record(void);

/*
 * Reference counting.
 */
struct state_value *state_value_ref(struct state_value *);
void		  state_value_unref(struct state_value *);

/*
 * Container operations.  Each takes a ref on the supplied child
 * value; the caller still owns its own ref and should unref it when
 * done if it doesn't need it any longer.
 */
void state_list_append(struct state_value *list, struct state_value *item);
void state_record_set(struct state_value *record, const char *key,
    struct state_value *val);

/*
 * Lookup.  Returns a borrowed pointer (no ref bump); NULL on miss.
 */
struct state_value *state_record_get(const struct state_value *record,
    const char						*key);

/*
 * Truthiness.  Used by `if` and `where`:
 *   nil     -> false
 *   bool    -> the bool
 *   number  -> non-zero
 *   string  -> non-empty
 *   list    -> non-empty
 *   record  -> always true
 */
bool state_value_truthy(const struct state_value *);

/*
 * Stringify for command argv interpolation.  Number prints with the
 * shortest %.17g representation that round-trips; bool prints
 * "true"/"false"; string prints verbatim; list and record return
 * NULL (caller treats as evaluation error: cannot splice an
 * aggregate into argv).  Caller owns the returned string (xfree).
 */
char *state_value_to_argv_string(const struct state_value *);

/* Convert a state value recursively to an owned cJSON value. */
struct cJSON *state_value_to_json(const struct state_value *);
struct state_value *state_value_from_json(const struct cJSON *);

#endif /* COW_STATE_VALUE_H */
