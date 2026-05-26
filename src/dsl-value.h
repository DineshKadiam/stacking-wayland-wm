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

#if !defined(COW_DSL_VALUE_H)
#define COW_DSL_VALUE_H

#include <stdbool.h>
#include <stddef.h>

/*
 * DSL runtime value.
 *
 * Tagged union shared between the evaluator and source adapters.
 * Records (per-window/desk/output snapshots) are flat name->value
 * maps; lists are vectors of values; scalars are number / string /
 * bool.  Numbers are doubles to keep parsing and comparison simple
 * -- the DSL never does real arithmetic.
 *
 * Memory: values are heap-allocated and reference-counted.  Lists
 * and records own their contained values via dsl_value_ref/_unref.
 */

enum dsl_value_type {
	DSL_V_NIL = 0,
	DSL_V_BOOL,
	DSL_V_NUMBER,
	DSL_V_STRING,
	DSL_V_LIST,
	DSL_V_RECORD,
};

struct dsl_value;

struct dsl_record_entry {
	char		 *key;
	struct dsl_value *val;
};

struct dsl_value {
	enum dsl_value_type type;
	unsigned	    refcount;
	union {
		bool   b;
		double n;
		char  *s;
		struct {
			struct dsl_value **items;
			size_t		   n;
			size_t		   cap;
		} list;
		struct {
			struct dsl_record_entry *entries;
			size_t			 n;
			size_t			 cap;
		} record;
	} u;
};

/*
 * Constructors.  Each returns a fresh ref (refcount == 1).
 */
struct dsl_value *dsl_value_nil(void);
struct dsl_value *dsl_value_bool(bool);
struct dsl_value *dsl_value_number(double);
struct dsl_value *dsl_value_string(const char *); /* copies */
struct dsl_value *dsl_value_string_take(char *);  /* takes  */
struct dsl_value *dsl_value_list(void);
struct dsl_value *dsl_value_record(void);

/*
 * Reference counting.
 */
struct dsl_value *dsl_value_ref(struct dsl_value *);
void		  dsl_value_unref(struct dsl_value *);

/*
 * Container operations.  Each takes a ref on the supplied child
 * value; the caller still owns its own ref and should unref it when
 * done if it doesn't need it any longer.
 */
void dsl_list_append(struct dsl_value *list, struct dsl_value *item);
void dsl_record_set(struct dsl_value *record, const char *key,
    struct dsl_value *val);

/*
 * Lookup.  Returns a borrowed pointer (no ref bump); NULL on miss.
 */
struct dsl_value *dsl_record_get(const struct dsl_value *record,
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
bool dsl_value_truthy(const struct dsl_value *);

/*
 * Stringify for command argv interpolation.  Number prints with the
 * shortest %.17g representation that round-trips; bool prints
 * "true"/"false"; string prints verbatim; list and record return
 * NULL (caller treats as evaluation error: cannot splice an
 * aggregate into argv).  Caller owns the returned string (xfree).
 */
char *dsl_value_to_argv_string(const struct dsl_value *);

#endif /* COW_DSL_VALUE_H */
