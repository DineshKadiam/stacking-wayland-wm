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
 * CoW DSL grammar (step 1: skeleton).
 *
 * This skeleton only accepts an empty `{ }` block.  Subsequent steps
 * grow the grammar incrementally.  The %define api.pure full + %locations
 * scaffolding is in place now so the lexer doesn't have to be reworked
 * later when the parser sprouts state.
 */

%define api.pure full
%define parse.error verbose
%locations

%parse-param { struct dsl_parse_ctx *ctx }
%parse-param { void *scanner }
%lex-param   { void *scanner }

%code requires {
#include "cow/dsl.h"
struct dsl_parse_ctx;
}

%code provides {
int dsl_yylex(YYSTYPE *yylval_param, YYLTYPE *yylloc_param, void *yyscanner);
void dsl_yyerror(YYLTYPE *llocp, struct dsl_parse_ctx *ctx, void *scanner,
    const char *msg);
}

%code {
#include <stdio.h>
#include <stdlib.h>

#include "cow/attrs.h"

#include "dsl-lexer.h"

/* Bison's pure lex API takes (yylval, yylloc, scanner). flex's
 * bison-bridge yields a yylex prototype expecting that exact shape,
 * so we forward by macro-renaming below. */
#define yylex dsl_yylex
}

%union {
	int dummy;
}

%token LBRACE RBRACE

%%

script
	: LBRACE RBRACE
	;

%%

void
dsl_yyerror(__unused YYLTYPE *llocp, __unused struct dsl_parse_ctx *ctx,
    __unused void *scanner, __unused const char *msg)
{
	/* Real diagnostics surfaced in step 4. */
}
