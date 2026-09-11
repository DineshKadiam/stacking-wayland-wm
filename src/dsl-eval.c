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

/* DSL parser entry points and tree-walking evaluator. */

#include <fnmatch.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cow/cmd.h"
#include "cow/dsl.h"
#include "cow/memory.h"
#include "cow/view.h"

#include "dsl-ast.h"
#include "state-snapshot.h"
#include "state-value.h"

struct dsl_env_entry {
	char		 *name;
	struct state_value *value;
};

struct dsl_env {
	struct dsl_env	     *parent;
	struct dsl_env_entry *entries;
	size_t		      n;
	size_t		      cap;
};

enum dsl_control {
	DSL_CTL_NONE,
	DSL_CTL_BREAK,
	DSL_CTL_CONTINUE,
};

struct dsl_eval_ctx {
	struct cmd_q	 *cmdq;
	struct dsl_error *err;
	bool		  failed;
	enum dsl_control control;
	size_t		  loop_depth;

	/* Default -t for commands inside window loops. */
	const char *implicit_target_id;
};

static struct dsl_env *
env_new(struct dsl_env *parent)
{
	struct dsl_env *e = xcalloc(1, sizeof(*e));
	e->parent	  = parent;
	return (e);
}

static void
env_free(struct dsl_env *e)
{
	for (size_t i = 0; i < e->n; i++) {
		xfree(e->entries[i].name);
		state_value_unref(e->entries[i].value);
	}
	xfree(e->entries);
	xfree(e);
}

static void
env_set(struct dsl_env *e, const char *name, struct state_value *v)
{
	for (size_t i = 0; i < e->n; i++) {
		if (strcmp(e->entries[i].name, name) != 0)
			continue;
		state_value_unref(e->entries[i].value);
		e->entries[i].value = state_value_ref(v);
		return;
	}

	if (e->n == e->cap) {
		e->cap	   = e->cap ? e->cap * 2 : 4;
		e->entries = xrealloc(e->entries, e->cap, sizeof(*e->entries));
	}
	e->entries[e->n].name  = xstrdup(name);
	e->entries[e->n].value = state_value_ref(v);
	e->n++;
}

/* Borrowed lookup; NULL if not found. */
static struct state_value *
env_get(const struct dsl_env *e, const char *name)
{
	for (; e != NULL; e = e->parent) {
		for (size_t i = 0; i < e->n; i++) {
			if (strcmp(e->entries[i].name, name) == 0)
				return (e->entries[i].value);
		}
	}
	return (NULL);
}

static void
fail_at(struct dsl_eval_ctx *ctx, int line, int col, const char *fmt, ...)
{
	if (ctx->failed)
		return; /* keep first */
	ctx->failed = true;

	if (ctx->err == NULL)
		return;

	char	buf[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);

	ctx->err->file	  = NULL;
	ctx->err->line	  = line;
	ctx->err->col	  = col;
	ctx->err->message = xstrdup(buf);
}

static struct state_value *eval_expr(struct dsl_eval_ctx *, struct dsl_env *,
    struct dsl_node *);

static enum state_source
state_source_from_dsl(enum dsl_source source)
{
	switch (source) {
	case DSL_SRC_WINDOWS:
		return (STATE_SRC_WINDOWS);
	case DSL_SRC_DESKS:
		return (STATE_SRC_DESKS);
	case DSL_SRC_OUTPUTS:
		return (STATE_SRC_OUTPUTS);
	case DSL_SRC_RULES:
		return (STATE_SRC_RULES);
	}
	return (STATE_SRC_WINDOWS);
}

/* Evaluate a dotted path and return a fresh reference. */
static struct state_value *
eval_path(struct dsl_eval_ctx *ctx, struct dsl_env *env, struct dsl_node *n)
{
	if (n->u.path.n == 0)
		return (NULL);

	struct state_value *cur = env_get(env, n->u.path.parts[0]);
	if (cur == NULL) {
		fail_at(ctx, n->line, n->col, "unknown name '%s'",
		    n->u.path.parts[0]);
		return (NULL);
	}
	state_value_ref(cur);

	for (size_t i = 1; i < n->u.path.n; i++) {
		if (cur->type != STATE_V_RECORD) {
			fail_at(ctx, n->line, n->col,
			    "field access on non-record: '%s.%s'",
			    n->u.path.parts[0], n->u.path.parts[i]);
			state_value_unref(cur);
			return (NULL);
		}
		struct state_value *field = state_record_get(cur,
		    n->u.path.parts[i]);
		if (field == NULL) {
			state_value_unref(cur);
			return (state_value_nil());
		}
		state_value_ref(field);
		state_value_unref(cur);
		cur = field;
	}
	return (cur);
}

/* Typed comparison. */
static bool
compare(struct dsl_eval_ctx *ctx, struct dsl_node *n, struct state_value *lhs,
    struct state_value *rhs, bool *out)
{
	if (lhs->type != rhs->type) {
		fail_at(ctx, n->line, n->col,
		    "comparing values of different types");
		return (false);
	}

	enum dsl_cmp_op op = n->u.cmp.op;
	if (op == DSL_CMP_MATCH || op == DSL_CMP_NOT_MATCH) {
		if (lhs->type != STATE_V_STRING) {
			fail_at(ctx, n->line, n->col,
			    "'matches' requires string operands");
			return (false);
		}
		bool matched = fnmatch(rhs->u.s ? rhs->u.s : "",
		    lhs->u.s ? lhs->u.s : "", 0) == 0;
		*out = op == DSL_CMP_MATCH ? matched : !matched;
		return (true);
	}

	if (lhs->type == STATE_V_NUMBER) {
		double a = lhs->u.n, b = rhs->u.n;
		switch (op) {
		case DSL_CMP_EQ:
			*out = (a == b);
			break;
		case DSL_CMP_NE:
			*out = (a != b);
			break;
		case DSL_CMP_LT:
			*out = (a < b);
			break;
		case DSL_CMP_LE:
			*out = (a <= b);
			break;
		case DSL_CMP_GT:
			*out = (a > b);
			break;
		case DSL_CMP_GE:
			*out = (a >= b);
			break;
		case DSL_CMP_MATCH:
		case DSL_CMP_NOT_MATCH:
			return (false);
		}
		return (true);
	}
	if (lhs->type == STATE_V_STRING) {
		int c = strcmp(lhs->u.s ? lhs->u.s : "",
		    rhs->u.s ? rhs->u.s : "");
		switch (op) {
		case DSL_CMP_EQ:
			*out = (c == 0);
			break;
		case DSL_CMP_NE:
			*out = (c != 0);
			break;
		case DSL_CMP_LT:
			*out = (c < 0);
			break;
		case DSL_CMP_LE:
			*out = (c <= 0);
			break;
		case DSL_CMP_GT:
			*out = (c > 0);
			break;
		case DSL_CMP_GE:
			*out = (c >= 0);
			break;
		case DSL_CMP_MATCH:
		case DSL_CMP_NOT_MATCH:
			return (false);
		}
		return (true);
	}
	if (lhs->type == STATE_V_BOOL) {
		if (op != DSL_CMP_EQ && op != DSL_CMP_NE) {
			fail_at(ctx, n->line, n->col,
			    "ordering comparison on bool");
			return (false);
		}
		*out = (op == DSL_CMP_EQ) ? (lhs->u.b == rhs->u.b) :
					    (lhs->u.b != rhs->u.b);
		return (true);
	}
	if (lhs->type == STATE_V_NIL) {
		if (op != DSL_CMP_EQ && op != DSL_CMP_NE) {
			fail_at(ctx, n->line, n->col,
			    "ordering comparison on nil");
			return (false);
		}
		*out = op == DSL_CMP_EQ;
		return (true);
	}
	fail_at(ctx, n->line, n->col, "comparison not defined for this type");
	return (false);
}

static bool
value_equal(const struct state_value *a, const struct state_value *b)
{
	if (a->type != b->type)
		return (false);

	switch (a->type) {
	case STATE_V_NIL:
		return (true);
	case STATE_V_BOOL:
		return (a->u.b == b->u.b);
	case STATE_V_NUMBER:
		return (a->u.n == b->u.n);
	case STATE_V_STRING:
		return (strcmp(a->u.s ? a->u.s : "", b->u.s ? b->u.s : "")
		    == 0);
	case STATE_V_LIST:
	case STATE_V_RECORD:
		return (false);
	}
	return (false);
}

static bool
membership(struct dsl_eval_ctx *ctx, struct dsl_node *n,
    struct state_value *lhs, struct state_value *rhs, bool *out)
{
	if (rhs->type != STATE_V_LIST) {
		fail_at(ctx, n->line, n->col,
		    "right side of 'in' must be a list");
		return (false);
	}

	*out = false;
	for (size_t i = 0; i < rhs->u.list.n; i++) {
		if (value_equal(lhs, rhs->u.list.items[i])) {
			*out = true;
			break;
		}
	}
	if (n->u.in.negated)
		*out = !*out;
	return (true);
}

static struct state_value *
eval_expr(struct dsl_eval_ctx *ctx, struct dsl_env *env, struct dsl_node *n)
{
	if (ctx->failed || n == NULL)
		return (NULL);

	switch (n->kind) {
	case DSL_N_NUM:
		return (state_value_number(n->u.num.n));
	case DSL_N_STR:
		return (state_value_string(n->u.str.s));
	case DSL_N_BOOL:
		return (state_value_bool(n->u.boolean.b));
	case DSL_N_NIL:
		return (state_value_nil());
	case DSL_N_PATH:
		return (eval_path(ctx, env, n));
	case DSL_N_LIST: {
		struct state_value *list = state_value_list();
		for (size_t i = 0; i < n->u.list.n; i++) {
			struct state_value *item = eval_expr(ctx, env,
			    n->u.list.items[i]);
			if (item == NULL) {
				state_value_unref(list);
				return (NULL);
			}
			state_list_append(list, item);
			state_value_unref(item);
		}
		return (list);
	}
	case DSL_N_NOT: {
		struct state_value *e = eval_expr(ctx, env, n->u.unop.e);
		if (e == NULL)
			return (NULL);
		struct state_value *r = state_value_bool(!state_value_truthy(e));
		state_value_unref(e);
		return (r);
	}
	case DSL_N_AND: {
		struct state_value *l = eval_expr(ctx, env, n->u.binop.lhs);
		if (l == NULL)
			return (NULL);
		if (!state_value_truthy(l)) {
			state_value_unref(l);
			return (state_value_bool(false));
		}
		state_value_unref(l);
		struct state_value *r = eval_expr(ctx, env, n->u.binop.rhs);
		if (r == NULL)
			return (NULL);
		bool rv = state_value_truthy(r);
		state_value_unref(r);
		return (state_value_bool(rv));
	}
	case DSL_N_OR: {
		struct state_value *l = eval_expr(ctx, env, n->u.binop.lhs);
		if (l == NULL)
			return (NULL);
		if (state_value_truthy(l)) {
			state_value_unref(l);
			return (state_value_bool(true));
		}
		state_value_unref(l);
		struct state_value *r = eval_expr(ctx, env, n->u.binop.rhs);
		if (r == NULL)
			return (NULL);
		bool rv = state_value_truthy(r);
		state_value_unref(r);
		return (state_value_bool(rv));
	}
	case DSL_N_CMP: {
		struct state_value *l = eval_expr(ctx, env, n->u.cmp.lhs);
		if (l == NULL)
			return (NULL);
		struct state_value *r = eval_expr(ctx, env, n->u.cmp.rhs);
		if (r == NULL) {
			state_value_unref(l);
			return (NULL);
		}
		bool result = false;
		bool ok	    = compare(ctx, n, l, r, &result);
		state_value_unref(l);
		state_value_unref(r);
		return (ok ? state_value_bool(result) : NULL);
	}
	case DSL_N_IN: {
		struct state_value *l = eval_expr(ctx, env, n->u.in.lhs);
		if (l == NULL)
			return (NULL);
		struct state_value *r = eval_expr(ctx, env, n->u.in.rhs);
		if (r == NULL) {
			state_value_unref(l);
			return (NULL);
		}
		bool result = false;
		bool ok	    = membership(ctx, n, l, r, &result);
		state_value_unref(l);
		state_value_unref(r);
		return (ok ? state_value_bool(result) : NULL);
	}
	default:
		fail_at(ctx, n->line, n->col, "not an expression");
		return (NULL);
	}
}

/* True when the script supplied its own -t. */
static bool
cmd_has_explicit_t(struct dsl_node *cmd_node)
{
	for (size_t i = 0; i < cmd_node->u.cmd.n; i++) {
		struct dsl_cmd_arg *a = &cmd_node->u.cmd.args[i];
		if (a->n == 1 && a->pieces[0].is_raw &&
		    strcmp(a->pieces[0].u.raw, "-t") == 0)
			return (true);
	}
	return (false);
}

static bool
enqueue_command(struct dsl_eval_ctx *ctx, struct dsl_env *env,
    struct dsl_node *cmd_node)
{
	size_t argc = 1 + cmd_node->u.cmd.n;
	char **argv = xcalloc(argc, sizeof(*argv));
	argv[0]	    = xstrdup(cmd_node->u.cmd.verb);

	for (size_t i = 0; i < cmd_node->u.cmd.n; i++) {
		struct dsl_cmd_arg *a = &cmd_node->u.cmd.args[i];

		/* Concatenate raw/interpolated pieces into one argv element. */
		size_t cap = 64;
		size_t len = 0;
		char  *buf = xmalloc(cap);
		buf[0]	   = '\0';

		for (size_t k = 0; k < a->n; k++) {
			struct dsl_arg_piece *p = &a->pieces[k];
			char		     *s;
			bool		      free_s = true;

			if (p->is_raw) {
				s      = p->u.raw;
				free_s = false;
			} else {
				struct state_value *v = eval_expr(ctx, env,
				    p->u.interp);
				if (v == NULL) {
					xfree(buf);
					goto fail;
				}
				s = state_value_to_argv_string(v);
				state_value_unref(v);
				if (s == NULL) {
					fail_at(ctx, cmd_node->line,
					    cmd_node->col,
					    "cannot splice list/record into "
					    "argv");
					xfree(buf);
					goto fail;
				}
			}
			size_t add = strlen(s);
			if (len + add + 1 > cap) {
				while (len + add + 1 > cap)
					cap *= 2;
				buf = xrealloc(buf, cap, 1);
			}
			memcpy(buf + len, s, add);
			len	 = len + add;
			buf[len] = '\0';
			if (free_s)
				xfree(s);
		}

		argv[i + 1] = buf;
	}

	char	   *cause = NULL;
	struct cmd *cmd	  = cmd_parse((int)argc, argv, NULL, 0, &cause);
	if (cmd == NULL) {
		fail_at(ctx, cmd_node->line, cmd_node->col, "%s",
		    cause != NULL ? cause : "command parse failed");
		xfree(cause);
		goto fail;
	}

	/* Window loops provide an implicit per-command target. */
	if (ctx->implicit_target_id != NULL && !cmd_has_explicit_t(cmd_node)) {
		struct cow_view *v = cow_view_find_by_identifier(
		    ctx->implicit_target_id);
		if (v != NULL) {
			cmd->default_target.window = v;
			cmd->default_target.desk   = v->desk;
			cmd->default_target.output = v->output;
		}
	}

	struct cmd_list *cl = xcalloc(1, sizeof(*cl));
	cl->references	    = 1;
	TAILQ_INIT(&cl->list);
	TAILQ_INSERT_TAIL(&cl->list, cmd, qentry);

	bool ok = cmdq_run_immediate(ctx->cmdq, cl);
	cmd_list_free(cl);

	for (size_t i = 0; i < argc; i++)
		xfree(argv[i]);
	xfree(argv);
	if (!ok) {
		fail_at(ctx, cmd_node->line, cmd_node->col,
		    "command failed: %s", cmd_node->u.cmd.verb);
		return (false);
	}
	return (true);

fail:
	for (size_t i = 0; i < argc; i++)
		xfree(argv[i]);
	xfree(argv);
	return (false);
}

static void exec_block(struct dsl_eval_ctx *, struct dsl_env *,
    struct dsl_node *);

static void
exec_stmt(struct dsl_eval_ctx *ctx, struct dsl_env *env, struct dsl_node *n)
{
	if (ctx->failed || ctx->control != DSL_CTL_NONE || n == NULL)
		return;

	switch (n->kind) {
	case DSL_N_LET: {
		struct state_value *v = eval_expr(ctx, env, n->u.let.value);
		if (v == NULL)
			return;
		env_set(env, n->u.let.name, v);
		state_value_unref(v);
		return;
	}
	case DSL_N_IF: {
		struct state_value *c = eval_expr(ctx, env, n->u.ifstmt.cond);
		if (c == NULL)
			return;
		bool t = state_value_truthy(c);
		state_value_unref(c);

		if (t) {
			exec_block(ctx, env, n->u.ifstmt.body);
		} else if (n->u.ifstmt.otherwise != NULL) {
			/* otherwise is either a block or another if. */
			if (n->u.ifstmt.otherwise->kind == DSL_N_BLOCK)
				exec_block(ctx, env, n->u.ifstmt.otherwise);
			else
				exec_stmt(ctx, env, n->u.ifstmt.otherwise);
		}
		return;
	}
	case DSL_N_FOR: {
		struct state_value *list = state_snapshot_collection(
		    state_source_from_dsl(n->u.forstmt.src));
		if (list == NULL)
			return;
		const char *saved_target = ctx->implicit_target_id;
		for (size_t i = 0; i < list->u.list.n; i++) {
			struct state_value *item = list->u.list.items[i];
			struct dsl_env	 *sub  = env_new(env);
			env_set(sub, n->u.forstmt.var, item);

			if (n->u.forstmt.where != NULL) {
				struct state_value *w = eval_expr(ctx, sub,
				    n->u.forstmt.where);
				if (w == NULL) {
					env_free(sub);
					break;
				}
				bool keep = state_value_truthy(w);
				state_value_unref(w);
				if (!keep) {
					env_free(sub);
					continue;
				}
			}

			/* Window iteration gets an implicit -t target. */
			if (n->u.forstmt.src == DSL_SRC_WINDOWS &&
			    item->type == STATE_V_RECORD) {
				struct state_value *idv = state_record_get(item,
				    "id");
				if (idv != NULL && idv->type == STATE_V_STRING &&
				    idv->u.s != NULL && idv->u.s[0] != '\0')
					ctx->implicit_target_id = idv->u.s;
				else
					ctx->implicit_target_id = saved_target;
			}

			ctx->loop_depth++;
			exec_block(ctx, sub, n->u.forstmt.body);
			ctx->loop_depth--;
			env_free(sub);
			if (ctx->failed)
				break;
			if (ctx->control == DSL_CTL_BREAK) {
				ctx->control = DSL_CTL_NONE;
				break;
			}
			if (ctx->control == DSL_CTL_CONTINUE) {
				ctx->control = DSL_CTL_NONE;
				continue;
			}
		}
		ctx->implicit_target_id = saved_target;
		state_value_unref(list);
		return;
	}
	case DSL_N_BREAK:
		if (ctx->loop_depth == 0) {
			fail_at(ctx, n->line, n->col,
			    "break outside loop");
			return;
		}
		ctx->control = DSL_CTL_BREAK;
		return;
	case DSL_N_CONTINUE:
		if (ctx->loop_depth == 0) {
			fail_at(ctx, n->line, n->col,
			    "continue outside loop");
			return;
		}
		ctx->control = DSL_CTL_CONTINUE;
		return;
	case DSL_N_CMD:
		enqueue_command(ctx, env, n);
		return;
	default:
		fail_at(ctx, n->line, n->col, "not a statement");
		return;
	}
}

static void
exec_block(struct dsl_eval_ctx *ctx, struct dsl_env *env, struct dsl_node *blk)
{
	if (blk == NULL || blk->kind != DSL_N_BLOCK)
		return;
	for (size_t i = 0; i < blk->u.block.n && !ctx->failed &&
	    ctx->control == DSL_CTL_NONE; i++)
		exec_stmt(ctx, env, blk->u.block.stmts[i]);
}

bool
dsl_eval(struct dsl_ast *ast, struct cmd_q *cmdq, struct dsl_error *err)
{
	if (ast == NULL || ast->root == NULL)
		return (true);
	if (ast->root->kind != DSL_N_BLOCK) {
		if (err != NULL) {
			err->file    = NULL;
			err->line    = ast->root->line;
			err->col     = ast->root->col;
			err->message = xstrdup("script root is not a block");
		}
		return (false);
	}

	struct dsl_eval_ctx ctx = { 0 };
	ctx.cmdq		= cmdq;
	ctx.err			= err;

	struct dsl_env *env = env_new(NULL);
	if (cmdq != NULL && cmdq->default_target.window != NULL) {
		struct state_value *self =
		    state_snapshot_view(cmdq->default_target.window);
		env_set(env, "self", self);
		state_value_unref(self);
	}
	exec_block(&ctx, env, ast->root);
	env_free(env);

	return (!ctx.failed);
}

bool
dsl_eval_record_expr(struct dsl_ast *ast, const struct state_value *record,
    bool *out, struct dsl_error *err)
{
	if (out == NULL)
		return (false);
	*out = false;
	if (ast == NULL || ast->root == NULL)
		return (true);

	struct dsl_eval_ctx ctx = { 0 };
	ctx.err			= err;

	struct dsl_env *env = env_new(NULL);
	if (record != NULL)
		env_set(env, "self", (struct state_value *)record);

	struct state_value *v = eval_expr(&ctx, env, ast->root);
	bool ok = v != NULL;
	if (v != NULL) {
		*out = state_value_truthy(v);
		state_value_unref(v);
	}
	env_free(env);

	return (!ctx.failed && ok);
}

bool
dsl_eval_view_expr(struct dsl_ast *ast, const struct cow_view *view,
    bool *out, struct dsl_error *err)
{
	struct state_value *record = state_snapshot_view(view);
	bool ok = dsl_eval_record_expr(ast, record, out, err);
	state_value_unref(record);
	return (ok);
}
