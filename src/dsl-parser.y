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
 * CoW DSL grammar.
 *
 * Productions follow the locked design: script blocks contain
 * statements (let / for / if / command); expressions form a small
 * tree with comparisons + boolean ops; sources are one of windows /
 * containers / panes / desks / outputs / rules / selections.
 */

%define api.pure full
%define parse.error verbose
%locations

%parse-param { struct dsl_parse_ctx *ctx }
%parse-param { void *scanner }
%lex-param   { void *scanner }

/* Bison maintains this counter for error recovery, but the pure parser API
 * does not otherwise expose it to this grammar.  Referencing it here avoids
 * an unused-but-set warning from generated parsers with newer GCC versions. */
%initial-action
{
	(void)yynerrs;
}

%code requires {
#include "cow/dsl.h"
#include "dsl-ast.h"
}

%code provides {
int dsl_yylex(YYSTYPE *yylval_param, YYLTYPE *yylloc_param, void *yyscanner);
void dsl_yyerror(YYLTYPE *llocp, struct dsl_parse_ctx *ctx, void *scanner,
    const char *msg);
}

%code {
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cow/cmd.h"
#include "cow/attrs.h"
#include "cow/memory.h"

#include "dsl-lexer.h"

/* Bison's pure lex API takes (yylval, yylloc, scanner). */
#define yylex dsl_yylex

static void
record_error(struct dsl_parse_ctx *ctx, YYLTYPE *loc, const char *msg)
{
	if (ctx == NULL || ctx->err == NULL)
		return;
	if (ctx->err->message != NULL)
		return; /* keep first error */
	ctx->err->file	  = (ctx->file != NULL) ? xstrdup(ctx->file) : NULL;
	ctx->err->line	  = (loc != NULL) ? loc->first_line : 0;
	ctx->err->col	  = (loc != NULL) ? loc->first_column : 0;
	ctx->err->message = xstrdup(msg);
}
}

%code requires {
struct dsl_source_spec {
	enum dsl_source	 src;
	struct dsl_node *where;	/* NULL = no where clause */
};
}

%union {
	struct dsl_node	      *node;
	char		      *str;
	double		       num;
	bool		       boolean;
	enum dsl_cmp_op	       cmp;
	enum dsl_source	       src;
	struct dsl_source_spec source_spec;
}

%token LBRACE RBRACE LBRACKET RBRACKET LPAREN RPAREN
%token COMMA DOT EQUALS
%token LET FOR IN IF ELSE BREAK CONTINUE WHERE AND OR NOT MATCHES
%token SELF
%token TRUE_LIT FALSE_LIT NIL_LIT
%token <src> SOURCE_NAME
%token <str> IDENT STRING_LIT RAW_STRING RAW_CONT CMD_VERB
%token <num> NUMBER_LIT
%token <cmp> CMP_OP
%token INTERP_BEGIN INTERP_CONT_BEGIN INTERP_END
%token STR_BEGIN STR_CONT_BEGIN STR_END
%token BODY_BEGIN BODY_CONT_BEGIN BODY_END
%token NEWLINE

%type <node> script stmt_list stmt let_stmt for_stmt if_stmt control_stmt
%type <node> cmd_stmt block
%type <node> expr or_expr and_expr not_expr rel_expr atom path list_literal
%type <node> list_items
%type <source_spec> source

%destructor { xfree($$); } <str>

%%

script
	: LBRACE stmt_list RBRACE
		{
			ctx->ast->root = $2;
		}
	| expr
		{
			ctx->ast->root = $1;
		}
	;

stmt_list
	: %empty
		{
			$$ = dsl_node_block(ctx->ast, @$.first_line,
			    @$.first_column);
		}
	| stmt_list stmt
		{
			$$ = $1;
			dsl_block_append(ctx->ast, $$, $2);
		}
	;

stmt
	: let_stmt
	| for_stmt
	| if_stmt
	| control_stmt
	| cmd_stmt
	;

let_stmt
	: LET IDENT EQUALS expr
		{
			$$ = dsl_node_let(ctx->ast, @1.first_line,
			    @1.first_column, $2, $4);
			xfree($2);
		}
	;

for_stmt
	: FOR IDENT IN source block
		{
			$$ = dsl_node_for(ctx->ast, @1.first_line,
			    @1.first_column, $2, $4.src, $4.where, $5);
			xfree($2);
		}
	;

source
	: LPAREN SOURCE_NAME RPAREN
		{
			$$.src	 = $2;
			$$.where = NULL;
		}
	| LPAREN SOURCE_NAME WHERE expr RPAREN
		{
			$$.src	 = $2;
			$$.where = $4;
		}
	;

block
	: LBRACE stmt_list RBRACE
		{
			$$ = $2;
		}
	;

if_stmt
	: IF expr block
		{
			$$ = dsl_node_if(ctx->ast, @1.first_line,
			    @1.first_column, $2, $3, NULL);
		}
	| IF expr block ELSE block
		{
			$$ = dsl_node_if(ctx->ast, @1.first_line,
			    @1.first_column, $2, $3, $5);
		}
	| IF expr block ELSE if_stmt
		{
			$$ = dsl_node_if(ctx->ast, @1.first_line,
			    @1.first_column, $2, $3, $5);
		}
	;

control_stmt
	: BREAK
		{
			$$ = dsl_node_break(ctx->ast, @1.first_line,
			    @1.first_column);
		}
	| CONTINUE
		{
			$$ = dsl_node_continue(ctx->ast, @1.first_line,
			    @1.first_column);
		}
	;

cmd_stmt
	: CMD_VERB
		{
			$$ = dsl_node_cmd(ctx->ast, @1.first_line,
			    @1.first_column, $1);
			xfree($1);
			ctx->current_cmd = $$;
		}
	| cmd_stmt RAW_STRING
		{
			$$ = $1;
			dsl_cmd_append_raw(ctx->ast, $$, $2);
			xfree($2);
		}
	| cmd_stmt RAW_CONT
		{
			$$ = $1;
			dsl_cmd_str_push_raw(ctx->ast, $$, $2);
			xfree($2);
		}
	| cmd_stmt INTERP_BEGIN expr INTERP_END
		{
			$$ = $1;
			dsl_cmd_append_interp(ctx->ast, $$, $3);
		}
	| cmd_stmt INTERP_CONT_BEGIN expr INTERP_END
		{
			$$ = $1;
			dsl_cmd_str_push_interp(ctx->ast, $$, $3);
		}
	| cmd_stmt STR_BEGIN
		{
			dsl_cmd_begin_str_arg(ctx->ast, $1);
		}
	  str_pieces STR_END
		{
			$$ = $1;
		}
	| cmd_stmt STR_CONT_BEGIN str_pieces STR_END
		{
			$$ = $1;
		}
	| cmd_stmt BODY_BEGIN
		{
			dsl_cmd_begin_str_arg(ctx->ast, $1);
			dsl_cmd_str_push_raw(ctx->ast, $1, "{");
		}
	  str_pieces BODY_END
		{
			$$ = $1;
			dsl_cmd_str_push_raw(ctx->ast, $$, "}");
		}
	| cmd_stmt BODY_CONT_BEGIN
		{
			dsl_cmd_str_push_raw(ctx->ast, $1, "{");
		}
	  str_pieces BODY_END
		{
			$$ = $1;
			dsl_cmd_str_push_raw(ctx->ast, $$, "}");
		}
	;

str_pieces
	: %empty
	| str_pieces str_piece
	;

str_piece
	: RAW_STRING
		{
			if (ctx->current_cmd != NULL)
				dsl_cmd_str_push_raw(ctx->ast,
				    ctx->current_cmd, $1);
			xfree($1);
		}
	| INTERP_BEGIN expr INTERP_END
		{
			if (ctx->current_cmd != NULL)
				dsl_cmd_str_push_interp(ctx->ast,
				    ctx->current_cmd, $2);
		}
	;

expr
	: or_expr
	;

or_expr
	: and_expr
	| or_expr OR and_expr
		{
			$$ = dsl_node_or(ctx->ast, @1.first_line,
			    @1.first_column, $1, $3);
		}
	;

and_expr
	: not_expr
	| and_expr AND not_expr
		{
			$$ = dsl_node_and(ctx->ast, @1.first_line,
			    @1.first_column, $1, $3);
		}
	;

not_expr
	: rel_expr
	| NOT not_expr
		{
			$$ = dsl_node_not(ctx->ast, @1.first_line,
			    @1.first_column, $2);
		}
	;

rel_expr
	: atom
	| rel_expr CMP_OP atom
		{
			$$ = dsl_node_cmp(ctx->ast, @1.first_line,
			    @1.first_column, $2, $1, $3);
		}
	| rel_expr IN atom
		{
			$$ = dsl_node_in(ctx->ast, @1.first_line,
			    @1.first_column, $1, $3, false);
		}
	| rel_expr NOT IN atom
		{
			$$ = dsl_node_in(ctx->ast, @1.first_line,
			    @1.first_column, $1, $4, true);
		}
	| rel_expr MATCHES atom
		{
			$$ = dsl_node_cmp(ctx->ast, @1.first_line,
			    @1.first_column, DSL_CMP_MATCH, $1, $3);
		}
	| rel_expr NOT MATCHES atom
		{
			$$ = dsl_node_cmp(ctx->ast, @1.first_line,
			    @1.first_column, DSL_CMP_NOT_MATCH, $1, $4);
		}
	;

atom
	: path
	| list_literal
	| NUMBER_LIT
		{
			$$ = dsl_node_num(ctx->ast, @1.first_line,
			    @1.first_column, $1);
		}
	| STRING_LIT
		{
			$$ = dsl_node_str(ctx->ast, @1.first_line,
			    @1.first_column, $1);
			xfree($1);
		}
	| TRUE_LIT
		{
			$$ = dsl_node_bool(ctx->ast, @1.first_line,
			    @1.first_column, true);
		}
	| FALSE_LIT
		{
			$$ = dsl_node_bool(ctx->ast, @1.first_line,
			    @1.first_column, false);
		}
	| NIL_LIT
		{
			$$ = dsl_node_nil(ctx->ast, @1.first_line,
			    @1.first_column);
		}
	| LPAREN expr RPAREN
		{
			$$ = $2;
		}
	;

list_literal
	: LBRACKET list_items RBRACKET
		{
			$$ = $2;
		}
	;

list_items
	: %empty
		{
			$$ = dsl_node_list(ctx->ast, @$.first_line,
			    @$.first_column);
		}
	| expr
		{
			$$ = dsl_node_list(ctx->ast, @1.first_line,
			    @1.first_column);
			dsl_list_append_node(ctx->ast, $$, $1);
		}
	| list_items COMMA expr
		{
			$$ = $1;
			dsl_list_append_node(ctx->ast, $$, $3);
		}
	;

path
	: IDENT
		{
			$$ = dsl_node_path(ctx->ast, @1.first_line,
			    @1.first_column, $1);
			xfree($1);
		}
	| SELF
		{
			$$ = dsl_node_path(ctx->ast, @1.first_line,
			    @1.first_column, "self");
		}
	| path DOT IDENT
		{
			$$ = $1;
			dsl_path_append(ctx->ast, $$, $3);
			xfree($3);
		}
	;

%%

void
dsl_yyerror(YYLTYPE *llocp, struct dsl_parse_ctx *ctx, __unused void *scanner,
    const char *msg)
{
	record_error(ctx, llocp, msg);
}
