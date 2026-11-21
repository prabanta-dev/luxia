/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * lower_exc.c - exceptions in the IR (§ 3.16, § 7.9, § 10.1 of the
 * specification; progetto_ir.md § 11i, doc/ir.md § 13 of Limba).
 *
 * The table of the exceptions of the module gives every run-time error
 * the name of its exception, so that one no handler takes stops the
 * program with it (luxia: IndexError: index out of range at ...), the
 * predefined families too, and the exceptions of the program from 1000.
 *
 * A try: its statements are blocks protected by a handler, the first
 * one entered by a jump from the block before. The handler takes the
 * three values of the occurrence and chooses the branch with a switch on
 * the identity (the families are known here); what no branch takes is
 * raised again. With a finally, a pad protects the statements (or the
 * handler and the branches), runs a copy of the finally and raises again;
 * the finally has a copy of its own on every other way out too (the end,
 * return, exit, continue), each protected by the handler around the try,
 * so that definite assignment after the try sees only the ways that get
 * there (§ 5.5).
 *
 * A handler sees the values variables have where the blocks it protects
 * begin, never what they assign (the SSA builder of Limba): so a scalar
 * assigned inside a try and declared outside it lives in a slot, with a
 * shadow SSA variable for its definite assignment (as a captured one,
 * lower_nest.c). Each try keeps for every shadow its value from before
 * the try, which defines the shadow again where the handler and the pad
 * begin: a variable assigned only inside the try is not assigned in a
 * handler nor in the finally (§ 5.5).
 *
 * The message of an occurrence is an SSA value: the engine holds its
 * reference, raise gives it away; nothing is released or retained here
 * (doc/ir.md § 12.1).
 *
 * A block of the heap the generator makes, an array with computed bounds
 * or a temporary of a comparison or an aggregate, has an implicit
 * finally that frees it (D157): its copies on the ways out are those of
 * free_dyns, and a pad frees it and raises again; the pad is made only
 * in a function under a handler (lxs_exc_handled), where an exception
 * may be taken, so that a program without try has the same IR.
 */
#include "lower.h"

#include "common/xalloc.h"

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
    return list ? L->S->t->node[list].b : 0;
}

static uint32_t list_at(const lxl *L, uint32_t list, uint32_t i)
{
    return limba_lx_list_at(L->S->t, list, i);
}

/* ---- the table ---- */

/* the predefined exceptions: identity (traps.def), name, family */
static const struct {
    int32_t id;
    const char *name;
    int32_t parent;
} predefined[] = {
    {LIMBA_FAMILY_CONSTRAINT, "ConstraintError", -1},
    {LIMBA_TRAP_OVERFLOW, "OverflowError", LIMBA_FAMILY_CONSTRAINT},
    {LIMBA_TRAP_DIVZERO, "DivisionError", LIMBA_FAMILY_CONSTRAINT},
    {LIMBA_TRAP_INDEX, "IndexError", LIMBA_FAMILY_CONSTRAINT},
    {LIMBA_TRAP_RANGE, "RangeError", LIMBA_FAMILY_CONSTRAINT},
    {LIMBA_TRAP_NIL, "NilError", LIMBA_FAMILY_CONSTRAINT},
    {LIMBA_TRAP_CONVERSION, "ConversionError", LIMBA_FAMILY_CONSTRAINT},
    {LIMBA_TRAP_SHIFT, "ShiftError", LIMBA_FAMILY_CONSTRAINT},
    {LIMBA_TRAP_DANGLING, "DanglingError", LIMBA_FAMILY_CONSTRAINT},
    {LIMBA_TRAP_LENGTH, "LengthError", LIMBA_FAMILY_CONSTRAINT},
    {LIMBA_FAMILY_STORAGE, "StorageError", -1},
    {LIMBA_TRAP_NOMEM, "StorageError", LIMBA_FAMILY_STORAGE},
    {LIMBA_TRAP_STACK, "StorageError", LIMBA_FAMILY_STORAGE},
    {LIMBA_FAMILY_PROGRAM, "ProgramError", -1},
    {LIMBA_TRAP_INVALID_FREE, "ProgramError", LIMBA_FAMILY_PROGRAM},
    {LIMBA_TRAP_INVALID_HANDLE, "ProgramError", LIMBA_FAMILY_PROGRAM},
    {LIMBA_FAMILY_INPUT_OUTPUT, "InputOutputError", -1},
    {LIMBA_TRAP_IO, "InputOutputError", LIMBA_FAMILY_INPUT_OUTPUT},
};

/* the name of exception t as e.name and the message say it: qualified by
   its unit for one declared in a unit (§ 3.16) */
static limba_id exc_name(lxl *L, limba_ltype t)
{
    limba_lxs *S = L->S;
    limba_sym s = ti(L, t)->name;
    size_t n = 0;
    const char *sp = s != UINT32_MAX ? lxs_spell(S, s, &n) : "";
    char buf[512];
    size_t len = 0;
    if (s != UINT32_MAX && S->nunits && S->st.sym[s].scope != S->universe) {
        bool impl;
        uint32_t u = lxs_unit_of(S, S->st.sym[s].scope, &impl);
        const limba_lx_node *r = nd(L, S->units[u].root);
        if (r->kind == LXN_UNIT) {
            const char *un = lxs_text_at(S, nd(L, r->a)->loc);
            uint32_t ul = lxs_ident_len(un);
            if (ul + 1 < sizeof(buf)) {
                memcpy(buf, un, ul);
                buf[ul] = '.';
                len = ul + 1;
            }
        }
    }
    if (len + n > sizeof(buf))
        n = sizeof(buf) - len;
    memcpy(buf + len, sp, n);
    return limba_str_intern(L->m, buf, len + n);
}

/* the exception type of identity id, 0 if none */
static limba_ltype exc_of_id(const lxl *L, int64_t id)
{
    const limba_types *ts = &L->S->ts;
    for (limba_ltype t = 1; t < ts->n; t++)
        if (ts->t[t].kind == LIMBA_LTK_EXCEPTION && ts->t[t].lo == id)
            return t;
    return 0;
}

/* the identities a handler may see: every exception of the program and
   predefined but StorageError, and the errors without an exception of
   their own, under their family */
static void make_idents(lxl *L)
{
    if (L->idents_made)
        return;
    L->idents_made = true;
    const limba_types *ts = &L->S->ts;
    uint32_t cap = 0;
    for (limba_ltype t = 1; t < ts->n; t++) {
        if (ts->t[t].kind != LIMBA_LTK_EXCEPTION ||
            lxs_exc_within(L->S, t, L->S->ty_storage))
            continue;
        LIMBA_GROW(L->idents, L->nidents, cap);
        L->idents[L->nidents++] =
            (struct lxl_ident){(int64_t)ts->t[t].lo, t, exc_name(L, t)};
    }
    static const struct {
        int id, family;
    } extra[] = {{LIMBA_TRAP_INVALID_FREE, LIMBA_FAMILY_PROGRAM},
                 {LIMBA_TRAP_INVALID_HANDLE, LIMBA_FAMILY_PROGRAM},
                 {LIMBA_TRAP_IO, LIMBA_FAMILY_INPUT_OUTPUT}};
    for (size_t i = 0; i < sizeof(extra) / sizeof(extra[0]); i++) {
        limba_ltype f = exc_of_id(L, extra[i].family);
        if (!f)
            continue;
        LIMBA_GROW(L->idents, L->nidents, cap);
        L->idents[L->nidents++] =
            (struct lxl_ident){extra[i].id, f, exc_name(L, f)};
    }
}

void lxl_exc_table(lxl *L)
{
    for (size_t i = 0; i < sizeof(predefined) / sizeof(predefined[0]); i++)
        limba_exception_add(L->m, predefined[i].id,
                            limba_str_intern(L->m, predefined[i].name,
                                             strlen(predefined[i].name)),
                            predefined[i].parent);
    /* those of the program, from 1000 (§ 3.16) */
    const limba_types *ts = &L->S->ts;
    for (limba_ltype t = 1; t < ts->n; t++) {
        const limba_typeinfo *x = &ts->t[t];
        if (x->kind != LIMBA_LTK_EXCEPTION || x->lo < 1000)
            continue;
        limba_exception_add(L->m, (int32_t)x->lo, exc_name(L, t),
                            x->elem ? (int32_t)ts->t[x->elem].lo : -1);
    }
}

/* ---- the variables a try assigns ---- */

static const char *const fields[] = {
#define LX_NODE(name, text, f) f,
#include "nodes.def"
#undef LX_NODE
};

typedef struct {
    uint32_t min;
    limba_sym *set;
    uint32_t n, cap;
} scan;

static void scan_node(lxl *L, scan *W, uint32_t node, unsigned depth)
{
    if (!node || depth > 4096)
        return;
    if (node < W->min)
        W->min = node;
    const limba_lx_node *x = nd(L, node);
    if (x->kind == LXN_LIST) {
        for (uint32_t i = 0; i < x->b; i++)
            scan_node(L, W, list_at(L, node, i), depth + 1);
        return;
    }
    if (x->kind == LXN_ASSIGN && nd(L, x->a)->kind == LXN_REF &&
        L->S->sym[x->a]) {
        LIMBA_GROW(W->set, W->n, W->cap);
        W->set[W->n++] = L->S->sym[x->a];
    }
    uint32_t v[4] = {x->a, x->b, x->c, x->d};
    const char *f = fields[x->kind];
    for (int i = 0; i < 4; i++)
        if (f[i] == 'n' || f[i] == 'l' || f[i] == 'q')
            scan_node(L, W, v[i], depth + 1);
}

void lxl_exc_scan(lxl *L)
{
    limba_lxs *S = L->S;
    scan W = {0};
    for (uint32_t n = 1; n < S->t->nnode; n++) {
        if (nd(L, n)->kind != LXN_TRY)
            continue;
        W.min = n;
        W.n = 0;
        scan_node(L, &W, n, 0);
        for (uint32_t i = 0; i < W.n; i++) {
            limba_sym s = W.set[i];
            const limba_symbol *y = &S->st.sym[s];
            bool local = y->kind == LIMBA_LSYM_VAR ||
                         (y->kind == LIMBA_LSYM_PARAM && y->mode == LXS_OUT);
            /* declared before the try: its nodes come before those of
               the try, which the parser made in order */
            if (local && (y->node < W.min || y->node > n))
                L->trysl[s] = 1;
        }
    }
    free(W.set);
}

/* ---- try ---- */

/* a handler: its three parameters first, protected by h */
static limba_id new_handler(lxl *L, limba_id h, limba_id p[3])
{
    limba_id b = limba_ssa_block(L->ssa);
    limba_func *f = limba_ssa_func(L->ssa);
    if (h != LIMBA_NONE)
        limba_block_set_handler(f, b, h);
    p[0] = limba_param_add(f, b, LIMBA_T_I32);
    p[1] = limba_param_add(f, b, LIMBA_T_I32);
    p[2] = limba_param_add(f, b, LIMBA_T_STR);
    return b;
}

/* a new block that is where the code goes now, sealed */
static void go_new(lxl *L)
{
    limba_id b = lxl_block(L);
    if (!limba_ssa_terminated(L->ssa, L->cur))
        limba_ssa_br(L->ssa, L->cur, b);
    limba_ssa_seal(L->ssa, b);
    L->cur = b;
}

/* where handler h begins: each shadow again its value from before the
   try (pre, one per variable of L->shadowed up to n) */
static void shadows_again(lxl *L, limba_id h, const uint32_t *pre, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        const lxl_store *st = &L->store[L->shadowed[i]];
        if (!st->shadow || pre[i] == UINT32_MAX)
            continue;
        limba_id v = limba_ssa_use(L->ssa, pre[i], h, UINT32_MAX);
        limba_ssa_def(L->ssa, st->shadow - 1, h, v);
    }
}

/* a copy of the finally fin where the code is now, protected by the
   handler h around its try */
static void finally_copy(lxl *L, uint32_t fin, limba_id h)
{
    limba_id keep = L->handler;
    L->handler = h;
    go_new(L);
    lxl_stmts(L, fin);
    L->handler = keep;
}

void lxl_leave_trys(lxl *L, uint32_t n)
{
    uint32_t ntrys = L->ntrys, noccs = L->noccs;
    limba_id handler = L->handler;
    /* the computed arrays, the try statements and the occurrences alive
       here: a finally copied below puts its own in their places, while
       the jump's own lists still need them (a return gives back the
       count of the arrays, whose guards its list closes) */
    uint32_t ndyns = L->ndyns;
    limba_sym *dyns = limba_xmalloc((ndyns ? ndyns : 1) * sizeof(*dyns));
    if (ndyns)
        memcpy(dyns, L->dyns, ndyns * sizeof(*dyns));
    lxl_try *trys = limba_xmalloc((ntrys ? ntrys : 1) * sizeof(*trys));
    if (ntrys)
        memcpy(trys, L->trys, ntrys * sizeof(*trys));
    lxl_occ *occs = limba_xmalloc((noccs ? noccs : 1) * sizeof(*occs));
    if (noccs)
        memcpy(occs, L->occs, noccs * sizeof(*occs));
    for (uint32_t i = ntrys; i-- > n;) {
        lxl_try T = L->trys[i];
        lxl_free_dyns(L, T.ndyn);
        if (L->ndyns > T.ndyn)
            L->ndyns = T.ndyn;
        L->ntrys = i;
        L->noccs = T.nocc;
        if (T.fin) {
            finally_copy(L, T.fin, T.outer);
            if (limba_ssa_terminated(L->ssa, L->cur)) {
                /* the finally raised: the jump is never reached */
                L->handler = T.outer;
                limba_id b = lxl_block(L);
                limba_ssa_seal(L->ssa, b);
                L->cur = b;
            }
        }
    }
    L->ntrys = ntrys;
    L->noccs = noccs;
    L->handler = handler;
    if (ndyns)
        memcpy(L->dyns, dyns, ndyns * sizeof(*dyns));
    if (ntrys)
        memcpy(L->trys, trys, ntrys * sizeof(*trys));
    if (noccs)
        memcpy(L->occs, occs, noccs * sizeof(*occs));
    free(dyns);
    free(trys);
    free(occs);
}

/* does branch h take exception t: one of its names is t or a family of
   it */
static bool takes(lxl *L, uint32_t h, limba_ltype t)
{
    uint32_t names = nd(L, h)->a;
    for (uint32_t k = 0; k < list_n(L, names); k++) {
        limba_ltype e = L->S->type[list_at(L, names, k)];
        if (e && lxs_exc_within(L->S, t, e))
            return true;
    }
    return false;
}

void lxl_try_stmt(lxl *L, uint32_t node)
{
    const limba_lx_node *x = nd(L, node);
    uint32_t body = x->a, handlers = x->b, other = x->c, fin = x->d;
    limba_id outer = L->handler;
    /* the shadows as they are before the try */
    uint32_t npre = L->nshadowed;
    uint32_t *pre = limba_xmalloc((npre + 1) * sizeof(*pre));
    for (uint32_t i = 0; i < npre; i++) {
        const lxl_store *st = &L->store[L->shadowed[i]];
        pre[i] = UINT32_MAX;
        if (!st->shadow)
            continue;
        pre[i] = limba_ssa_var(L->ssa, LIMBA_T_PTR);
        limba_ssa_def(
            L->ssa, pre[i], L->cur,
            limba_ssa_use(L->ssa, st->shadow - 1, L->cur, UINT32_MAX));
    }
    limba_id pp[3] = {0}, hp[3] = {0};
    limba_id pad = fin ? new_handler(L, outer, pp) : LIMBA_NONE;
    limba_id inner = fin ? pad : outer; /* around the branches */
    limba_id H = handlers ? new_handler(L, inner, hp) : LIMBA_NONE;
    limba_id normal = lxl_block(L); /* the end of every way, then finally */
    LIMBA_GROW(L->trys, L->ntrys, L->captrys);
    L->trys[L->ntrys++] = (lxl_try){fin, outer, L->ndyns, L->noccs};

    /* the statements */
    L->handler = H != LIMBA_NONE ? H : pad;
    go_new(L);
    lxl_stmts(L, body);
    if (!limba_ssa_terminated(L->ssa, L->cur))
        limba_ssa_br(L->ssa, L->cur, normal);

    /* the branches */
    if (H != LIMBA_NONE) {
        limba_ssa_seal(L->ssa, H);
        L->handler = inner;
        shadows_again(L, H, pre, npre);
        L->cur = H;
        uint32_t nb = list_n(L, handlers);
        limba_id *blk = limba_xmalloc((nb + 1) * sizeof(*blk));
        limba_id dflt = LIMBA_NONE, rr = LIMBA_NONE;
        uint32_t named = 0;
        for (uint32_t i = 0; i < nb; i++) {
            uint32_t h = list_at(L, handlers, i);
            blk[i] = lxl_block(L);
            if (!nd(L, h)->a)
                dflt = blk[i]; /* when var e: */
            else
                named++;
        }
        limba_id oth = other ? lxl_block(L) : LIMBA_NONE;
        if (oth != LIMBA_NONE)
            dflt = oth;
        if (dflt == LIMBA_NONE)
            dflt = rr = lxl_block(L);
        if (named) {
            make_idents(L);
            int64_t *vals = limba_xmalloc((L->nidents + 1) * sizeof(*vals));
            limba_id *tg = limba_xmalloc((L->nidents + 1) * sizeof(*tg));
            uint32_t n = 0;
            for (uint32_t k = 0; k < L->nidents; k++)
                for (uint32_t i = 0; i < nb; i++) {
                    uint32_t h = list_at(L, handlers, i);
                    if (nd(L, h)->a && takes(L, h, L->idents[k].exc)) {
                        vals[n] = L->idents[k].id;
                        tg[n++] = blk[i];
                        break;
                    }
                }
            limba_ssa_switch(L->ssa, H, hp[0], dflt, vals, tg, n);
            free(vals);
            free(tg);
        } else {
            limba_ssa_br(L->ssa, H, dflt);
        }
        for (uint32_t i = 0; i < nb; i++) {
            uint32_t h = list_at(L, handlers, i);
            limba_ssa_seal(L->ssa, blk[i]);
            L->cur = blk[i];
            limba_sym occ = nd(L, h)->b ? L->S->sym[nd(L, h)->b] : 0;
            if (occ) {
                lxl_store *st = &L->store[occ];
                st->kind = LXL_OCC;
                st->lo = hp[0];
                st->hi = hp[1];
                st->addr = hp[2];
            }
            LIMBA_GROW(L->occs, L->noccs, L->capoccs);
            L->occs[L->noccs++] = (lxl_occ){hp[0], hp[1], hp[2]};
            lxl_stmts(L, nd(L, h)->c);
            L->noccs--;
            if (!limba_ssa_terminated(L->ssa, L->cur))
                limba_ssa_br(L->ssa, L->cur, normal);
        }
        if (oth != LIMBA_NONE) {
            limba_ssa_seal(L->ssa, oth);
            L->cur = oth;
            LIMBA_GROW(L->occs, L->noccs, L->capoccs);
            L->occs[L->noccs++] = (lxl_occ){hp[0], hp[1], hp[2]};
            lxl_stmts(L, other);
            L->noccs--;
            if (!limba_ssa_terminated(L->ssa, L->cur))
                limba_ssa_br(L->ssa, L->cur, normal);
        }
        if (rr != LIMBA_NONE) {
            /* no branch takes it: on to the try around */
            limba_ssa_seal(L->ssa, rr);
            limba_ssa_raise(L->ssa, rr, hp[0], hp[1], hp[2]);
        }
        free(blk);
    }
    L->ntrys--;
    L->handler = outer;
    limba_ssa_seal(L->ssa, normal);

    /* the pad: the finally, then the same occurrence again */
    if (fin) {
        limba_ssa_seal(L->ssa, pad);
        shadows_again(L, pad, pre, npre);
        L->cur = pad;
        lxl_stmts(L, fin);
        if (!limba_ssa_terminated(L->ssa, L->cur))
            limba_ssa_raise(L->ssa, L->cur, pp[0], pp[1], pp[2]);
    }

    /* every way that gets past the try, then its finally */
    L->cur = normal;
    if (fin)
        finally_copy(L, fin, outer);
    free(pre);
}

/* ---- the blocks of the heap (D157) ---- */

uint32_t lxl_guard_begin(lxl *L, limba_id p, limba_ltype t, limba_id n)
{
    if (!L->fn_handled)
        return 0;
    struct lxl_guard G = {0};
    G.outer = L->handler;
    G.pad = new_handler(L, L->handler, G.pp);
    G.p = p;
    G.n = n;
    G.t = t;
    LIMBA_GROW(L->guards, L->nguards, L->capguards);
    L->guards[L->nguards++] = G;
    L->handler = G.pad;
    go_new(L);
    return L->nguards;
}

void lxl_guard_leave(lxl *L, uint32_t g)
{
    if (!g)
        return;
    L->handler = L->guards[g - 1].outer;
    go_new(L);
}

void lxl_guard_close(lxl *L, uint32_t g)
{
    if (!g)
        return;
    struct lxl_guard G = L->guards[g - 1];
    limba_ssa_seal(L->ssa, G.pad);
    limba_id cur = L->cur, handler = L->handler;
    L->cur = G.pad;
    L->handler = G.outer;
    if (G.t)
        lxl_rc(L, LIMBA_OP_RELEASE, G.p, G.t, G.n);
    limba_id p = G.p;
    lxl_rt(L, LIMBA_RT_MEM_FREE, LIMBA_T_VOID, &p, 1);
    limba_ssa_raise(L->ssa, L->cur, G.pp[0], G.pp[1], G.pp[2]);
    L->cur = cur;
    L->handler = handler;
}

void lxl_guard_end(lxl *L, uint32_t g)
{
    lxl_guard_leave(L, g);
    lxl_guard_close(L, g);
}

/* ---- raise ---- */

static limba_id sconst(lxl *L, const char *s, size_t n)
{
    return lxl_emit(L, LIMBA_OP_SCONST, LIMBA_T_STR, 0,
                    limba_str_intern(L->m, s, n), 0, NULL, 0);
}

void lxl_raise_stmt(lxl *L, uint32_t node)
{
    const limba_lx_node *x = nd(L, node);
    uint32_t exc = x->a, msg = x->b;
    limba_id id, pos, m;
    if (!exc) {
        /* raise;: the occurrence of the branch around (§ 7.9) */
        if (!L->noccs)
            return; /* reported */
        const lxl_occ *o = &L->occs[L->noccs - 1];
        id = o->id;
        pos = o->pos;
        m = o->msg;
    } else {
        limba_ltype t = L->S->type[exc];
        if (!t)
            return;
        int64_t code = ti(L, t)->lo;
        if (msg) {
            m = lxl_value(L, msg);
        } else {
            /* a predefined one without with: the text of its run-time
               error; a family and those of the program: empty */
            const char *text = code < 1000 ? limba_trap_text(code) : NULL;
            m = sconst(L, text ? text : "", text ? strlen(text) : 0);
        }
        lxl_at(L, node);
        id = lxl_iconst(L, LIMBA_T_I32, code);
        pos = lxl_iconst(L, LIMBA_T_I32,
                         L->node_pos[node] ? L->node_pos[node] - 1 : 0);
    }
    limba_ssa_raise(L->ssa, L->cur, id, pos, m);
    limba_id b = lxl_block(L);
    limba_ssa_seal(L->ssa, b);
    L->cur = b;
}

/* ---- the occurrence ---- */

limba_id lxl_occ_component(lxl *L, uint32_t node)
{
    const limba_lx_node *x = nd(L, node);
    limba_sym s = L->S->sym[x->a];
    const lxl_store *st = &L->store[s];
    size_t n;
    const char *f = lxs_name(L->S, x->b, &n);
    if (n == 7 && !memcmp(f, "message", 7))
        return st->addr;
    if (n == 8 && !memcmp(f, "position", 8)) {
        uint32_t p = st->hi;
        return lxl_rt(L, LIMBA_RT_POS_STR, LIMBA_T_STR, &p, 1);
    }
    /* name: a switch on the identity, to the names known here */
    make_idents(L);
    uint32_t var = limba_ssa_var(L->ssa, LIMBA_T_STR);
    limba_id join = lxl_block(L), dflt = lxl_block(L);
    int64_t *vals = limba_xmalloc((L->nidents + 1) * sizeof(*vals));
    limba_id *tg = limba_xmalloc((L->nidents + 1) * sizeof(*tg));
    for (uint32_t k = 0; k < L->nidents; k++) {
        vals[k] = L->idents[k].id;
        tg[k] = lxl_block(L);
    }
    limba_ssa_switch(L->ssa, L->cur, st->lo, dflt, vals, tg, L->nidents);
    for (uint32_t k = 0; k <= L->nidents; k++) {
        limba_id b = k < L->nidents ? tg[k] : dflt;
        limba_ssa_seal(L->ssa, b);
        L->cur = b;
        limba_id v = k < L->nidents ? lxl_emit(L, LIMBA_OP_SCONST, LIMBA_T_STR,
                                               0, L->idents[k].name, 0, NULL, 0)
                                    : sconst(L, "", 0);
        limba_ssa_def(L->ssa, var, b, v);
        limba_ssa_br(L->ssa, b, join);
    }
    limba_ssa_seal(L->ssa, join);
    L->cur = join;
    free(vals);
    free(tg);
    return limba_ssa_use(L->ssa, var, join, UINT32_MAX);
}
