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
 * DSL AST: bump-arena allocator and node constructors.
 *
 * Every AST node, the strings it owns (identifiers, string literals,
 * RAW_STRING command-arg chunks), and the inline child arrays
 * (block.stmts, path.parts, cmd.args) live in chunks attached to
 * the dsl_ast.  dsl_free() walks the chunk list and frees everything
 * in one pass.
 *
 * Chunks start at 4 KiB and double on each new allocation, capped at
 * 64 KiB per chunk to keep growth predictable.
 */

#include <stdlib.h>
#include <string.h>

#include "cow/compat.h"
#include "cow/memory.h"

#include "dsl-ast.h"

#define DSL_ARENA_MIN_CHUNK 4096u
#define DSL_ARENA_MAX_CHUNK (64u * 1024u)

struct dsl_ast *
dsl_ast_new(void)
{
	return (xcalloc(1, sizeof(struct dsl_ast)));
}

void
dsl_free(struct dsl_ast *ast)
{
	if (ast == NULL)
		return;

	struct dsl_arena_chunk *c = ast->arena;
	while (c != NULL) {
		struct dsl_arena_chunk *next = c->next;
		xfree(c);
		c = next;
	}
	xfree(ast);
}

static struct dsl_arena_chunk *
arena_new_chunk(size_t need)
{
	size_t cap = DSL_ARENA_MIN_CHUNK;
	while (cap < need + sizeof(struct dsl_arena_chunk))
		cap *= 2;
	if (cap > DSL_ARENA_MAX_CHUNK &&
	    need + sizeof(struct dsl_arena_chunk) <= DSL_ARENA_MAX_CHUNK)
		cap = DSL_ARENA_MAX_CHUNK;

	struct dsl_arena_chunk *c = xmalloc(cap);
	c->next			  = NULL;
	c->used			  = 0;
	c->cap			  = cap - sizeof(struct dsl_arena_chunk);
	return (c);
}

void *
dsl_arena_alloc(struct dsl_ast *ast, size_t bytes)
{
	/* 8-byte align all returned pointers so we can safely place
	 * any of our struct types here. */
	bytes = (bytes + 7u) & ~(size_t)7u;

	struct dsl_arena_chunk *c = ast->arena;
	if (c == NULL || c->used + bytes > c->cap) {
		struct dsl_arena_chunk *nc = arena_new_chunk(bytes);
		nc->next		   = ast->arena;
		ast->arena		   = nc;
		c			   = nc;
	}
	void *p = &c->mem[c->used];
	c->used += bytes;
	return (p);
}

char *
dsl_arena_strdup(struct dsl_ast *ast, const char *s)
{
	if (s == NULL)
		return (NULL);
	return (dsl_arena_strndup(ast, s, strlen(s)));
}

char *
dsl_arena_strndup(struct dsl_ast *ast, const char *s, size_t n)
{
	char *out = dsl_arena_alloc(ast, n + 1);
	if (n > 0)
		memcpy(out, s, n);
	out[n] = '\0';
	return (out);
}

static struct dsl_node *
node_new(struct dsl_ast *ast, enum dsl_node_kind k, int line, int col)
{
	struct dsl_node *n = dsl_arena_alloc(ast, sizeof(*n));
	memset(n, 0, sizeof(*n));
	n->kind = k;
	n->line = line;
	n->col	= col;
	return (n);
}

struct dsl_node *
dsl_node_block(struct dsl_ast *ast, int line, int col)
{
	return (node_new(ast, DSL_N_BLOCK, line, col));
}

struct dsl_node *
dsl_node_let(struct dsl_ast *ast, int line, int col, const char *name,
    struct dsl_node *value)
{
	struct dsl_node *n = node_new(ast, DSL_N_LET, line, col);
	n->u.let.name	   = dsl_arena_strdup(ast, name);
	n->u.let.value	   = value;
	return (n);
}

struct dsl_node *
dsl_node_for(struct dsl_ast *ast, int line, int col, const char *var,
    enum dsl_source src, struct dsl_node *where, struct dsl_node *body)
{
	struct dsl_node *n = node_new(ast, DSL_N_FOR, line, col);
	n->u.forstmt.var   = dsl_arena_strdup(ast, var);
	n->u.forstmt.src   = src;
	n->u.forstmt.where = where;
	n->u.forstmt.body  = body;
	return (n);
}

struct dsl_node *
dsl_node_if(struct dsl_ast *ast, int line, int col, struct dsl_node *cond,
    struct dsl_node *body, struct dsl_node *otherwise)
{
	struct dsl_node *n    = node_new(ast, DSL_N_IF, line, col);
	n->u.ifstmt.cond      = cond;
	n->u.ifstmt.body      = body;
	n->u.ifstmt.otherwise = otherwise;
	return (n);
}

struct dsl_node *
dsl_node_cmd(struct dsl_ast *ast, int line, int col, const char *verb)
{
	struct dsl_node *n = node_new(ast, DSL_N_CMD, line, col);
	n->u.cmd.verb	   = dsl_arena_strdup(ast, verb);
	return (n);
}

struct dsl_node *
dsl_node_or(struct dsl_ast *ast, int line, int col, struct dsl_node *lhs,
    struct dsl_node *rhs)
{
	struct dsl_node *n = node_new(ast, DSL_N_OR, line, col);
	n->u.binop.lhs	   = lhs;
	n->u.binop.rhs	   = rhs;
	return (n);
}

struct dsl_node *
dsl_node_and(struct dsl_ast *ast, int line, int col, struct dsl_node *lhs,
    struct dsl_node *rhs)
{
	struct dsl_node *n = node_new(ast, DSL_N_AND, line, col);
	n->u.binop.lhs	   = lhs;
	n->u.binop.rhs	   = rhs;
	return (n);
}

struct dsl_node *
dsl_node_not(struct dsl_ast *ast, int line, int col, struct dsl_node *e)
{
	struct dsl_node *n = node_new(ast, DSL_N_NOT, line, col);
	n->u.unop.e	   = e;
	return (n);
}

struct dsl_node *
dsl_node_cmp(struct dsl_ast *ast, int line, int col, enum dsl_cmp_op op,
    struct dsl_node *lhs, struct dsl_node *rhs)
{
	struct dsl_node *n = node_new(ast, DSL_N_CMP, line, col);
	n->u.cmp.op	   = op;
	n->u.cmp.lhs	   = lhs;
	n->u.cmp.rhs	   = rhs;
	return (n);
}

struct dsl_node *
dsl_node_path(struct dsl_ast *ast, int line, int col, const char *base)
{
	struct dsl_node *n = node_new(ast, DSL_N_PATH, line, col);
	dsl_path_append(ast, n, base);
	return (n);
}

struct dsl_node *
dsl_node_num(struct dsl_ast *ast, int line, int col, double v)
{
	struct dsl_node *n = node_new(ast, DSL_N_NUM, line, col);
	n->u.num.n	   = v;
	return (n);
}

struct dsl_node *
dsl_node_str(struct dsl_ast *ast, int line, int col, const char *s)
{
	struct dsl_node *n = node_new(ast, DSL_N_STR, line, col);
	n->u.str.s	   = dsl_arena_strdup(ast, s);
	return (n);
}

struct dsl_node *
dsl_node_bool(struct dsl_ast *ast, int line, int col, bool b)
{
	struct dsl_node *n = node_new(ast, DSL_N_BOOL, line, col);
	n->u.boolean.b	   = b;
	return (n);
}

/*
 * Variable-length child builders.  Each allocates a fresh, larger
 * array on each append and copies the old contents over; the old
 * array is left abandoned in the arena (and freed in one shot at
 * dsl_free()).  AST construction is one-shot so the wasted memory
 * is bounded by the input size.
 */

static void *
arena_array_append(struct dsl_ast *ast, void *old, size_t n, size_t item_size,
    const void *item)
{
	void *new_v = dsl_arena_alloc(ast, (n + 1) * item_size);

	if (old != NULL && n > 0)
		memcpy(new_v, old, n * item_size);
	memcpy((char *)new_v + n * item_size, item, item_size);
	return (new_v);
}

void
dsl_block_append(struct dsl_ast *ast, struct dsl_node *block,
    struct dsl_node *stmt)
{
	block->u.block.stmts = arena_array_append(ast, block->u.block.stmts,
	    block->u.block.n, sizeof(*block->u.block.stmts), &stmt);
	block->u.block.n++;
}

void
dsl_path_append(struct dsl_ast *ast, struct dsl_node *path, const char *field)
{
	char *copy = dsl_arena_strdup(ast, field);

	path->u.path.parts = arena_array_append(ast, path->u.path.parts,
	    path->u.path.n, sizeof(*path->u.path.parts), &copy);
	path->u.path.n++;
}

/* Grow cmd->u.cmd.args by one and return a pointer to the new slot,
 * initialised empty. */
static struct dsl_cmd_arg *
cmd_grow_args(struct dsl_ast *ast, struct dsl_node *cmd)
{
	struct dsl_cmd_arg empty = { 0 };

	cmd->u.cmd.args = arena_array_append(ast, cmd->u.cmd.args,
	    cmd->u.cmd.n, sizeof(*cmd->u.cmd.args), &empty);
	return (&cmd->u.cmd.args[cmd->u.cmd.n++]);
}

/* Append a piece to the given arg slot. */
static void
arg_push_piece(struct dsl_ast *ast, struct dsl_cmd_arg *arg, bool is_raw,
    const char *raw, struct dsl_node *interp)
{
	struct dsl_arg_piece piece = { .is_raw = is_raw };

	if (is_raw)
		piece.u.raw = dsl_arena_strdup(ast, raw);
	else
		piece.u.interp = interp;

	arg->pieces = arena_array_append(ast, arg->pieces, arg->n,
	    sizeof(*arg->pieces), &piece);
	arg->n++;
}

void
dsl_cmd_append_raw(struct dsl_ast *ast, struct dsl_node *cmd, const char *raw)
{
	struct dsl_cmd_arg *slot = cmd_grow_args(ast, cmd);
	arg_push_piece(ast, slot, true, raw, NULL);
}

void
dsl_cmd_append_interp(struct dsl_ast *ast, struct dsl_node *cmd,
    struct dsl_node *expr)
{
	struct dsl_cmd_arg *slot = cmd_grow_args(ast, cmd);
	arg_push_piece(ast, slot, false, NULL, expr);
}

void
dsl_cmd_begin_str_arg(struct dsl_ast *ast, struct dsl_node *cmd)
{
	cmd_grow_args(ast, cmd);
}

void
dsl_cmd_str_push_raw(struct dsl_ast *ast, struct dsl_node *cmd, const char *raw)
{
	if (cmd->u.cmd.n == 0)
		return; /* defensive: parser should have begun first */
	struct dsl_cmd_arg *slot = &cmd->u.cmd.args[cmd->u.cmd.n - 1];
	arg_push_piece(ast, slot, true, raw, NULL);
}

void
dsl_cmd_str_push_interp(struct dsl_ast *ast, struct dsl_node *cmd,
    struct dsl_node *expr)
{
	if (cmd->u.cmd.n == 0)
		return;
	struct dsl_cmd_arg *slot = &cmd->u.cmd.args[cmd->u.cmd.n - 1];
	arg_push_piece(ast, slot, false, NULL, expr);
}
