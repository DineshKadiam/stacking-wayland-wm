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
 * DSL AST allocator and lifecycle.
 *
 * Step 1: trivial -- a single struct that gets stamped during parse
 * and freed at dsl_free().  Step 2 grows this into a tagged-union
 * tree with a bump-arena allocator.
 */

#include <stdlib.h>

#include "cow/memory.h"
#include "dsl-ast.h"

struct dsl_ast *
dsl_ast_new(void)
{
	struct dsl_ast *a = xcalloc(1, sizeof *a);
	return (a);
}

void
dsl_free(struct dsl_ast *a)
{
	xfree(a);
}
