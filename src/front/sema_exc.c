/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * sema_exc.c - exceptions (§ 3.16, § 7.9 of the specification): their
 * declarations, raise and raise;, try with its branches, else and
 * finally, and the occurrence a branch names. Definite assignment in a
 * try is checked on the IR, as everywhere (§ 5.5).
 */
#include "sema.h"

#include "common/xalloc.h"

#include <stdlib.h>
#include <string.h>

static unsigned kind_of(const limba_lxs *S, limba_ltype t)
{
    return lxs_ty(S, t)->kind;
}

bool lxs_exc_within(const limba_lxs *S, limba_ltype t, limba_ltype f)
{
    for (unsigned depth = 0; t && depth < 1024; depth++) {
        if (t == f)
            return true;
        t = lxs_ty(S, t)->elem;
    }
    return false;
}

/* the name node of the declaration of type symbol s */
static uint32_t decl_name(limba_lxs *S, limba_sym s)
{
    return lxs_node(S, S->st.sym[s].node)->a;
}

limba_ltype lxs_exc_type(limba_lxs *S, uint32_t node, uint32_t scope,
                         unsigned where, limba_sym self)
{
    if (!self || where != LXT_DECL) {
        lxs_error(S, LXE_EXCEPTION_PLACE, node,
                  "an exception is declared with a name, in a type section: "
                  "type ParseError = exception;");
        return 0;
    }
    if (S->st.scope[scope].kind != LXS_PROGRAM) {
        lxs_error(S, LXE_EXCEPTION_PLACE, decl_name(S, self),
                  "an exception is declared at the level of a file, not "
                  "inside a routine");
        return 0;
    }
    limba_ltype parent = 0;
    uint32_t pn = lxs_node(S, node)->a;
    if (pn) {
        limba_sym p = lxs_lookup(S, scope, pn);
        if (!p)
            return 0;
        limba_symbol *y = &S->st.sym[p];
        if (y->kind == LIMBA_LSYM_TYPE && (y->flags & LXS_RESOLVING) &&
            !y->type) {
            lxs_error(S, LXE_EXCEPTION_PLACE, pn,
                      "the parents of an exception form a circle");
            return 0;
        }
        if (y->kind == LIMBA_LSYM_TYPE)
            lxs_force(S, p);
        parent = S->st.sym[p].kind == LIMBA_LSYM_TYPE ? S->st.sym[p].type : 0;
        if (!parent && S->st.sym[p].kind == LIMBA_LSYM_TYPE)
            return 0; /* a mistake of its own, reported */
        if (!parent || kind_of(S, parent) != LIMBA_LTK_EXCEPTION) {
            size_t n;
            const char *sp = lxs_spell(S, p, &n);
            lxs_error(S, LXE_EXCEPTION_PLACE, pn,
                      "the parent of an exception is an exception, and "
                      "'%.*s' is not one",
                      (int)n, sp);
            return 0;
        }
        if (lxs_exc_within(S, parent, S->ty_storage)) {
            lxs_error(S, LXE_HANDLER, pn,
                      "an exception of the program cannot be of the family "
                      "of StorageError, which no handler takes");
            return 0;
        }
    }
    return limba_types_exception(&S->ts, 1000 + (int64_t)S->nexc++, parent);
}

/* the exception a TNAME of raise or of a branch names, 0 if none
   (reported); its type kept on the node */
static limba_ltype exception_at(limba_lxs *S, uint32_t node, uint32_t scope)
{
    limba_sym s = lxs_lookup(S, scope, node);
    if (!s)
        return 0;
    if (S->st.sym[s].kind == LIMBA_LSYM_TYPE)
        lxs_force(S, s);
    limba_ltype t =
        S->st.sym[s].kind == LIMBA_LSYM_TYPE ? S->st.sym[s].type : 0;
    if (!t && S->st.sym[s].kind == LIMBA_LSYM_TYPE)
        return 0; /* a mistake of its declaration, reported */
    if (!t || kind_of(S, t) != LIMBA_LTK_EXCEPTION) {
        size_t n;
        const char *sp = lxs_spell(S, s, &n);
        lxs_error(S, LXE_TYPE_MISMATCH, node, "'%.*s' is not an exception",
                  (int)n, sp);
        return 0;
    }
    S->sym[node] = s;
    S->type[node] = t;
    return t;
}

bool lxs_no_handlers(const limba_lxs *S, uint32_t node)
{
    if (!S->nunits)
        return S->no_handlers;
    uint32_t u = lxs_unit_at(S, node);
    if (S->units[u].library)
        return S->units[u].no_handlers;
    return S->no_handlers || S->units[u].no_handlers;
}

static const char *exc_name(limba_lxs *S, limba_ltype t, size_t *n)
{
    uint32_t s = lxs_ty(S, t)->name;
    if (s == UINT32_MAX) {
        *n = 1;
        return "?";
    }
    return lxs_spell(S, s, n);
}

typedef struct {
    limba_ltype t;
    uint32_t branch;
} named;

static void branch_names(limba_lxs *S, uint32_t h, uint32_t bi, uint32_t scope,
                         named **seen, uint32_t *nseen, uint32_t *capseen)
{
    uint32_t names = lxs_node(S, h)->a;
    for (uint32_t k = 0; k < lxs_node(S, names)->b; k++) {
        uint32_t en = limba_lx_list_at(S->t, names, k);
        limba_ltype t = exception_at(S, en, scope);
        if (!t)
            continue;
        size_t n, pn;
        const char *sp = exc_name(S, t, &n);
        if (lxs_exc_within(S, t, S->ty_storage)) {
            lxs_error(S, LXE_HANDLER, en,
                      "no handler takes %.*s: the program stops with it",
                      (int)n, sp);
            continue;
        }
        bool bad = false;
        for (uint32_t i = 0; i < *nseen && !bad; i++) {
            limba_ltype p = (*seen)[i].t;
            const char *pp = exc_name(S, p, &pn);
            bool same = (*seen)[i].branch == bi;
            if (p == t) {
                lxs_error(S, LXE_HANDLER, en,
                          same ? "%.*s is already named in this branch"
                               : "this branch is never reached: %.*s is "
                                 "already named in a branch before it",
                          (int)n, sp);
                bad = true;
            } else if (same &&
                       (lxs_exc_within(S, t, p) || lxs_exc_within(S, p, t))) {
                lxs_error(S, LXE_HANDLER, en,
                          "a family and its child in one branch: %.*s and "
                          "%.*s",
                          (int)pn, pp, (int)n, sp);
                bad = true;
            } else if (lxs_exc_within(S, t, p)) {
                lxs_error(S, LXE_HANDLER, en,
                          "this branch is never reached: %.*s, a branch "
                          "before it, takes %.*s",
                          (int)pn, pp, (int)n, sp);
                bad = true;
            }
        }
        if (!bad) {
            LIMBA_GROW(*seen, *nseen, *capseen);
            (*seen)[(*nseen)++] = (named){t, bi};
        }
    }
}

/* the statements of a branch of except: raise; allowed there; the
   occurrence, if named, declared in its scope */
static void branch_body(limba_lxs *S, uint32_t h, uint32_t occ, uint32_t body,
                        uint32_t scope)
{
    uint32_t inner = lxs_scope_new(S, scope, LXS_BLOCK);
    if (occ) {
        limba_sym s = lxs_declare(S, inner, occ, LIMBA_LSYM_VAR, h);
        if (s)
            S->st.sym[s].type = S->ts.occurrence;
    }
    S->branches++;
    lxs_stmts(S, body, inner);
    S->branches--;
}

void lxs_exc_try(limba_lxs *S, uint32_t node, uint32_t scope)
{
    limba_lx_node *x = lxs_node(S, node);
    uint32_t body = x->a, handlers = x->b, other = x->c, fin = x->d;
    if (lxs_no_handlers(S, node))
        lxs_error(S, LXE_NO_HANDLERS, node,
                  "pragma restrictions(no_handlers) forbids try");
    /* where the tries are: the routines under a handler start from them
       (lxs_exc_handled); one of the library is an error only if the
       program, under the restriction, reaches it (§ 10.4) */
    if (S->nunits) {
        if (S->who & LXS_INIT) {
            limba_lxs_unit *u = &S->units[S->who & ~LXS_INIT];
            if (!u->try_node)
                u->try_node = node;
            S->main_handled = true;
        } else if (S->who) {
            if (S->who >= S->ntry_at) {
                uint32_t n = S->who + 64;
                S->try_at = limba_xrealloc(S->try_at, n, sizeof(*S->try_at));
                memset(S->try_at + S->ntry_at, 0,
                       (n - S->ntry_at) * sizeof(*S->try_at));
                S->ntry_at = n;
            }
            if (!S->try_at[S->who])
                S->try_at[S->who] = node;
        }
    }
    lxs_stmts(S, body, lxs_scope_new(S, scope, LXS_BLOCK));
    named *seen = NULL;
    uint32_t nseen = 0, capseen = 0;
    uint32_t nh = handlers ? lxs_node(S, handlers)->b : 0;
    for (uint32_t i = 0; i < nh; i++) {
        uint32_t h = limba_lx_list_at(S->t, handlers, i);
        limba_lx_node *hx = lxs_node(S, h);
        uint32_t occ = hx->b, hbody = hx->c;
        if (!hx->a) {
            /* when var e: every exception left, the last branch */
            if (i + 1 < nh || other)
                lxs_error(S, LXE_HANDLER, h,
                          "when var e takes every exception the branches "
                          "before it do not: it is the last branch, without "
                          "else");
        } else {
            branch_names(S, h, i, scope, &seen, &nseen, &capseen);
        }
        branch_body(S, h, occ, hbody, scope);
    }
    free(seen);
    if (other) {
        S->branches++;
        lxs_stmts(S, other, lxs_scope_new(S, scope, LXS_BLOCK));
        S->branches--;
    }
    if (fin) {
        /* a finally runs to its end: return, and exit and continue of a
           loop around it, may not leave it */
        unsigned fin_loops = S->fin_loops;
        S->fin_loops = S->loops;
        S->in_finally++;
        lxs_stmts(S, fin, lxs_scope_new(S, scope, LXS_BLOCK));
        S->in_finally--;
        S->fin_loops = fin_loops;
    }
}

void lxs_exc_raise(limba_lxs *S, uint32_t node, uint32_t scope)
{
    limba_lx_node *x = lxs_node(S, node);
    uint32_t exc = x->a, msg = x->b;
    if (!exc) {
        if (!S->branches)
            lxs_error(S, LXE_RAISE_PLACE, node,
                      "raise without an exception raises the one of a "
                      "handler again: it goes only inside a branch of "
                      "except");
        return;
    }
    exception_at(S, exc, scope);
    if (msg) {
        lxs_expr(S, msg, scope, S->ts.string);
        lxs_assign_to(S, msg, S->ts.string, "the message");
    }
}

bool lxs_exc_handled_is(const limba_lxs *S, limba_sym s)
{
    return S->handled && S->handled[s];
}

void lxs_exc_handled(limba_lxs *S)
{
    uint32_t ns = S->st.nsym + 1;
    bool any = S->main_handled;
    for (limba_sym x = 1; x < S->ntry_at && x < ns; x++)
        any |= S->try_at[x] != 0;
    S->any_try = any;
    if (!any)
        return;
    S->handled = limba_xcalloc(ns, 1);
    for (limba_sym x = 1; x < S->ntry_at && x < ns; x++)
        if (S->try_at[x])
            S->handled[x] = 1;
    /* what they call, through all calls; a call through a value reaches
       every routine taken as a value (wider than Limba's set, which goes
       by signature: never narrower, or an array would be lost) */
    for (bool changed = true; changed;) {
        changed = false;
        for (uint32_t i = 0; i < S->nuses; i++) {
            const limba_lxs_use *u = &S->uses[i];
            bool from =
                u->who & LXS_INIT ? S->main_handled : S->handled[u->who];
            if (!from || u->taken ||
                S->st.sym[u->what].kind != LIMBA_LSYM_ROUTINE ||
                S->handled[u->what])
                continue;
            S->handled[u->what] = 1;
            changed = true;
        }
        for (uint32_t i = 0; i < S->nicalls; i++) {
            uint32_t w = S->icalls[i].who;
            bool from = w & LXS_INIT ? S->main_handled : S->handled[w];
            for (uint32_t k = 0; from && k < S->nptaken; k++) {
                limba_sym r = S->ptaken[k].routine;
                if (!S->handled[r]) {
                    S->handled[r] = 1;
                    changed = true;
                }
            }
        }
    }
}

limba_ltype lxs_exc_component(limba_lxs *S, uint32_t node, limba_sym s)
{
    uint32_t base = lxs_node(S, node)->a, field = lxs_node(S, node)->b;
    S->sym[base] = s;
    S->type[base] = S->ts.occurrence;
    size_t n;
    const char *f = lxs_name(S, field, &n);
    if ((n == 7 && !memcmp(f, "message", 7)) ||
        (n == 8 && !memcmp(f, "position", 8)) ||
        (n == 4 && !memcmp(f, "name", 4))) {
        S->type[node] = S->ts.string;
        return S->ts.string;
    }
    lxs_error(S, LXE_OCCURRENCE, node,
              "an occurrence has message, position and name only");
    S->type[node] = 0;
    return 0;
}
