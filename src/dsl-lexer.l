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
 * CoW DSL lexer (step 1: skeleton).
 *
 * This skeleton emits only the open/close braces of the outermost
 * `script { ... }` block.  Subsequent steps fill in the real token
 * set (keywords, identifiers, literals, command-arg mode, #{}
 * interpolation).
 */

%option reentrant
%option bison-bridge
%option bison-locations
%option noyywrap
%option nounput
%option noinput
%option yylineno

%{
#include <string.h>

#include "cow/dsl.h"
#include "dsl-parser.h"

#define YY_USER_ACTION                                                         \
	do {                                                                   \
		yylloc->first_line   = yylineno;                               \
		yylloc->first_column = 0;                                      \
		yylloc->last_line    = yylineno;                               \
		yylloc->last_column  = 0;                                      \
	} while (0);
%}

%%

"{"                 { return LBRACE; }
"}"                 { return RBRACE; }
[ \t\r\n]+          { /* whitespace */ }
.                   { /* swallow anything else for now */ }

%%
