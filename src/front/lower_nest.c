/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * lower_nest.c - routines nested in a routine (§ 8.9 of the
 * specification) in the IR, the IR unchanged.
 *
 * The variables and parameters of a routine that its nested routines
 * name (captured) live in its environment: one slot, typed when it holds
 * Strings, with a field per variable. A scalar or a record or an array of
 * fixed size is the field itself; what the routine has by address (a var
 * parameter, a record or an array parameter) is its address there; an
 * open array parameter its address and its bounds; an array with
 * computed bounds the same, written when its declaration runs (before,
 * an empty array). The address of the environment is the first parameter
 * of a nested routine, which reads the fields at its entry.
 *
 * Definite assignment (§ 5.5): a captured scalar the routine would keep
 * in SSA form keeps a shadow SSA variable (no instruction: each
 * definition is the address of the environment), defined by its assignments
 * and by the calls of the nested routines that assign it on every path,
 * and used by its reads and by the calls that read it: the SSA builder
 * reports what no definition reaches. What a nested routine reads before
 * it writes it, and writes on every path that returns, is computed here
 * on the tree, for all the nested routines of a routine together, as a
 * least fixed point (writes first, then reads).
 */
#include "lower.h"

#include "common/xalloc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const limba_typeinfo *ti(const lxl *L, limba_ltype t)
{
    return &L->S->ts.t[t];
}

static limba_lx_node *nd(const lxl *L, uint32_t node)
{
    return &L->S->t->node[node];
}

static uint32_t list_n(const lxl *L, uint32_t list)
{
    return list ? nd(L, list)->b : 0;
}

static uint32_t list_at(const lxl *L, uint32_t list, uint32_t i)
{
    return limba_lx_list_at(L->S->t, list, i);
}

/* how a captured variable is in the environment */
enum { CAP_VALUE, CAP_PTR, CAP_OPEN, CAP_DYN };

/* ---- the environments ---- */

static uint32_t cap_of(const lxl *L, limba_sym s)
{
    return s < L->ncapix ? L->capix[s] : 0;
}

static uint32_t nest_of(const lxl *L, limba_sym s)
{
    return s < L->nnestix ? L->nestix[s] : 0;
}

static void grow_ix(uint32_t **ix, uint32_t *n, uint32_t at)
{
    if (at < *n)
        return;
    uint32_t m = at + 256;
    *ix = limba_xrealloc(*ix, m, sizeof(**ix));
    memset(*ix + *n, 0, (m - *n) * sizeof(**ix));
    *n = m;
}

static int by_sym(const void *a, const void *b)
{
    const lxl_cap *x = a, *y = b;
    return x->var < y->var ? -1 : x->var > y->var;
}

/* the layout of the captured variables of each routine, and the nested
   routines with what each names itself */
static void layouts(lxl *L)
{
    limba_lxs *S = L->S;
    for (uint32_t i = 0; i < S->ncaps; i++) {
        const limba_lxs_capture *c = &S->caps[i];
        limba_sym o = lxs_outer(S, c->nested);
        if (!o || !S->st.sym[c->var].type)
            continue;
        grow_ix(&L->nestix, &L->nnestix, o);
        if (!L->nestix[o]) {
            LIMBA_GROW(L->envs, L->nenvs, L->capenvs);
            L->envs[L->nenvs] = (lxl_env){.outer = o, .type = LIMBA_NONE};
            L->nestix[o] = ++L->nenvs;
        }
        lxl_env *E = &L->envs[L->nestix[o] - 1];
        grow_ix(&L->capix, &L->ncapix, c->var);
        if (!L->capix[c->var]) {
            LIMBA_GROW(E->caps, E->ncaps, E->capcaps);
            E->caps[E->ncaps++] = (lxl_cap){.var = c->var, .track = -1};
            L->capix[c->var] = UINT32_MAX; /* numbered below */
        }
    }
    for (uint32_t e = 0; e < L->nenvs; e++) {
        lxl_env *E = &L->envs[e];
        qsort(E->caps, E->ncaps, sizeof(*E->caps), by_sym);
        uint64_t off = 0;
        uint32_t align = 8;
        for (uint32_t k = 0; k < E->ncaps; k++) {
            lxl_cap *c = &E->caps[k];
            const limba_symbol *y = &S->st.sym[c->var];
            limba_ltype t = y->type;
            const limba_typeinfo *x = ti(L, t);
            uint64_t size = 8, al = 8;
            if (x->kind == LIMBA_LTK_OPEN) {
                c->how = CAP_OPEN;
                size = 8 * (1 + 2 * (uint64_t)lxl_dims(L, t));
            } else if (x->flags & LIMBA_TF_DYNAMIC) {
                c->how = CAP_DYN;
                size = 8 * (4 + 2 * (uint64_t)(lxl_dims(L, t) - 1));
            } else if (y->kind == LIMBA_LSYM_PARAM && y->mode == LXS_VAR) {
                c->how = CAP_PTR;
            } else if (y->kind == LIMBA_LSYM_PARAM && !lxl_scalar(L, t)) {
                c->how = CAP_PTR;
            } else {
                c->how = CAP_VALUE;
                size = x->size ? x->size : 8;
                al = x->align ? x->align : 1;
            }
            off = (off + al - 1) / al * al;
            c->off = off;
            off += size;
            if (al > align)
                align = (uint32_t)al;
            /* followed by definite assignment: a scalar the routine
               would keep in SSA form, not a parameter without a mode */
            if (c->how == CAP_VALUE && lxl_scalar(L, t) && !L->taken[c->var] &&
                (y->kind == LIMBA_LSYM_VAR || y->mode == LXS_OUT))
                c->track = (int32_t)E->ntrack++;
            L->capix[c->var] = (e + 1) << 16 | (k + 1);
        }
        E->size = (off + align - 1) / align * align;
        E->align = align;
        E->nw = (E->ntrack + 63) / 64;
        if (!E->nw)
            E->nw = 1;
    }
    /* every nested routine of a routine with an environment: the calls
       among them carry what each reads and writes */
    for (limba_sym m = 1; m < S->st.nsym && m < S->nouter; m++) {
        limba_sym o = S->outer[m];
        if (!o || o >= L->nnestix || !L->nestix[o])
            continue;
        grow_ix(&L->nestix, &L->nnestix, m);
        if (!L->nestix[m]) {
            LIMBA_GROW(L->nests, L->nnests, L->capnests);
            L->nests[L->nnests] = (lxl_nest){.sym = m};
            L->nestix[m] = ++L->nnests;
        }
    }
    /* what each names */
    for (uint32_t i = 0; i < S->ncaps; i++) {
        const limba_lxs_capture *c = &S->caps[i];
        limba_sym n = c->nested;
        limba_sym o = lxs_outer(S, n);
        if (!o || !S->st.sym[c->var].type)
            continue;
        grow_ix(&L->nestix, &L->nnestix, n);
        lxl_nest *N;
        if (!L->nestix[n]) {
            LIMBA_GROW(L->nests, L->nnests, L->capnests);
            L->nests[L->nnests] = (lxl_nest){.sym = n};
            L->nestix[n] = ++L->nnests;
        }
        N = &L->nests[L->nestix[n] - 1];
        bool seen = false;
        for (uint32_t k = 0; k < N->nvars && !seen; k++)
            seen = N->vars[k] == c->var;
        if (!seen) {
            LIMBA_GROW(N->vars, N->nvars, N->capvars);
            N->vars[N->nvars++] = c->var;
        }
    }
}

/* the cap of a captured variable, NULL if none */
static lxl_cap *cap(const lxl *L, limba_sym s)
{
    uint32_t x = cap_of(L, s);
    if (!x || x == UINT32_MAX)
        return NULL;
    return &L->envs[(x >> 16) - 1].caps[(x & 0xffff) - 1];
}

/* the environment of routine o (an outer routine), NULL if none */
static lxl_env *env_of(const lxl *L, limba_sym o)
{
    uint32_t x = nest_of(L, o);
    if (!x || lxs_outer(L->S, o))
        return NULL;
    return &L->envs[x - 1];
}

bool lxl_has_env(const lxl *L, limba_sym n)
{
    limba_sym o = lxs_outer(L->S, n);
    return o && env_of(L, o) != NULL;
}

/* ---- the sets of the nested routines (§ 5.5), on the tree ---- */

typedef struct {
    uint64_t *exits, *conts;
} wloop;

typedef struct {
    lxl *L;
    lxl_env *E;
    limba_sym outer;
    uint32_t nw;
    uint64_t *d;     /* assigned here; all ones where no path reaches */
    uint64_t *reads; /* read before assigned */
    uint64_t *rets;  /* assigned at every return */
    wloop *loops;
    uint32_t nloops, caploops;
} walker;

static uint64_t *bs_new(const walker *W)
{
    return limba_xcalloc(W->nw, sizeof(uint64_t));
}

static uint64_t *bs_copy(const walker *W, const uint64_t *a)
{
    uint64_t *r = limba_xmalloc(W->nw * sizeof(uint64_t));
    memcpy(r, a, W->nw * sizeof(uint64_t));
    return r;
}

static void bs_all(const walker *W, uint64_t *a)
{
    memset(a, 0xff, W->nw * sizeof(uint64_t));
}

static void bs_and(const walker *W, uint64_t *a, const uint64_t *b)
{
    for (uint32_t i = 0; i < W->nw; i++)
        a[i] &= b[i];
}

static void bs_set(const walker *W, uint64_t *a, const uint64_t *b)
{
    memcpy(a, b, W->nw * sizeof(uint64_t));
}

/* the index of a tracked variable read or written at REF node, -1: a
   variable a nested routine names is one of the routine around it */
static int tracked(const walker *W, uint32_t node)
{
    limba_sym s = W->L->S->sym[node];
    lxl_cap *c = s ? cap(W->L, s) : NULL;
    return c ? c->track : -1;
}

static void read_at(walker *W, uint32_t node)
{
    int k = tracked(W, node);
    if (k >= 0 && !(W->d[k / 64] >> (k % 64) & 1))
        W->reads[k / 64] |= 1ull << (k % 64);
}

static void walk_expr(walker *W, uint32_t node);
static void walk_stmts(walker *W, uint32_t list);

/* a condition: the assigned when true into t, when false into f */
static void walk_cond(walker *W, uint32_t node, uint64_t *t, uint64_t *f)
{
    const limba_lx_node *x = nd(W->L, node);
    bool logic =
        (x->kind == LXN_BINARY && (x->op == LX_KW_AND || x->op == LX_KW_OR)) ||
        (x->kind == LXN_UNARY && x->op == LX_KW_NOT);
    if (logic && ti(W->L, W->L->S->type[node])->kind == LIMBA_LTK_BOOL &&
        !W->L->S->sym[node]) {
        if (x->kind == LXN_UNARY) {
            walk_cond(W, x->a, f, t);
            return;
        }
        uint64_t *ta = bs_new(W), *fa = bs_new(W);
        walk_cond(W, x->a, ta, fa);
        bs_set(W, W->d, x->op == LX_KW_AND ? ta : fa);
        walk_cond(W, nd(W->L, node)->b, t, f);
        if (x->op == LX_KW_AND)
            bs_and(W, f, fa);
        else
            bs_and(W, t, ta);
        free(ta);
        free(fa);
        return;
    }
    walk_expr(W, node);
    bs_set(W, t, W->d);
    bs_set(W, f, W->d);
}

static void walk_list(walker *W, uint32_t list)
{
    for (uint32_t i = 0; i < list_n(W->L, list); i++)
        walk_expr(W, list_at(W->L, list, i));
}

static void walk_call(walker *W, uint32_t node)
{
    lxl *L = W->L;
    limba_lxs *S = L->S;
    const limba_lx_node *x = nd(L, node);
    limba_sym s = S->sym[x->a];
    const limba_symbol *y = s ? &S->st.sym[s] : NULL;
    if (!y || (y->kind != LIMBA_LSYM_ROUTINE && y->kind != LIMBA_LSYM_BUILTIN &&
               y->kind != LIMBA_LSYM_TYPE))
        walk_expr(W, x->a); /* a call through a value: the value first */
    walk_list(W, x->b);
    if (!y)
        return;
    if (y->kind == LIMBA_LSYM_BUILTIN && y->value == LXB_HALT) {
        bs_all(W, W->d); /* the program ends here */
        return;
    }
    if (y->kind != LIMBA_LSYM_ROUTINE || lxs_outer(S, s) != W->outer)
        return;
    uint32_t ni = nest_of(L, s);
    if (!ni)
        return; /* names nothing of the routine around it */
    const lxl_nest *N = &L->nests[ni - 1];
    if (!N->writes)
        return;
    for (uint32_t i = 0; i < W->nw; i++) {
        W->reads[i] |= N->reads[i] & ~W->d[i];
        W->d[i] |= N->writes[i];
    }
}

static void walk_expr(walker *W, uint32_t node)
{
    lxl *L = W->L;
    if (!node)
        return;
    const limba_lx_node *x = nd(L, node);
    if (L->S->val[node] && x->kind != LXN_CALL)
        return; /* a constant */
    switch (x->kind) {
    case LXN_REF:
        read_at(W, node);
        return;
    case LXN_UNARY:
    case LXN_BINARY:
        if ((x->kind == LXN_UNARY && x->op == LX_KW_NOT) ||
            (x->kind == LXN_BINARY &&
             (x->op == LX_KW_AND || x->op == LX_KW_OR))) {
            if (ti(L, L->S->type[node])->kind == LIMBA_LTK_BOOL &&
                !L->S->sym[node]) {
                uint64_t *t = bs_new(W), *f = bs_new(W);
                walk_cond(W, node, t, f);
                bs_and(W, t, f);
                bs_set(W, W->d, t);
                free(t);
                free(f);
                return;
            }
        }
        walk_expr(W, x->a);
        if (x->kind == LXN_BINARY)
            walk_expr(W, nd(L, node)->b);
        return;
    case LXN_IN:
        walk_expr(W, x->a);
        walk_expr(W, nd(L, node)->b);
        walk_expr(W, nd(L, node)->c);
        return;
    case LXN_SEL:
    case LXN_DEREF:
        walk_expr(W, x->a);
        return;
    case LXN_INDEX:
        walk_expr(W, x->a);
        walk_expr(W, nd(L, node)->b);
        return;
    case LXN_CALL:
        walk_call(W, node);
        return;
    case LXN_FMT:
        walk_expr(W, x->a);
        walk_expr(W, nd(L, node)->b);
        walk_expr(W, nd(L, node)->c);
        return;
    case LXN_NAMED:
        walk_expr(W, x->b);
        return;
    case LXN_NEW: {
        const limba_lx_node *tn = nd(L, x->a);
        if ((x->flags & LXN_F_HEAP) && tn->kind == LXN_TNAME) {
            walk_expr(W, tn->b);
            walk_expr(W, nd(L, x->a)->c);
        }
        for (uint32_t i = 0; i < list_n(L, nd(L, node)->b); i++) {
            uint32_t r = list_at(L, nd(L, node)->b, i);
            walk_expr(W, nd(L, r)->a);
            walk_expr(W, nd(L, r)->b);
        }
        return;
    }
    case LXN_AGG:
        for (uint32_t i = 0; i < list_n(L, x->a); i++) {
            uint32_t c = list_at(L, nd(L, node)->a, i);
            walk_expr(W, nd(L, c)->c);
        }
        return;
    case LXN_LIST:
        walk_list(W, node);
        return;
    }
}

static wloop *push(walker *W)
{
    LIMBA_GROW(W->loops, W->nloops, W->caploops);
    wloop *lp = &W->loops[W->nloops++];
    lp->exits = bs_new(W);
    lp->conts = bs_new(W);
    bs_all(W, lp->exits);
    bs_all(W, lp->conts);
    return lp;
}

static void pop(walker *W)
{
    wloop *lp = &W->loops[--W->nloops];
    free(lp->exits);
    free(lp->conts);
}

static void walk_stmt(walker *W, uint32_t node)
{
    lxl *L = W->L;
    const limba_lx_node *x = nd(L, node);
    switch (x->kind) {
    case LXN_ASSIGN: {
        uint32_t target = x->a, value = x->b;
        if (nd(L, target)->kind != LXN_REF)
            walk_expr(W, target);
        walk_expr(W, value);
        if (nd(L, target)->kind == LXN_REF) {
            int k = tracked(W, target);
            if (k >= 0)
                W->d[k / 64] |= 1ull << (k % 64);
        }
        return;
    }
    case LXN_CALLST:
        walk_expr(W, x->a);
        return;
    case LXN_IF: {
        uint32_t arms = x->a, other = x->b;
        uint64_t *join = bs_new(W), *t = bs_new(W), *f = bs_new(W);
        bs_all(W, join);
        for (uint32_t i = 0; i < list_n(L, arms); i++) {
            uint32_t arm = list_at(L, arms, i);
            walk_cond(W, nd(L, arm)->a, t, f);
            bs_set(W, W->d, t);
            walk_stmts(W, nd(L, arm)->b);
            bs_and(W, join, W->d);
            bs_set(W, W->d, f);
        }
        walk_stmts(W, other);
        bs_and(W, join, W->d);
        bs_set(W, W->d, join);
        free(join);
        free(t);
        free(f);
        return;
    }
    case LXN_CASE: {
        walk_expr(W, x->a);
        /* without else the default of the switch goes to the end */
        uint32_t whens = x->b, other = x->c;
        uint64_t *from = bs_copy(W, W->d), *join = bs_copy(W, W->d);
        if (other)
            bs_all(W, join);
        for (uint32_t i = 0; i < list_n(L, whens); i++) {
            bs_set(W, W->d, from);
            walk_stmts(W, nd(L, list_at(L, whens, i))->b);
            bs_and(W, join, W->d);
        }
        if (other) {
            bs_set(W, W->d, from);
            walk_stmts(W, other);
            bs_and(W, join, W->d);
        }
        bs_set(W, W->d, join);
        free(from);
        free(join);
        return;
    }
    case LXN_WHILE: {
        uint64_t *t = bs_new(W), *f = bs_new(W);
        walk_cond(W, x->a, t, f);
        wloop *lp = push(W);
        (void)lp;
        bs_set(W, W->d, t);
        walk_stmts(W, nd(L, node)->b);
        bs_and(W, f, W->loops[W->nloops - 1].exits);
        pop(W);
        bs_set(W, W->d, f);
        free(t);
        free(f);
        return;
    }
    case LXN_REPEAT: {
        push(W);
        walk_stmts(W, x->a);
        bs_and(W, W->d, W->loops[W->nloops - 1].conts);
        uint64_t *t = bs_new(W), *f = bs_new(W);
        walk_cond(W, nd(L, node)->b, t, f);
        bs_and(W, t, W->loops[W->nloops - 1].exits);
        pop(W);
        bs_set(W, W->d, t);
        free(t);
        free(f);
        return;
    }
    case LXN_LOOP: {
        push(W);
        walk_stmts(W, x->a);
        bs_set(W, W->d, W->loops[W->nloops - 1].exits);
        pop(W);
        return;
    }
    case LXN_FOR: {
        uint32_t range = x->c;
        walk_expr(W, nd(L, range)->a);
        walk_expr(W, nd(L, range)->b);
        uint64_t *out = bs_copy(W, W->d);
        push(W);
        walk_stmts(W, nd(L, node)->d);
        pop(W);
        bs_set(W, W->d, out);
        free(out);
        return;
    }
    case LXN_EXIT:
    case LXN_CONTINUE: {
        if (!W->nloops)
            return;
        wloop *lp = &W->loops[W->nloops - 1];
        uint64_t *acc = x->kind == LXN_EXIT ? lp->exits : lp->conts;
        if (!x->a) {
            bs_and(W, acc, W->d);
            bs_all(W, W->d);
            return;
        }
        uint64_t *t = bs_new(W), *f = bs_new(W);
        walk_cond(W, x->a, t, f);
        lp = &W->loops[W->nloops - 1];
        bs_and(W, x->kind == LXN_EXIT ? lp->exits : lp->conts, t);
        bs_set(W, W->d, f);
        free(t);
        free(f);
        return;
    }
    case LXN_RETURN:
        walk_expr(W, x->a);
        bs_and(W, W->rets, W->d);
        bs_all(W, W->d);
        return;
    case LXN_VAR:
        walk_expr(W, x->c);
        return;
    }
}

static void walk_stmts(walker *W, uint32_t list)
{
    for (uint32_t i = 0; i < list_n(W->L, list); i++)
        walk_stmt(W, list_at(W->L, list, i));
}

/* one walk of nested routine N: what it reads before it assigns, what
   it assigns at every return */
static void walk_routine(lxl *L, lxl_env *E, lxl_nest *N, uint64_t *reads,
                         uint64_t *rets)
{
    walker W = {.L = L, .E = E, .outer = E->outer, .nw = E->nw};
    W.d = bs_new(&W);
    W.reads = reads;
    W.rets = rets;
    memset(reads, 0, W.nw * sizeof(uint64_t));
    bs_all(&W, rets);
    const limba_lx_node *r = nd(L, L->S->st.sym[N->sym].node);
    const limba_lx_node *b = nd(L, r->d);
    /* the initial values of its own variables, then its statements */
    for (uint32_t i = 0; i < list_n(L, b->a); i++) {
        uint32_t d = list_at(L, b->a, i);
        if (nd(L, d)->kind == LXN_VAR)
            walk_expr(&W, nd(L, d)->c);
    }
    walk_stmts(&W, b->b);
    bs_and(&W, rets, W.d);
    /* the bits past the variables */
    for (uint32_t k = E->ntrack; k < 64 * W.nw; k++)
        rets[k / 64] &= ~(1ull << (k % 64));
    free(W.d);
    free(W.loops);
}

static void sets(lxl *L, lxl_env *E)
{
    uint32_t nw = E->nw;
    /* the nested routines of E->outer that name something */
    lxl_nest **ns = NULL;
    uint32_t n = 0, cap = 0;
    for (uint32_t i = 0; i < L->nnests; i++)
        if (lxs_outer(L->S, L->nests[i].sym) == E->outer &&
            nd(L, L->S->st.sym[L->nests[i].sym].node)->d) {
            LIMBA_GROW(ns, n, cap);
            ns[n++] = &L->nests[i];
            L->nests[i].reads = limba_xcalloc(nw, sizeof(uint64_t));
            L->nests[i].writes = limba_xcalloc(nw, sizeof(uint64_t));
        }
    uint64_t *reads = limba_xcalloc(nw, sizeof(uint64_t));
    uint64_t *rets = limba_xcalloc(nw, sizeof(uint64_t));
    /* the writes from none, growing; then the reads, from none */
    for (int phase = 0; phase < 2; phase++)
        for (bool changed = true; changed;) {
            changed = false;
            for (uint32_t i = 0; i < n; i++) {
                walk_routine(L, E, ns[i], reads, rets);
                uint64_t *to = phase ? ns[i]->reads : ns[i]->writes;
                const uint64_t *from = phase ? reads : rets;
                for (uint32_t w = 0; w < nw; w++)
                    if ((to[w] | from[w]) != to[w]) {
                        to[w] |= from[w];
                        changed = true;
                    }
            }
        }
    free(reads);
    free(rets);
    free(ns);
}

void lxl_nest_prepare(lxl *L)
{
    if (!L->S->ncaps)
        return;
    layouts(L);
    for (uint32_t e = 0; e < L->nenvs; e++)
        sets(L, &L->envs[e]);
}

void lxl_nest_free(lxl *L)
{
    for (uint32_t e = 0; e < L->nenvs; e++)
        free(L->envs[e].caps);
    for (uint32_t i = 0; i < L->nnests; i++) {
        free(L->nests[i].vars);
        free(L->nests[i].reads);
        free(L->nests[i].writes);
    }
    free(L->envs);
    free(L->nests);
    free(L->capix);
    free(L->nestix);
    free(L->rplace);
}

/* ---- in the functions ---- */

/* an address in the environment, made in the entry block */
static limba_id field(lxl *L, uint32_t off)
{
    limba_id cur = L->cur;
    L->cur = 0;
    uint32_t o[2] = {L->env, lxl_iconst(L, LIMBA_T_I64, 0)};
    limba_id a =
        lxl_emit(L, LIMBA_OP_ADDR, LIMBA_T_PTR, 0, 0, (int64_t)off, o, 2);
    L->cur = cur;
    return a;
}

static void put(lxl *L, limba_id v, uint32_t off)
{
    uint32_t o[2] = {v, field(L, off)};
    lxl_emit(L, LIMBA_OP_STORE, LIMBA_T_VOID, 0, 0, 0, o, 2);
}

static limba_id get(lxl *L, limba_id type, uint32_t off)
{
    limba_id a = field(L, off);
    return lxl_emit(L, LIMBA_OP_LOAD, type, 0, 0, 0, &a, 1);
}

/* the IR type of the environment: its fields, typed for the Strings */
static limba_id env_type(lxl *L, lxl_env *E)
{
    if (E->type != LIMBA_NONE)
        return E->type;
    limba_lxs *S = L->S;
    limba_member *mem = NULL;
    uint32_t n = 0, cap_ = 0;
    for (uint32_t k = 0; k < E->ncaps; k++) {
        const lxl_cap *c = &E->caps[k];
        limba_ltype t = S->st.sym[c->var].type;
        uint32_t nf = 1;
        limba_id ft = LIMBA_T_PTR;
        if (c->how == CAP_VALUE)
            ft = lxl_type(L, t);
        else if (c->how == CAP_OPEN)
            nf = 1 + 2 * lxl_dims(L, t);
        else if (c->how == CAP_DYN)
            nf = 4 + 2 * (lxl_dims(L, t) - 1);
        for (uint32_t j = 0; j < nf; j++) {
            limba_id mt = j == 0 ? ft : LIMBA_T_I64;
            if (c->how == CAP_DYN && (j == 1 || j == 2))
                mt = lxl_type(L, ti(L, t)->index);
            LIMBA_GROW(mem, n, cap_);
            mem[n++] = (limba_member){mt, (uint32_t)(c->off + 8 * j)};
        }
    }
    const limba_func *of = &L->m->funcs[L->func_of[E->outer]];
    size_t ln;
    const char *on = limba_str(L->m, of->name, &ln);
    char buf[1100];
    for (unsigned k = 0; E->type == LIMBA_NONE && k < 1000; k++) {
        snprintf(buf, sizeof(buf), k ? "%.*s/$env%u" : "%.*s/$env", (int)ln, on,
                 k);
        E->type =
            limba_type_struct(L->m, limba_str_intern(L->m, buf, strlen(buf)),
                              mem, n, (uint32_t)E->size, E->align);
    }
    free(mem);
    return E->type;
}

void lxl_env_begin(lxl *L, limba_sym o)
{
    lxl_env *E = env_of(L, o);
    L->nrplace = 0;
    if (!E)
        return;
    limba_lxs *S = L->S;
    if (E->size > UINT32_MAX) {
        lxs_error(S, LXE_UNSUPPORTED, nd(L, S->st.sym[o].node)->a,
                  "the variables its nested routines name are too large for "
                  "the stack");
        E->size = 8;
    }
    limba_id t = env_type(L, E);
    bool counted = limba_type_holds_str(L->m, t);
    limba_id slot =
        limba_slot_add_typed(limba_ssa_func(L->ssa), (uint32_t)E->size,
                             E->align, counted ? t : LIMBA_NONE);
    limba_id cur = L->cur;
    L->cur = 0;
    L->env = lxl_emit(L, LIMBA_OP_SLOT, LIMBA_T_PTR, 0, slot, 0, NULL, 0);
    L->cur = cur;
    for (uint32_t k = 0; k < E->ncaps; k++) {
        const lxl_cap *c = &E->caps[k];
        lxl_store *st = &L->store[c->var];
        free(st->dims);
        *st = (lxl_store){0};
        if (c->how == CAP_VALUE) {
            st->kind = LXL_MEM;
            st->addr = field(L, c->off);
            if (c->track >= 0)
                st->shadow = limba_ssa_var(L->ssa, LIMBA_T_PTR) + 1;
        } else if (c->how == CAP_DYN) {
            /* an empty array until its declaration runs */
            limba_ltype it = ti(L, S->st.sym[c->var].type)->index;
            limba_id ity = lxl_type(L, it);
            put(L, lxl_iconst(L, ity, 1), c->off + 8);
            put(L, lxl_iconst(L, ity, 0), c->off + 16);
            for (uint32_t d = 1; d < lxl_dims(L, S->st.sym[c->var].type); d++) {
                put(L, lxl_iconst(L, LIMBA_T_I64, 1),
                    c->off + 32 + 16 * (d - 1));
                put(L, lxl_iconst(L, LIMBA_T_I64, 0),
                    c->off + 40 + 16 * (d - 1));
            }
        }
    }
}

bool lxl_env_value(const lxl *L, limba_sym s)
{
    const lxl_cap *c = L->env != LIMBA_NONE ? cap(L, s) : NULL;
    return c && c->how == CAP_VALUE;
}

void lxl_env_put(lxl *L, limba_sym s)
{
    const lxl_cap *c = L->env != LIMBA_NONE ? cap(L, s) : NULL;
    if (!c || c->how == CAP_VALUE)
        return;
    const lxl_store *st = &L->store[s];
    limba_ltype t = L->S->st.sym[s].type;
    put(L, st->addr, c->off);
    if (c->how == CAP_PTR)
        return;
    uint32_t dims = lxl_dims(L, t);
    put(L, st->lo, c->off + 8);
    put(L, st->hi, c->off + 16);
    uint32_t more = c->off + 24;
    if (c->how == CAP_DYN) {
        put(L, st->count, c->off + 24);
        more = c->off + 32;
    }
    for (uint32_t d = 0; d + 1 < dims; d++) {
        put(L, st->dims[2 * d], more + 16 * d);
        put(L, st->dims[2 * d + 1], more + 16 * d + 8);
    }
}

void lxl_nest_begin(lxl *L, limba_sym n)
{
    uint32_t ni = nest_of(L, n);
    if (!ni || L->env == LIMBA_NONE || !lxs_outer(L->S, n))
        return;
    const lxl_nest *N = &L->nests[ni - 1];
    limba_id cur = L->cur;
    L->cur = 0;
    for (uint32_t k = 0; k < N->nvars; k++) {
        limba_sym v = N->vars[k];
        const lxl_cap *c = cap(L, v);
        if (!c)
            continue;
        limba_ltype t = L->S->st.sym[v].type;
        lxl_store *st = &L->store[v];
        free(st->dims);
        *st = (lxl_store){0};
        st->kind = LXL_MEM;
        if (c->how == CAP_VALUE) {
            st->addr = field(L, c->off);
            continue;
        }
        st->addr = get(L, LIMBA_T_PTR, c->off);
        if (c->how == CAP_PTR)
            continue;
        uint32_t dims = lxl_dims(L, t);
        if (dims > 1)
            st->dims = limba_xmalloc(2 * (dims - 1) * sizeof(limba_id));
        uint32_t more = c->off + 24;
        if (c->how == CAP_OPEN) {
            st->kind = LXL_OPEN;
            st->lo = get(L, LIMBA_T_I64, c->off + 8);
            st->hi = get(L, LIMBA_T_I64, c->off + 16);
        } else {
            limba_id ity = lxl_type(L, ti(L, t)->index);
            st->kind = LXL_DYN;
            st->lo = get(L, ity, c->off + 8);
            st->hi = get(L, ity, c->off + 16);
            st->count = get(L, LIMBA_T_I64, c->off + 24);
            more = c->off + 32;
        }
        for (uint32_t d = 0; d + 1 < dims; d++) {
            st->dims[2 * d] = get(L, LIMBA_T_I64, more + 16 * d);
            st->dims[2 * d + 1] = get(L, LIMBA_T_I64, more + 16 * d + 8);
        }
    }
    L->cur = cur;
}

void lxl_shadow_def(lxl *L, limba_sym s)
{
    const lxl_store *st = &L->store[s];
    if (st->shadow)
        limba_ssa_def(L->ssa, st->shadow - 1, L->cur, L->env);
}

void lxl_shadow_use(lxl *L, limba_sym s, uint32_t tag)
{
    const lxl_store *st = &L->store[s];
    if (st->shadow)
        limba_ssa_use(L->ssa, st->shadow - 1, L->cur, tag);
}

/* the variable of tracked index k of environment E */
static limba_sym track_var(const lxl_env *E, uint32_t k)
{
    for (uint32_t i = 0; i < E->ncaps; i++)
        if (E->caps[i].track == (int32_t)k)
            return E->caps[i].var;
    return 0;
}

void lxl_nest_call(lxl *L, limba_sym n, uint32_t node, bool after)
{
    limba_sym o = lxs_outer(L->S, n);
    lxl_env *E = o ? env_of(L, o) : NULL;
    uint32_t ni = nest_of(L, n);
    if (!E || !ni || L->routine_sym != o)
        return; /* in a nested routine the sets say it (walk_call) */
    const lxl_nest *N = &L->nests[ni - 1];
    if (!N->writes)
        return;
    for (uint32_t k = 0; k < E->ntrack; k++) {
        bool r = N->reads[k / 64] >> (k % 64) & 1;
        bool w = N->writes[k / 64] >> (k % 64) & 1;
        limba_sym v = track_var(E, k);
        if (!after && r) {
            uint32_t at = L->nrplace / 3;
            LIMBA_GROW(L->rplace, L->nrplace, L->caprplace);
            L->rplace[L->nrplace++] = v;
            LIMBA_GROW(L->rplace, L->nrplace, L->caprplace);
            L->rplace[L->nrplace++] = n;
            LIMBA_GROW(L->rplace, L->nrplace, L->caprplace);
            L->rplace[L->nrplace++] = node;
            lxl_shadow_use(L, v, 0xc0000000u | at);
        }
        if (after && w)
            lxl_shadow_def(L, v);
    }
}

void lxl_nest_undefined(lxl *L, uint32_t k)
{
    limba_sym v = L->rplace[3 * k], n = L->rplace[3 * k + 1];
    size_t vn, nn;
    const char *vp = lxs_spell(L->S, v, &vn);
    const char *np = lxs_spell(L->S, n, &nn);
    lxs_error(L->S, LXE_UNASSIGNED, L->rplace[3 * k + 2],
              "'%.*s' may be read by '%.*s' before it is given a value",
              (int)vn, vp, (int)nn, np);
}
