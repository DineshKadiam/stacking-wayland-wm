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

/* CoW DSL lexer. */

%option reentrant
%option bison-bridge
%option bison-locations
%option noyywrap
%option nounput
%option noinput
%option yylineno
%option stack
%option extra-type="struct dsl_lexer_ctx *"

%{
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cow/cmd.h"
#include "cow/compat.h"
#include "cow/dsl.h"
#include "cow/memory.h"

#include "dsl-ast.h"
#include "dsl-parser.h"

/* flex tracks lines, not columns. */
#define YY_USER_ACTION                                                         \
	do {                                                                   \
		yylloc->first_line   = yylineno;                               \
		yylloc->first_column = yyextra->col;                            \
		yylloc->last_line    = yylineno;                               \
		yylloc->last_column  = yyextra->col + (int)yyleng;              \
		yyextra->col += (int)yyleng;                                    \
	} while (0);

static void
strbuf_reset(struct dsl_lexer_ctx *lexer)
{
	lexer->strbuf_len = 0;
	if (lexer->strbuf_cap == 0) {
		lexer->strbuf_cap = 32;
		lexer->strbuf = xmalloc(lexer->strbuf_cap);
	}
	lexer->strbuf[0] = '\0';
}

static void
strbuf_putc(struct dsl_lexer_ctx *lexer, int c)
{
	if (lexer->strbuf_len + 2 > lexer->strbuf_cap) {
		lexer->strbuf_cap *= 2;
		lexer->strbuf = xrealloc(lexer->strbuf,
		    lexer->strbuf_cap, 1);
	}
	lexer->strbuf[lexer->strbuf_len++] = (char)c;
	lexer->strbuf[lexer->strbuf_len] = '\0';
}

static char *
strbuf_take(struct dsl_lexer_ctx *lexer)
{
	char *out = xstrdup(lexer->strbuf != NULL ? lexer->strbuf : "");
	lexer->strbuf_len = 0;
	if (lexer->strbuf != NULL)
		lexer->strbuf[0] = '\0';
	return (out);
}

/* Classify a bare word and flag command-argument mode. */
static int
classify_word(const char *word, YYSTYPE *lval, bool *push_cmd_arg)
{
	*push_cmd_arg = false;

	if (strcmp(word, "let") == 0)
		return (LET);
	if (strcmp(word, "for") == 0)
		return (FOR);
	if (strcmp(word, "in") == 0)
		return (IN);
	if (strcmp(word, "if") == 0)
		return (IF);
	if (strcmp(word, "else") == 0)
		return (ELSE);
	if (strcmp(word, "break") == 0)
		return (BREAK);
	if (strcmp(word, "continue") == 0)
		return (CONTINUE);
	if (strcmp(word, "where") == 0)
		return (WHERE);
	if (strcmp(word, "order") == 0)
		return (ORDER);
	if (strcmp(word, "and") == 0)
		return (AND);
	if (strcmp(word, "or") == 0)
		return (OR);
	if (strcmp(word, "not") == 0)
		return (NOT);
	if (strcmp(word, "matches") == 0)
		return (MATCHES);
	if (strcmp(word, "true") == 0)
		return (TRUE_LIT);
	if (strcmp(word, "false") == 0)
		return (FALSE_LIT);
	if (strcmp(word, "nil") == 0)
		return (NIL_LIT);
	if (strcmp(word, "self") == 0)
		return (SELF);
	if (strcmp(word, "windows") == 0) {
		lval->src = DSL_SRC_WINDOWS;
		return (SOURCE_NAME);
	}
	if (strcmp(word, "desks") == 0) {
		lval->src = DSL_SRC_DESKS;
		return (SOURCE_NAME);
	}
	if (strcmp(word, "outputs") == 0) {
		lval->src = DSL_SRC_OUTPUTS;
		return (SOURCE_NAME);
	}
	if (strcmp(word, "screens") == 0) {
		lval->src = DSL_SRC_OUTPUTS;
		return (SOURCE_NAME);
	}
	if (strcmp(word, "rules") == 0) {
		lval->src = DSL_SRC_RULES;
		return (SOURCE_NAME);
	}
	if (strcmp(word, "containers") == 0) {
		lval->src = DSL_SRC_CONTAINERS;
		return (SOURCE_NAME);
	}
	if (strcmp(word, "panes") == 0) {
		lval->src = DSL_SRC_PANES;
		return (SOURCE_NAME);
	}
	if (strcmp(word, "selections") == 0) {
		lval->src = DSL_SRC_SELECTIONS;
		return (SOURCE_NAME);
	}

	if (cmd_find_cmd(word) != NULL) {
		lval->str     = xstrdup(word);
		*push_cmd_arg = true;
		return (CMD_VERB);
	}

	lval->str = xstrdup(word);
	return (IDENT);
}
%}

%x CMDARG
%x CMDSTR
%x CMDSINGLE
%x CMDBODY
%x CMDBODYSTR
%x CMDBODYSINGLE
%x CMDBODYCOMMENT
%x INTERP
%x FIELD
%x STRLIT

ID	[A-Za-z_][A-Za-z0-9_-]*
INT	[0-9]+
DEC	[0-9]+\.[0-9]+

%%

<INITIAL,INTERP,FIELD>{
	[ \t]+			{ /* skip */ }
	\n			{ yyextra->col = 1; }
	"#"[^\n]*		{ /* line comment */ }
}

<CMDARG>{
	[ \t]+			{ yyextra->cmd_arg_continuation = false; }
	\n			{
				yyextra->col = 1;
				yy_pop_state(yyscanner);
			}
	";"			{
				yy_pop_state(yyscanner);
			}
	"#{"			{
				bool continuation = yyextra->cmd_arg_continuation;
				yyextra->cmd_arg_continuation = true;
				yy_push_state(INTERP, yyscanner);
				return (continuation ? INTERP_CONT_BEGIN :
				    INTERP_BEGIN);
			}
	"{"			{
				bool continuation = yyextra->cmd_arg_continuation;
				yyextra->cmd_arg_continuation = true;
				yyextra->cmd_body_depth = 1;
				yy_push_state(CMDBODY, yyscanner);
				return (continuation ? BODY_CONT_BEGIN : BODY_BEGIN);
			}
	\"			{
				bool continuation = yyextra->cmd_arg_continuation;
				yyextra->cmd_arg_continuation = true;
				yy_push_state(CMDSTR, yyscanner);
				return (continuation ? STR_CONT_BEGIN : STR_BEGIN);
			}
	"'"			{
				bool continuation = yyextra->cmd_arg_continuation;
				yyextra->cmd_arg_continuation = true;
				yy_push_state(CMDSINGLE, yyscanner);
				return (continuation ? STR_CONT_BEGIN : STR_BEGIN);
			}
	"}"			{
				/* Re-scan as a block close. */
				yy_pop_state(yyscanner);
				yyless(0);
			}
	[^ \t\n;#{}\"']+		{
				bool continuation = yyextra->cmd_arg_continuation;
				yyextra->cmd_arg_continuation = true;
				yylval->str = xstrdup(yytext);
				return (continuation ? RAW_CONT : RAW_STRING);
			}
}

	/* Single quotes group literal text without escapes or interpolation. */
<CMDSINGLE>{
	"'"			{
				yy_pop_state(yyscanner);
				return (STR_END);
			}
	[^'\n]+			{
				yylval->str = xstrdup(yytext);
				return (RAW_STRING);
			}
	\n			{
				yyextra->col = 1;
				yylval->str = xstrdup("\n");
				return (RAW_STRING);
			}
}

	/* A brace-delimited command argument remains one argv element while
	 * interpolation expressions are still exposed to the parser.
	 */
<CMDBODY>{
	"'"			{
				yy_push_state(CMDBODYSINGLE, yyscanner);
				yylval->str = xstrdup(yytext);
				return (RAW_STRING);
			}
	"#{"			{
				yy_push_state(INTERP, yyscanner);
				return (INTERP_BEGIN);
			}
	"\""			{
				yy_push_state(CMDBODYSTR, yyscanner);
				yylval->str = xstrdup(yytext);
				return (RAW_STRING);
			}
	"#"			{
				yy_push_state(CMDBODYCOMMENT, yyscanner);
				yylval->str = xstrdup(yytext);
				return (RAW_STRING);
			}
	"{"			{
				yyextra->cmd_body_depth++;
				yylval->str = xstrdup(yytext);
				return (RAW_STRING);
			}
	"}"			{
				if (--yyextra->cmd_body_depth == 0) {
					yy_pop_state(yyscanner);
					return (BODY_END);
				}
				yylval->str = xstrdup(yytext);
				return (RAW_STRING);
			}
	\n			{
				yyextra->col = 1;
				yylval->str = xstrdup("\n");
				return (RAW_STRING);
			}
	[^#{}\"'\n]+		{
				yylval->str = xstrdup(yytext);
				return (RAW_STRING);
			}
}

<CMDBODYSINGLE>{
	"'"			{
				yy_pop_state(yyscanner);
				yylval->str = xstrdup(yytext);
				return (RAW_STRING);
			}
	[^'\n]+			{
				yylval->str = xstrdup(yytext);
				return (RAW_STRING);
			}
	\n			{
				yyextra->col = 1;
				yylval->str = xstrdup("\n");
				return (RAW_STRING);
			}
}

<CMDBODYSTR>{
	"#{"			{
				yy_push_state(INTERP, yyscanner);
				return (INTERP_BEGIN);
			}
	\\(.|\n)		{
				yylval->str = xstrdup(yytext);
				return (RAW_STRING);
			}
	"\""			{
				yy_pop_state(yyscanner);
				yylval->str = xstrdup(yytext);
				return (RAW_STRING);
			}
	\n			{
				yyextra->col = 1;
				yylval->str = xstrdup("\n");
				return (RAW_STRING);
			}
	[^\\\"#\n]+		{
				yylval->str = xstrdup(yytext);
				return (RAW_STRING);
			}
	"#"			{
				yylval->str = xstrdup(yytext);
				return (RAW_STRING);
			}
}

<CMDBODYCOMMENT>{
	\n			{
				yyextra->col = 1;
				yy_pop_state(yyscanner);
				yylval->str = xstrdup("\n");
				return (RAW_STRING);
			}
	[^\n]+			{
				yylval->str = xstrdup(yytext);
				return (RAW_STRING);
			}
}

<CMDSTR>{
	\"			{
				yy_pop_state(yyscanner);
				return (STR_END);
			}
	"#{"			{
				yy_push_state(INTERP, yyscanner);
				return (INTERP_BEGIN);
			}
	\\n			{
				yylval->str = xstrdup("\n");
				return (RAW_STRING);
			}
	\\t			{
				yylval->str = xstrdup("\t");
				return (RAW_STRING);
			}
	\\\"			{
				yylval->str = xstrdup("\"");
				return (RAW_STRING);
			}
	\\\\			{
				yylval->str = xstrdup("\\");
				return (RAW_STRING);
			}
	\n			{
				/* unterminated string -- close it */
				yyextra->col = 1;
				yy_pop_state(yyscanner);
				return (STR_END);
			}
	[^\\\"#\n]+		{
				yylval->str = xstrdup(yytext);
				return (RAW_STRING);
			}
	.			{
				yylval->str = xstrdup(yytext);
				return (RAW_STRING);
			}
}

<INTERP>{
	"}"			{
				yy_pop_state(yyscanner);
				return (INTERP_END);
			}
}

	/* After a dot, command names and DSL keywords are ordinary field names. */
<FIELD>{
	{ID}			{
				yy_pop_state(yyscanner);
				yylval->str = xstrdup(yytext);
				return (IDENT);
			}
	.			{
				yy_pop_state(yyscanner);
				return (yytext[0]);
			}
}

<INITIAL,INTERP>{
	"{"			{ return (LBRACE); }
	"}"			{ return (RBRACE); }
	"["			{ return (LBRACKET); }
	"]"			{ return (RBRACKET); }
	"("			{ return (LPAREN); }
	")"			{ return (RPAREN); }
	","			{ return (COMMA); }
	"."			{
				yy_push_state(FIELD, yyscanner);
				return (DOT);
			}
	"=="			{
				yylval->cmp = DSL_CMP_EQ;
				return (CMP_OP);
			}
	"!="			{
				yylval->cmp = DSL_CMP_NE;
				return (CMP_OP);
			}
	"<="			{
				yylval->cmp = DSL_CMP_LE;
				return (CMP_OP);
			}
	">="			{
				yylval->cmp = DSL_CMP_GE;
				return (CMP_OP);
			}
	"<"			{
				yylval->cmp = DSL_CMP_LT;
				return (CMP_OP);
			}
	">"			{
				yylval->cmp = DSL_CMP_GT;
				return (CMP_OP);
			}
	"="			{ return (EQUALS); }

	{DEC}			{
				yylval->num = strtod(yytext, NULL);
				return (NUMBER_LIT);
			}
	{INT}			{
				yylval->num = strtod(yytext, NULL);
				return (NUMBER_LIT);
			}

	\"			{
				strbuf_reset(yyextra);
				yy_push_state(STRLIT, yyscanner);
			}

	{ID}			{
				bool push_cmd_arg = false;
				int  tok = classify_word(yytext, yylval,
				    &push_cmd_arg);
				if (push_cmd_arg)
					yyextra->cmd_arg_continuation = false;
				if (push_cmd_arg)
					yy_push_state(CMDARG, yyscanner);
				return (tok);
			}

	.			{
				/* Unknown character; let bison report. */
				return (yytext[0]);
			}
}

<STRLIT>{
	\\n			{ strbuf_putc(yyextra, '\n'); }
	\\t			{ strbuf_putc(yyextra, '\t'); }
	\\r			{ strbuf_putc(yyextra, '\r'); }
	\\\"			{ strbuf_putc(yyextra, '"'); }
	\\\\			{ strbuf_putc(yyextra, '\\'); }
	\\.			{ strbuf_putc(yyextra, yytext[1]); }
	\"			{
				yy_pop_state(yyscanner);
				yylval->str = strbuf_take(yyextra);
				return (STRING_LIT);
			}
	\n			{
				/* unterminated string -- treat as end */
				yyextra->col = 1;
				yy_pop_state(yyscanner);
				yylval->str = strbuf_take(yyextra);
				return (STRING_LIT);
			}
	.			{ strbuf_putc(yyextra, yytext[0]); }
}

%%

/*
 * Silence -Wunused-function for the flex-generated yy_top_state(),
 * which `%option stack` always emits even though we only use push /
 * pop.  Referencing it from this no-link symbol keeps the linker
 * happy without changing behaviour.
 */
void *dsl_lexer_keep_top_state_used = (void *)&yy_top_state;
