/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * sema_overload.c - routines of one name (§ 8.8 of the specification):
 * the word overload on each, the bodies of an implementation bound to
 * their headings by the types of their parameters, two routines that the
 * same arguments by position would reach (an error at the second), and
 * the choice of the one routine a call takes.
 *
 * The first routine of a name is the symbol a lookup finds; the others
 * are symbols no lookup finds, chained after it (ovl_next).
 */
#include "sema.h"

#include "common/xalloc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

limba_sym lxs_ovl_next(const limba_lxs *S, limba_sym s)
{
    return s < S->novl ? S->ovl_next[s] : 0;
}

bool lxs_ovl_is(const limba_lxs *S, limba_sym s)
{
    const limba_symbol *y = &S->st.sym[s];
    if (y->kind != LIMBA_LSYM_ROUTINE || !y->node)
        return false;
    return (S->t->node[y->node].flags & LXN_F_OVERLOAD) != 0;
}

static void link_after(limba_lxs *S, limba_sym head, limba_sym s)
{
    if (S->st.nsym > S->novl) {
        uint32_t n = S->st.nsym + 256;
        S->ovl_next = limba_xrealloc(S->ovl_next, n, sizeof(*S->ovl_next));
        memset(S->ovl_next + S->novl, 0, (n - S->novl) * sizeof(*S->ovl_next));
        S->novl = n;
    }
    limba_sym t = head;
    while (S->ovl_next[t])
        t = S->ovl_next[t];
    S->ovl_next[t] = s;
}

static const char *name_text(limba_lxs *S, limba_sym s, uint32_t *n)
{
    size_t len;
    const char *sp = lxs_spell(S, s, &len);
    *n = (uint32_t)len;
    return sp;
}

/* another routine of the name of head, declared by d in scope */
static limba_sym member(limba_lxs *S, uint32_t scope, uint32_t d,
                        limba_sym head)
{
    limba_lx_node *x = lxs_node(S, d);
    limba_lx_node *nx = lxs_node(S, x->a);
    uint32_t n;
    const char *sp = name_text(S, head, &n);
    if (!(x->flags & LXN_F_OVERLOAD)) {
        lxs_error(S, LXE_OVERLOAD_MISSING, x->a,
                  "'%.*s' has another meaning here: write overload after "
                  "the heading of each",
                  (int)n, sp);
        lxs_note(S, S->st.sym[head].loc, S->st.sym[head].len, "the other one");
    } else if (!lxs_ovl_next(S, head) && !lxs_ovl_is(S, head)) {
        lxs_error(S, LXE_OVERLOAD_MISSING, lxs_node(S, S->st.sym[head].node)->a,
                  "'%.*s' has another meaning below: write overload after "
                  "the heading of each",
                  (int)n, sp);
    }
    limba_sym s =
        limba_sym_hidden(&S->st, scope, nx->a, LIMBA_LSYM_ROUTINE, nx->loc,
                         lxs_ident_len(lxs_text_at(S, nx->loc)));
    S->st.sym[s].node = d;
    S->sym[x->a] = s;
    link_after(S, head, s);
    return s;
}

bool lxs_ovl_declare(limba_lxs *S, uint32_t scope, uint32_t d, limba_sym *out)
{
    limba_lx_node *x = lxs_node(S, d);
    *out = 0;
    if (lxs_node(S, x->a)->kind != LXN_NAME)
        return false;
    uint32_t name = lxs_node(S, x->a)->a;
    if (S->nunits) {
        bool impl;
        uint32_t u = lxs_unit_of(S, scope, &impl);
        const limba_lxs_unit *f = &S->units[u];
        if (f->intf && scope == f->impl && x->d) {
            /* a body for a heading of the interface: which one, the types
               of the parameters say once every file is declared */
            limba_sym h = limba_sym_local(&S->st, f->intf, name);
            if (h && S->st.sym[h].kind == LIMBA_LSYM_ROUTINE &&
                ((x->flags & LXN_F_OVERLOAD) || lxs_ovl_is(S, h) ||
                 lxs_ovl_next(S, h))) {
                LIMBA_GROW(S->ovl_pend, S->npend, S->cappend);
                S->ovl_pend[S->npend++] = d;
                LIMBA_GROW(S->ovl_pend, S->npend, S->cappend);
                S->ovl_pend[S->npend++] = scope;
                return true;
            }
        }
    }
    limba_sym h = limba_sym_local(&S->st, scope, name);
    if (!h || S->st.sym[h].kind != LIMBA_LSYM_ROUTINE)
        return false;
    *out = member(S, scope, d, h);
    return true;
}

/* two parameter types one argument could go to: the same type (its
   subtypes the same), or open arrays that take the same arguments */
static bool same_param(limba_lxs *S, limba_ltype a, limba_ltype b)
{
    if (!a || !b)
        return false;
    bool ao = lxs_ty(S, a)->kind == LIMBA_LTK_OPEN;
    bool bo = lxs_ty(S, b)->kind == LIMBA_LTK_OPEN;
    if (ao || bo)
        return bo ? lxs_open_fits(S, a, b) : lxs_open_fits(S, b, a);
    return lxs_compatible(S, a, b);
}

/* the types of the parameters of routine node d, written in scope */
static uint32_t node_params(limba_lxs *S, uint32_t d, uint32_t scope,
                            limba_ltype **out)
{
    uint32_t plist = lxs_node(S, d)->b, n = 0, cap = 0;
    limba_ltype *ts = NULL;
    for (uint32_t i = 0; i < lxs_node(S, plist)->b; i++) {
        uint32_t p = limba_lx_list_at(S->t, plist, i);
        limba_ltype t = lxs_type(S, lxs_node(S, p)->b, scope, LXT_PARAM);
        for (uint32_t k = 0; k < lxs_node(S, lxs_node(S, p)->a)->b; k++) {
            LIMBA_GROW(ts, n, cap);
            ts[n++] = t;
        }
    }
    *out = ts;
    return n;
}

void lxs_ovl_bind(limba_lxs *S)
{
    for (uint32_t i = 0; i + 1 < S->npend; i += 2) {
        uint32_t d = S->ovl_pend[i], scope = S->ovl_pend[i + 1];
        limba_lx_node *x = lxs_node(S, d);
        bool impl;
        uint32_t u = lxs_unit_of(S, scope, &impl);
        uint32_t intf = S->units[u].intf;
        limba_sym h = limba_sym_local(&S->st, intf, lxs_node(S, x->a)->a),
                  found = 0;
        limba_ltype *bt = NULL;
        uint32_t bn = node_params(S, d, scope, &bt);
        for (limba_sym m = h; m && !found; m = lxs_ovl_next(S, m)) {
            if (S->st.sym[m].scope != intf || lxs_heading(S, m) ||
                lxs_node(S, S->st.sym[m].node)->d)
                continue;
            limba_ltype *ht = NULL;
            uint32_t hn = node_params(S, S->st.sym[m].node, intf, &ht);
            bool same = hn == bn;
            for (uint32_t k = 0; same && k < bn; k++)
                same = same_param(S, ht[k], bt[k]);
            free(ht);
            if (same)
                found = m;
        }
        free(bt);
        limba_sym s = found;
        if (found) {
            if (found >= S->nheading) {
                uint32_t n = found + 64;
                S->heading = limba_xrealloc(S->heading, n, sizeof(*S->heading));
                memset(S->heading + S->nheading, 0,
                       (n - S->nheading) * sizeof(*S->heading));
                S->nheading = n;
            }
            S->heading[found] = S->st.sym[found].node;
            S->st.sym[found].node = d;
            S->sym[x->a] = found;
        } else {
            /* no heading has these parameters: a routine of the
               implementation only */
            s = member(S, scope, d, h);
        }
        S->st.sym[s].flags |= LXS_UNRESOLVED | LXS_TOP;
    }
}

/* how many parameters a call must give: those before the first with a
   default value */
static uint32_t required(limba_lxs *S, limba_sym s)
{
    uint32_t d = S->st.sym[s].node, plist = lxs_node(S, d)->b, n = 0;
    for (uint32_t i = 0; i < lxs_node(S, plist)->b; i++) {
        uint32_t p = limba_lx_list_at(S->t, plist, i);
        if (lxs_node(S, p)->c)
            break;
        n += lxs_node(S, lxs_node(S, p)->a)->b;
    }
    return n;
}

/* the k from which the same arguments by position reach a and b, 0 if
   none (k is at least 1 then, or both take no argument: 0 + flag) */
static bool clash(limba_lxs *S, limba_sym a, limba_sym b, uint32_t *k)
{
    limba_ltype sa = S->st.sym[a].type, sb = S->st.sym[b].type;
    if (!sa || !sb)
        return false;
    const limba_typeinfo *ta = lxs_ty(S, sa), *tb = lxs_ty(S, sb);
    uint32_t ra = required(S, a), rb = required(S, b);
    uint32_t kmin = ra > rb ? ra : rb;
    uint32_t kmax = ta->count < tb->count ? ta->count : tb->count;
    if (kmin > kmax)
        return false;
    for (uint32_t i = 0; i < kmin; i++)
        if (!same_param(S, S->ts.param[ta->first + i].type,
                        S->ts.param[tb->first + i].type))
            return false;
    *k = kmin;
    return true;
}

/* does the routine h, alone in its file, join others of its name: one of
   an interface may join those of other units in the files that use it;
   one of a program or an implementation those of the units its file uses
   (§ 11.3) */
static bool joins(limba_lxs *S, limba_sym h)
{
    bool impl;
    uint32_t scope = S->st.sym[h].scope;
    uint32_t u = lxs_unit_of(S, scope, &impl);
    if (S->nunits && scope == S->units[u].intf)
        return true;
    limba_sym *heads;
    uint8_t *levels;
    uint32_t nh = lxs_unit_heads(S, scope, S->st.sym[h].name, &heads, &levels);
    bool any = false;
    for (uint32_t i = 0; i < nh && !any; i++)
        any = lxs_ovl_is(S, heads[i]);
    free(heads);
    free(levels);
    return any;
}

void lxs_ovl_check(limba_lxs *S)
{
    uint8_t *inner = limba_xcalloc(S->st.nsym + 1, 1);
    for (limba_sym s = 1; s < S->st.nsym && s < S->novl; s++)
        if (S->ovl_next[s])
            inner[S->ovl_next[s]] = 1;
    for (limba_sym h = 1; h < S->st.nsym; h++) {
        if (inner[h] || S->st.sym[h].kind != LIMBA_LSYM_ROUTINE)
            continue;
        if (!lxs_ovl_next(S, h)) {
            if (lxs_ovl_is(S, h) && !joins(S, h)) {
                uint32_t n;
                const char *sp = name_text(S, h, &n);
                limba_report_add(S->rep, LIMBA_WARNING, LXE_LONE_OVERLOAD,
                                 S->st.sym[h].loc, S->st.sym[h].len,
                                 "'%.*s' has no other meaning: overload is "
                                 "for a name with more",
                                 (int)n, sp);
            }
            continue;
        }
        for (limba_sym b = lxs_ovl_next(S, h); b; b = lxs_ovl_next(S, b))
            for (limba_sym a = h; a != b; a = lxs_ovl_next(S, a)) {
                uint32_t k = 0;
                if (!clash(S, a, b, &k))
                    continue;
                uint32_t n;
                const char *sp = name_text(S, b, &n);
                limba_report_add(
                    S->rep, LIMBA_ERROR, LXE_OVERLOAD_CLASH, S->st.sym[b].loc,
                    S->st.sym[b].len,
                    "'%.*s' cannot be told from another '%.*s' when called "
                    "with %u argument%s: their parameters have the same types",
                    (int)n, sp, (int)n, sp, k, k == 1 ? "" : "s");
                lxs_note(S, S->st.sym[a].loc, S->st.sym[a].len,
                         "the other one");
                break;
            }
        for (limba_sym m = h; m; m = lxs_ovl_next(S, m)) {
            uint32_t e = lxs_node(S, S->st.sym[m].node)->d;
            if (e && lxs_node(S, e)->kind == LXN_EXTERNAL &&
                !lxs_node(S, e)->c) {
                uint32_t n;
                const char *sp = name_text(S, m, &n);
                limba_report_add(S->rep, LIMBA_ERROR, LXE_OVERLOAD_EXTERNAL,
                                 S->st.sym[m].loc, S->st.sym[m].len,
                                 "'%.*s' has more meanings: give the C "
                                 "symbol with name \"symbol\"",
                                 (int)n, sp);
            }
        }
    }
    free(inner);
}

/* ---- the choice of a call ---- */

/* an aggregate written as a record: separated by ';', or each component
   a name of a field of record type t */
static bool record_form(limba_lxs *S, uint32_t agg, limba_ltype t)
{
    limba_lx_node *x = lxs_node(S, agg);
    if (x->flags & LXN_F_COMMA)
        return false;
    if (x->flags & LXN_F_SEMI)
        return true;
    const limba_typeinfo *r = lxs_ty(S, t);
    for (uint32_t i = 0; i < lxs_node(S, x->a)->b; i++) {
        limba_lx_node *c = lxs_node(S, limba_lx_list_at(S->t, x->a, i));
        if (!c->a || lxs_node(S, c->a)->kind != LXN_REF ||
            (lxs_node(S, c->a)->flags & LXN_F_QUAL))
            return false;
        bool field = false;
        for (uint32_t f = 0; f < r->count && !field; f++)
            field = S->ts.field[r->first + f].name == lxs_node(S, c->a)->a;
        if (!field)
            return false;
    }
    return true;
}

static bool arg_fits(limba_lxs *S, uint32_t v, limba_param p)
{
    unsigned pk = p.type ? lxs_ty(S, p.type)->kind : 0;
    if (!p.type)
        return true;
    if (lxs_node(S, v)->kind == LXN_AGG) {
        if (p.mode != LXS_IN)
            return false;
        if (pk == LIMBA_LTK_RECORD)
            return record_form(S, v, p.type);
        return (pk == LIMBA_LTK_ARRAY || pk == LIMBA_LTK_OPEN) &&
               !(lxs_node(S, v)->flags & LXN_F_SEMI);
    }
    limba_ltype at = S->type[v];
    if (pk == LIMBA_LTK_OPEN)
        return lxs_open_fits(S, at, p.type);
    bool untyped = at == S->ts.uint || at == S->ts.ureal;
    if (p.mode != LXS_IN)
        return !untyped && at != S->ts.nil && lxs_compatible(S, at, p.type);
    if (at == S->ts.uint)
        return pk == LIMBA_LTK_INT || pk == LIMBA_LTK_BIGINT ||
               pk == LIMBA_LTK_FLOAT;
    if (at == S->ts.ureal)
        return pk == LIMBA_LTK_FLOAT;
    if (at == S->ts.nil)
        return pk == LIMBA_LTK_POINTER || pk == LIMBA_LTK_OPAQUE;
    return lxs_compatible(S, at, p.type);
}

/* does the call node fit routine m: the rules of § 8.6 and § 8.7, and
   each argument goes to its parameter */
static bool fits(limba_lxs *S, uint32_t node, limba_sym m)
{
    limba_ltype sig = S->st.sym[m].type;
    if (!sig)
        return false;
    const limba_typeinfo *ti = lxs_ty(S, sig);
    uint32_t count = ti->count, first = ti->first;
    uint32_t args = lxs_node(S, node)->b, nargs = lxs_node(S, args)->b;
    uint32_t plist = lxs_node(S, S->st.sym[m].node)->b;
    uint32_t *pname = limba_xcalloc(count + 1, sizeof(*pname));
    uint8_t *given = limba_xcalloc(count + 1, 1);
    uint32_t k = 0;
    for (uint32_t i = 0; i < lxs_node(S, plist)->b; i++) {
        uint32_t p = limba_lx_list_at(S->t, plist, i);
        uint32_t names = lxs_node(S, p)->a;
        for (uint32_t j = 0; j < lxs_node(S, names)->b && k < count; j++)
            pname[k++] = limba_lx_list_at(S->t, names, j);
    }
    bool ok = true, named = false;
    uint32_t npos = 0;
    for (uint32_t i = 0; i < nargs && ok; i++) {
        uint32_t a = limba_lx_list_at(S->t, args, i);
        limba_lx_node *ax = lxs_node(S, a);
        uint32_t j = count, v = a;
        if (ax->kind != LXN_NAMED) {
            if (named)
                ok = false;
            j = npos++;
        } else {
            named = true;
            v = ax->b;
            for (j = 0; j < count; j++)
                if (ax->a && pname[j] && lxs_node(S, pname[j])->a == ax->a)
                    break;
        }
        if (!ok || j >= count || given[j]) {
            ok = false;
            break;
        }
        given[j] = 1;
        ok = arg_fits(S, v, S->ts.param[first + j]);
    }
    uint32_t req = required(S, m);
    for (uint32_t j = 0; ok && j < req; j++)
        ok = given[j];
    free(pname);
    free(given);
    return ok;
}

/* the parameters of m as a message says them: (Int32, Float64) */
const char *lxs_ovl_profile(limba_lxs *S, limba_sym m, char *buf, size_t cap)
{
    size_t at = 0;
    char tb[128];
    at += (size_t)snprintf(buf, cap, "(");
    limba_ltype sig = S->st.sym[m].type;
    const limba_typeinfo *ti = sig ? lxs_ty(S, sig) : NULL;
    for (uint32_t i = 0; ti && i < ti->count && at < cap; i++)
        at +=
            (size_t)snprintf(buf + at, cap - at, "%s%s", i ? ", " : "",
                             lxs_tname(S, S->ts.param[ti->first + i].type, tb));
    if (at < cap)
        snprintf(buf + at, cap - at, ")");
    return buf;
}

bool lxs_ovl_homograph(limba_lxs *S, limba_sym a, limba_sym b)
{
    lxs_force(S, a);
    lxs_force(S, b);
    limba_ltype sa = S->st.sym[a].type, sb = S->st.sym[b].type;
    if (!sa || !sb)
        return false;
    const limba_typeinfo *ta = lxs_ty(S, sa), *tb = lxs_ty(S, sb);
    if (ta->count != tb->count)
        return false;
    for (uint32_t i = 0; i < ta->count; i++)
        if (!same_param(S, S->ts.param[ta->first + i].type,
                        S->ts.param[tb->first + i].type))
            return false;
    return true;
}

typedef struct {
    limba_sym *m;
    uint8_t *level;
    uint32_t n, cap, capl;
} cands;

static void add_cand(cands *c, limba_sym m, uint8_t level)
{
    uint32_t n = c->n;
    LIMBA_GROW(c->m, c->n, c->cap);
    LIMBA_GROW(c->level, n, c->capl);
    c->m[c->n] = m;
    c->level[c->n++] = level;
}

/* the routines of the chain of h a file in unit cu sees: those of an
   implementation only there; a routine with the parameters of one of a
   level above is hidden by it (§ 11.3) */
static void add_chain(limba_lxs *S, cands *c, limba_sym h, uint8_t level,
                      uint32_t cu, uint32_t at)
{
    bool mi;
    for (limba_sym m = h; m; m = lxs_ovl_next(S, m)) {
        uint32_t mu = lxs_unit_of(S, S->st.sym[m].scope, &mi);
        if (mi && mu != cu && S->units[mu].intf)
            continue;
        bool hidden = false;
        for (uint32_t i = 0; i < c->n && !hidden; i++)
            if (c->level[i] < level && lxs_ovl_homograph(S, c->m[i], m)) {
                hidden = true;
                if (level == 2 && c->level[i] == 1 && at) {
                    uint32_t n;
                    const char *sp = name_text(S, m, &n);
                    limba_report_add(
                        S->rep, LIMBA_WARNING, LXE_HIDES, lxs_node(S, at)->loc,
                        n,
                        "'%.*s' of a unit of the program hides the one of "
                        "the library with the same parameters: write it "
                        "qualified",
                        (int)n, sp);
                }
            }
        if (!hidden)
            add_cand(c, m, level);
    }
}

limba_sym lxs_ovl_choose(limba_lxs *S, uint32_t node, uint32_t scope,
                         limba_sym s)
{
    uint32_t callee = lxs_node(S, node)->a;
    bool qual = lxs_node(S, callee)->kind == LXN_REF &&
                (lxs_node(S, callee)->flags & LXN_F_QUAL);
    bool merge = !qual && S->nunits && lxs_ovl_is(S, s);
    if (!merge && !lxs_ovl_next(S, s))
        return s;
    bool ci;
    uint32_t cu = lxs_unit_of(S, scope, &ci);
    cands C = {0};
    if (merge) {
        /* the routines of the name in the file and in the units it uses,
           one name when each says overload (§ 8.8, § 11.3) */
        uint32_t name = S->st.sym[s].name;
        limba_sym f = limba_sym_lookup(&S->st, scope, name);
        bool fi;
        if (f && S->st.sym[f].scope != S->universe &&
            lxs_unit_of(S, S->st.sym[f].scope, &fi) == cu)
            add_chain(S, &C, f, 0, cu, callee);
        limba_sym *heads;
        uint8_t *levels;
        uint32_t nh = lxs_unit_heads(S, scope, name, &heads, &levels);
        bool all = true;
        for (uint32_t i = 0; i < nh; i++)
            all = all && lxs_ovl_is(S, heads[i]);
        if (C.n || all)
            for (uint8_t lev = 1; lev <= 2 && all; lev++)
                for (uint32_t i = 0; i < nh; i++)
                    if (levels[i] == lev)
                        add_chain(S, &C, heads[i], lev, cu, callee);
        free(heads);
        free(levels);
        if (!all && !C.n)
            add_chain(S, &C, s, 0, cu, callee);
    } else {
        add_chain(S, &C, s, 0, cu, callee);
    }
    limba_sym *cand = C.m, *ok = NULL;
    uint32_t nc = 0, nall = C.n, capok = 0;
    free(C.level);
    limba_sym chosen = 0;
    if (nall == 1) {
        chosen = cand[0];
        free(cand);
        lxs_unit_named(S, scope, chosen);
        return chosen;
    }
    if (!nall) {
        free(cand);
        return s;
    }
    /* the arguments first, without a type to take: an aggregate waits
       for its parameter */
    uint32_t args = lxs_node(S, node)->b, nargs = lxs_node(S, args)->b;
    bool broken = false;
    for (uint32_t i = 0; i < nargs; i++) {
        uint32_t a = limba_lx_list_at(S->t, args, i);
        uint32_t v = lxs_node(S, a)->kind == LXN_NAMED ? lxs_node(S, a)->b : a;
        if (!v || lxs_node(S, v)->kind == LXN_AGG)
            continue;
        if (!S->pre[v]) {
            lxs_expr(S, v, scope, 0);
            S->pre[v] = 1;
        }
        if (!S->type[v])
            broken = true;
    }
    for (uint32_t i = 0; i < nall; i++)
        lxs_force(S, cand[i]);
    if (broken) {
        chosen = cand[0]; /* an argument already reported */
        free(cand);
        return chosen;
    }
    for (uint32_t i = 0; i < nall; i++)
        if (fits(S, node, cand[i])) {
            LIMBA_GROW(ok, nc, capok);
            ok[nc++] = cand[i];
        }
    if (nc == 1) {
        chosen = ok[0];
        free(cand);
        free(ok);
        lxs_unit_named(S, scope, chosen);
        return chosen;
    }
    uint32_t n;
    const char *sp = name_text(S, s, &n);
    char buf[400];
    if (!nc) {
        lxs_error(S, LXE_NO_MATCH, node,
                  "no '%.*s' takes these arguments: there are %u", (int)n, sp,
                  nall);
        for (uint32_t i = 0; i < nall && i < 8; i++)
            lxs_note(S, S->st.sym[cand[i]].loc, S->st.sym[cand[i]].len,
                     "'%.*s%s'", (int)n, sp,
                     lxs_ovl_profile(S, cand[i], buf, sizeof(buf)));
        free(cand);
        free(ok);
        return 0;
    }
    for (uint32_t i = 0; i < nc; i++)
        lxs_unit_named(S, scope, ok[i]); /* named, though ambiguous */
    lxs_error(S, LXE_AMBIGUOUS_CALL, node,
              "'%.*s' is ambiguous here: %u routines take these arguments; "
              "give a constant its type, as Int32(1)",
              (int)n, sp, nc);
    for (uint32_t i = 0; i < nc && i < 8; i++)
        lxs_note(S, S->st.sym[ok[i]].loc, S->st.sym[ok[i]].len, "'%.*s%s'",
                 (int)n, sp, lxs_ovl_profile(S, ok[i], buf, sizeof(buf)));
    free(cand);
    free(ok);
    return 0;
}
