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
 * DSL parser entry points.
 */

#include <stdlib.h>

#include "cow/dsl.h"
#include "cow/memory.h"

#include "dsl-ast.h"
#include "dsl-parser.h"
#include "dsl-lexer.h"

void
dsl_error_fini(struct dsl_error *error)
{
	if (error == NULL)
		return;
	xfree(error->file);
	xfree(error->message);
	error->file = NULL;
	error->message = NULL;
}

struct dsl_ast *
dsl_parse(const char *src, const char *file, struct dsl_error *err)
{
	if (src == NULL) {
		if (err != NULL) {
			err->file    = NULL;
			err->line    = 0;
			err->col     = 0;
			err->message = xstrdup("null source");
		}
		return (NULL);
	}

	struct dsl_lexer_ctx lexer = { .col = 1 };
	yyscan_t scanner;
	if (dsl_yylex_init_extra(&lexer, &scanner) != 0) {
		if (err != NULL) {
			err->file    = NULL;
			err->line    = 0;
			err->col     = 0;
			err->message = xstrdup("lexer init failed");
		}
		return (NULL);
	}

	YY_BUFFER_STATE buf = dsl_yy_scan_string(src, scanner);
	dsl_yyset_lineno(1, scanner);

	struct dsl_parse_ctx ctx = { 0 };
	ctx.err			 = err;
	ctx.file		 = file;
	ctx.ast			 = dsl_ast_new();

	int rc = dsl_yyparse(&ctx, scanner);

	dsl_yy_delete_buffer(buf, scanner);
	dsl_yylex_destroy(scanner);
	xfree(lexer.strbuf);

	if (rc != 0) {
		dsl_free(ctx.ast);
		return (NULL);
	}

	return (ctx.ast);
}
