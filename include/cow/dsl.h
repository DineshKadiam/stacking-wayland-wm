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

#if !defined(COW_DSL_H)
#define COW_DSL_H

#include <stdbool.h>

/*
 * CoW DSL public surface.
 *
 * `script { ... }` blocks parse into an opaque AST and evaluate
 * against a cmd_q. Command atoms enqueue cmd_lists that the normal
 * cmdq_continue() walk drains as usual.
 */

struct cmd_q;
struct cow_view;
struct dsl_ast;
struct state_value;

/* Parse / eval error carrier. */
struct dsl_error {
	char *file; /* may be NULL */
	int   line, col;
	char *message; /* xmalloced; caller frees */
};

void dsl_error_fini(struct dsl_error *);

/*
 * Parse `src` as a DSL `script { ... }` block.  On success returns
 * a heap-allocated AST owned by the caller; free with dsl_free().
 * On failure returns NULL and writes a description into `err`
 * (message field is xmalloced).
 */
struct dsl_ast *dsl_parse(const char *src, const char *file,
    struct dsl_error *err);

/*
 * Release an AST returned by dsl_parse().  Safe to call with NULL.
 */
void dsl_free(struct dsl_ast *);

/*
 * Evaluate `ast` against `cmdq`.  Any commands produced by the
 * script are appended to the cmdq's queue and run by the existing
 * cmdq_continue() loop.  Returns false and populates `err` on
 * evaluation failure (e.g. unknown property, type mismatch).
 */
bool dsl_eval(struct dsl_ast *ast, struct cmd_q *cmdq, struct dsl_error *err);

/*
 * Evaluate an expression AST with "self" bound to a window snapshot.
 * Writes the expression's truthiness to out on success.
 */
bool dsl_eval_view_expr(struct dsl_ast *ast, const struct cow_view *view,
    bool *out, struct dsl_error *err);

/* Evaluate an expression with "self" bound to a state record. */
bool dsl_eval_record_expr(struct dsl_ast *, const struct state_value *, bool *,
    struct dsl_error *);

#endif /* COW_DSL_H */
