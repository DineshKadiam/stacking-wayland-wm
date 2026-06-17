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
 * CoW DSL lexer.
 *
 * Three start conditions:
 *
 *   INITIAL    -- the default "expression" mode.  Recognises DSL
 *                 keywords, identifiers, literals, comparisons, etc.
 *                 Identifiers that match a registered cow command
 *                 verb are emitted as CMD_VERB and switch the lexer
 *                 into <CMDARG> mode for the rest of that statement.
 *
 *   <CMDARG>   -- command-argument mode.  Each whitespace-delimited
 *                 chunk is emitted as a RAW_STRING.  Recognises `#{`
 *                 (-> INTERP_BEGIN, push <INTERP> on the start-condition
 *                 stack) and statement terminators (newline, ; or
 *                 reaching the enclosing }), which pop back to
 *                 INITIAL so the parser is ready for the next stmt.
 *
 *   <INTERP>   -- inside #{ ... }, behaves like the expression mode
 *                 until the matching } which emits INTERP_END and
 *                 pops back to <CMDARG>.
 *
 *   <STRLIT>   -- inside a "..." string literal, accumulates chars
 *                 (with escape processing) until the closing quote.
 */

%option reentrant
%option bison-bridge
%option bison-locations
%option noyywrap
%option nounput
%option noinput
%option yylineno
%option stack

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

/*
 * Track column ourselves; flex updates yylineno but not column.  We
 * reset col to 1 on newline and add yyleng on every match.
 */
#define YY_USER_ACTION                                                         \
	do {                                                                   \
		yylloc->first_line   = yylineno;                               \
		yylloc->first_column = yycol_start;                            \
		yylloc->last_line    = yylineno;                               \
		yylloc->last_column  = yycol_start + (int)yyleng;              \
		yycol_start += (int)yyleng;                                    \
	} while (0);

/* These live in lexer-local extra storage; flex's reentrancy
 * normally requires yyextra, but we cheat a little since the column
 * counter is purely local to this translation unit and never read
 * from outside.  For multi-instance correctness we use a thread-
 * local; cow is single-threaded so this is safe. */
static __thread int yycol_start = 1;

/*
 * Accumulator for string-literal content.  Filled by the <STRLIT>
 * rules; emitted as a STRING_LIT token on the closing quote.
 */
struct str_accum {
	char  *buf;
	size_t len;
	size_t cap;
};

static __thread struct str_accum strbuf;

static void
strbuf_reset(void)
{
	strbuf.len = 0;
	if (strbuf.cap == 0) {
		strbuf.cap = 32;
		strbuf.buf = xmalloc(strbuf.cap);
	}
	strbuf.buf[0] = '\0';
}

static void
strbuf_putc(int c)
{
	if (strbuf.len + 2 > strbuf.cap) {
		strbuf.cap *= 2;
		strbuf.buf = xrealloc(strbuf.buf, strbuf.cap, 1);
	}
	strbuf.buf[strbuf.len++] = (char)c;
	strbuf.buf[strbuf.len]	 = '\0';
}

static char *
strbuf_take(void)
{
	char *out  = xstrdup(strbuf.buf != NULL ? strbuf.buf : "");
	strbuf.len = 0;
	if (strbuf.buf != NULL)
		strbuf.buf[0] = '\0';
	return (out);
}

/*
 * Classify a bare-word.  Returns the token id; if the word is a
 * cow command verb, *push_cmd_arg is set to true so the caller
 * (which has access to yy_push_state) can transition the lexer.
 */
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
	if (strcmp(word, "where") == 0)
		return (WHERE);
	if (strcmp(word, "and") == 0)
		return (AND);
	if (strcmp(word, "or") == 0)
		return (OR);
	if (strcmp(word, "not") == 0)
		return (NOT);
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
%x INTERP
%x STRLIT

ID	[A-Za-z_][A-Za-z0-9_-]*
INT	[0-9]+
DEC	[0-9]+\.[0-9]+

%%

	/*
	 * Whitespace + comments: skipped in INITIAL and INTERP modes.
	 * In CMDARG mode newline / ; pops back to INITIAL because the
	 * statement is over.
	 */
<INITIAL,INTERP>{
	[ \t]+			{ /* skip */ }
	\n			{ yycol_start = 1; }
	"#"[^\n]*		{ /* line comment */ }
}

<CMDARG>{
	[ \t]+			{ /* skip inter-arg whitespace */ }
	\n			{
				yycol_start = 1;
				yy_pop_state(yyscanner);
			}
	";"			{
				yy_pop_state(yyscanner);
			}
	"#{"			{
				yy_push_state(INTERP, yyscanner);
				return (INTERP_BEGIN);
			}
	\"			{
				/*
				 * Double-quoted command argument.  The
				 * inner content (with #{...} interpolations
				 * honoured) is concatenated into a single
				 * argv element.
				 */
				yy_push_state(CMDSTR, yyscanner);
				return (STR_BEGIN);
			}
	"}"			{
				/*
				 * Statement ends because the enclosing
				 * block is closing; pop back to INITIAL
				 * and re-scan this `}` as the block close.
				 */
				yy_pop_state(yyscanner);
				yyless(0);
			}
	[^ \t\n;#}\"]+		{
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
				yycol_start = 1;
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

<INITIAL,INTERP>{
	"{"			{ return (LBRACE); }
	"}"			{ return (RBRACE); }
	"("			{ return (LPAREN); }
	")"			{ return (RPAREN); }
	"."			{ return (DOT); }
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
				strbuf_reset();
				yy_push_state(STRLIT, yyscanner);
			}

	{ID}			{
				bool push_cmd_arg = false;
				int  tok = classify_word(yytext, yylval,
				    &push_cmd_arg);
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
	\\n			{ strbuf_putc('\n'); }
	\\t			{ strbuf_putc('\t'); }
	\\r			{ strbuf_putc('\r'); }
	\\\"			{ strbuf_putc('"'); }
	\\\\			{ strbuf_putc('\\'); }
	\\.			{ strbuf_putc(yytext[1]); }
	\"			{
				yy_pop_state(yyscanner);
				yylval->str = strbuf_take();
				return (STRING_LIT);
			}
	\n			{
				/* unterminated string -- treat as end */
				yycol_start = 1;
				yy_pop_state(yyscanner);
				yylval->str = strbuf_take();
				return (STRING_LIT);
			}
	.			{ strbuf_putc(yytext[0]); }
}

%%

/*
 * Silence -Wunused-function for the flex-generated yy_top_state(),
 * which `%option stack` always emits even though we only use push /
 * pop.  Referencing it from this no-link symbol keeps the linker
 * happy without changing behaviour.
 */
void *dsl_lexer_keep_top_state_used = (void *)&yy_top_state;
