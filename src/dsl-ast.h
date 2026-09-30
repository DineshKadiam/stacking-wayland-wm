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

#if !defined(COW_DSL_AST_H)
#define COW_DSL_AST_H

#include <stdbool.h>
#include <stddef.h>

#include "cow/dsl.h"

/*
 * DSL AST node taxonomy + parse-context plumbing.
 *
 * Nodes are allocated from a bump arena owned by the dsl_ast.  The
 * arena frees in one shot at dsl_free() so individual nodes don't
 * need per-field destructors -- strings owned by nodes (identifiers,
 * literals, RAW_STRING arg chunks) are also arena-allocated.
 *
 * Public surface lives in cow/dsl.h; this header is internal to the
 * DSL implementation units (lexer, parser, evaluator).
 */

enum dsl_node_kind {
	DSL_N_BLOCK, /* { stmt* } */

	/* Statements */
	DSL_N_LET, /* let IDENT = expr */
	DSL_N_FOR, /* for IDENT in source block */
	DSL_N_IF,  /* if expr block (else block-or-if)? */
	DSL_N_CMD, /* VERB arg*  (a cow command invocation) */
	DSL_N_BREAK,
	DSL_N_CONTINUE,

	/* Expressions */
	DSL_N_OR,
	DSL_N_AND,
	DSL_N_NOT,
	DSL_N_CMP,  /* binary comparison; op encoded in u.cmp.op */
	DSL_N_IN,   /* membership test; negated encoded in u.in.negated */
	DSL_N_LIST, /* list literal */
	DSL_N_PATH, /* IDENT(.IDENT)*  -- variable + field access */
	DSL_N_NIL,
	DSL_N_NUM,
	DSL_N_STR,
	DSL_N_BOOL,
};

/* Comparison operators for DSL_N_CMP. */
enum dsl_cmp_op {
	DSL_CMP_EQ,
	DSL_CMP_NE,
	DSL_CMP_LT,
	DSL_CMP_LE,
	DSL_CMP_GT,
	DSL_CMP_GE,
	DSL_CMP_MATCH,
	DSL_CMP_NOT_MATCH,
};

/* Source identifiers used by DSL_N_FOR. */
enum dsl_source {
	DSL_SRC_WINDOWS,
	DSL_SRC_CONTAINERS,
	DSL_SRC_PANES,
	DSL_SRC_DESKS,
	DSL_SRC_OUTPUTS,
	DSL_SRC_RULES,
	DSL_SRC_SELECTIONS,
};

struct dsl_node;

/*
 * A "piece" of a command argument: either a raw text chunk or an
 * interpolation expression.  Bare arguments (outside double quotes)
 * are a single piece.  Double-quoted arguments are an ordered list
 * of pieces concatenated at evaluation time.
 */
struct dsl_arg_piece {
	bool is_raw;
	union {
		char		*raw;
		struct dsl_node *interp;
	} u;
};

/*
 * One argument of a command atom.  An argument owns one or more
 * pieces; the evaluator stringifies each and concatenates them to
 * produce the final argv string.  For bare RAW_STRING / interp
 * arguments there is a single piece.
 */
struct dsl_cmd_arg {
	struct dsl_arg_piece *pieces;
	size_t		      n;
};

struct dsl_node {
	enum dsl_node_kind kind;
	int		   line;
	int		   col;

	union {
		struct {
			struct dsl_node **stmts;
			size_t		  n;
		} block;

		struct {
			char		*name;
			struct dsl_node *value;
		} let;

		struct {
			char		*var;
			enum dsl_source	 src;
			struct dsl_node *where; /* may be NULL */
			struct dsl_node *body;
		} forstmt;

		struct {
			struct dsl_node *cond;
			struct dsl_node *body;
			struct dsl_node *otherwise; /* NULL or block / if */
		} ifstmt;

		struct {
			char		   *verb;
			struct dsl_cmd_arg *args;
			size_t		    n;
		} cmd;

		struct {
			struct dsl_node *lhs;
			struct dsl_node *rhs;
		} binop;

		struct {
			struct dsl_node *e;
		} unop;

		struct {
			enum dsl_cmp_op	 op;
			struct dsl_node *lhs;
			struct dsl_node *rhs;
		} cmp;

		struct {
			bool		 negated;
			struct dsl_node *lhs;
			struct dsl_node *rhs;
		} in;

		struct {
			struct dsl_node **items;
			size_t		  n;
		} list;

		struct {
			char *
			    *parts; /* parts[0]=base ident, parts[1..]=fields */
			size_t n;
		} path;

		struct {
			double n;
		} num;

		struct {
			char *s;
		} str;

		struct {
			bool b;
		} boolean;
	} u;
};

/*
 * AST + bump-arena allocator.
 */
struct dsl_arena_chunk {
	struct dsl_arena_chunk *next;
	size_t			used;
	size_t			cap;
	unsigned char		mem[];
};

struct dsl_ast {
	struct dsl_node	       *root; /* DSL_N_BLOCK */
	struct dsl_arena_chunk *arena;
};

/*
 * Arena allocation.  Pointers stay valid for the lifetime of the
 * AST; freed in bulk at dsl_free().
 */
void *dsl_arena_alloc(struct dsl_ast *, size_t);
char *dsl_arena_strdup(struct dsl_ast *, const char *);
char *dsl_arena_strndup(struct dsl_ast *, const char *, size_t);

/*
 * AST construction.  Returns a fresh empty AST; nodes are added by
 * the parser actions in dsl-parser.y.
 */
struct dsl_ast *dsl_ast_new(void);

/*
 * Node constructors.  Each allocates from the AST's arena and
 * stamps line/col from the supplied location.
 */
struct dsl_node *dsl_node_block(struct dsl_ast *, int line, int col);
struct dsl_node *dsl_node_let(struct dsl_ast *, int line, int col,
    const char *name, struct dsl_node *value);
struct dsl_node *dsl_node_for(struct dsl_ast *, int line, int col,
    const char *var, enum dsl_source, struct dsl_node *where,
    struct dsl_node *body);
struct dsl_node *dsl_node_if(struct dsl_ast *, int line, int col,
    struct dsl_node *cond, struct dsl_node *body, struct dsl_node *otherwise);
struct dsl_node *dsl_node_cmd(struct dsl_ast *, int line, int col,
    const char *verb);
struct dsl_node *dsl_node_break(struct dsl_ast *, int line, int col);
struct dsl_node *dsl_node_continue(struct dsl_ast *, int line, int col);
struct dsl_node *dsl_node_or(struct dsl_ast *, int line, int col,
    struct dsl_node *lhs, struct dsl_node *rhs);
struct dsl_node *dsl_node_and(struct dsl_ast *, int line, int col,
    struct dsl_node *lhs, struct dsl_node *rhs);
struct dsl_node *dsl_node_not(struct dsl_ast *, int line, int col,
    struct dsl_node *e);
struct dsl_node *dsl_node_cmp(struct dsl_ast *, int line, int col,
    enum dsl_cmp_op, struct dsl_node *lhs, struct dsl_node *rhs);
struct dsl_node *dsl_node_in(struct dsl_ast *, int line, int col,
    struct dsl_node *lhs, struct dsl_node *rhs, bool negated);
struct dsl_node *dsl_node_list(struct dsl_ast *, int line, int col);
struct dsl_node *dsl_node_path(struct dsl_ast *, int line, int col,
    const char *base);
struct dsl_node *dsl_node_nil(struct dsl_ast *, int line, int col);
struct dsl_node *dsl_node_num(struct dsl_ast *, int line, int col, double);
struct dsl_node *dsl_node_str(struct dsl_ast *, int line, int col,
    const char *s);
struct dsl_node *dsl_node_bool(struct dsl_ast *, int line, int col, bool b);

/*
 * Builders for the variable-length children of certain nodes.  Each
 * resizes the inline array in place; uses arena memory.
 */
void dsl_block_append(struct dsl_ast *, struct dsl_node *block,
    struct dsl_node *stmt);
void dsl_path_append(struct dsl_ast *, struct dsl_node *path,
    const char *field);
void dsl_list_append_node(struct dsl_ast *, struct dsl_node *list,
    struct dsl_node *item);
/*
 * Bare-argument forms: a single raw chunk or a single interp.  Each
 * appends one new cmd_arg with exactly one piece.
 */
void dsl_cmd_append_raw(struct dsl_ast *, struct dsl_node *cmd,
    const char *raw);
void dsl_cmd_append_interp(struct dsl_ast *, struct dsl_node *cmd,
    struct dsl_node *expr);

/*
 * Quoted-argument forms.  dsl_cmd_begin_str_arg appends a new empty
 * cmd_arg slot; dsl_cmd_str_push_raw / _push_interp append pieces
 * into the most recently begun slot.
 */
void dsl_cmd_begin_str_arg(struct dsl_ast *, struct dsl_node *cmd);
void dsl_cmd_str_push_raw(struct dsl_ast *, struct dsl_node *cmd,
    const char *raw);
void dsl_cmd_str_push_interp(struct dsl_ast *, struct dsl_node *cmd,
    struct dsl_node *expr);

/*
 * Parse-context object passed to the bison parser.  Lives on the
 * caller's stack inside dsl_parse().
 *
 * `current_cmd` tracks the most recently begun command-statement so
 * grammar actions inside a quoted-arg's piece list can locate the
 * cmd node they should mutate.  Set when a CMD_VERB starts a new
 * cmd_stmt; cleared when the cmd_stmt is consumed by an enclosing
 * production.
 */
struct dsl_parse_ctx {
	struct dsl_ast	 *ast;
	struct dsl_error *err; /* may be NULL */
	const char	 *file;
	struct dsl_node	 *current_cmd;
};

/*
 * Per-scanner state used by the reentrant Flex lexer.
 */
struct dsl_lexer_ctx {
	int	 col;
	int	 cmd_body_depth;
	bool	 cmd_arg_continuation;
	char	*strbuf;
	size_t	 strbuf_len;
	size_t	 strbuf_cap;
};

#endif /* COW_DSL_AST_H */
