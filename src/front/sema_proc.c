/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * sema_proc.c - procedural types (§ 3.15 of the specification): the
 * profile of a routine and of a type, and the name of a routine as a
 * value. A name is a value only where a procedural type is expected; it
 * gives the routine of the name with the profile of that type (one at
 * most, § 8.8), which must be a routine of the level of a file: neither
 * nested, nor external, nor of the language, nor an operator. The value
 * is a constant (LXV_FUNC), known at compile time (§ 4.3).
 */
#include "sema.h"

#include "common/xalloc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool lxs_is_proc(const limba_lxs *S, limba_ltype t)
{
    return t && S->ts.t[t].kind == LIMBA_LTK_PROC;
}

static const char *mode_text(unsigned mode)
{
    return mode == LXS_VAR ? "var" : mode == LXS_OUT ? "out" : "without a mode";
}

bool lxs_profile(limba_lxs *S, limba_ltype sig, limba_ltype pt, char *why,
                 size_t cap)
{
    if (!sig || !pt)
        return true; /* reported already */
    const limba_typeinfo *a = lxs_ty(S, sig), *b = lxs_ty(S, pt);
    char ta[128], tb[128];
    if (why && cap)
        why[0] = 0;
    if (a->count != b->count) {
        if (why)
            snprintf(why, cap, "it has %u parameter%s, the type %u", a->count,
                     a->count == 1 ? "" : "s", b->count);
        return false;
    }
    for (uint32_t i = 0; i < a->count; i++) {
        limba_param p = S->ts.param[a->first + i];
        limba_param q = S->ts.param[b->first + i];
        if (p.mode != q.mode) {
            if (why)
                snprintf(why, cap, "its parameter %u is %s, the type's %s",
                         i + 1, mode_text(p.mode), mode_text(q.mode));
            return false;
        }
        if (!lxs_same_type(S, p.type, q.type)) {
            if (why)
                snprintf(why, cap, "its parameter %u is %s, the type's %s",
                         i + 1, lxs_tname(S, p.type, ta),
                         lxs_tname(S, q.type, tb));
            return false;
        }
    }
    bool fa = a->elem && a->elem != S->ts.void_;
    bool fb = b->elem && b->elem != S->ts.void_;
    if (fa != fb) {
        if (why)
            snprintf(why, cap, "it is a %s, the type a %s",
                     fa ? "function" : "procedure",
                     fb ? "function" : "procedure");
        return false;
    }
    if (fa && !lxs_same_type(S, a->elem, b->elem)) {
        if (why)
            snprintf(why, cap, "its result is %s, the type's %s",
                     lxs_tname(S, a->elem, ta), lxs_tname(S, b->elem, tb));
        return false;
    }
    return true;
}

limba_sym lxs_routine_name(limba_lxs *S, uint32_t scope, uint32_t node)
{
    if (!node || lxs_node(S, node)->kind != LXN_REF)
        return 0;
    limba_sym s = lxs_peek(S, scope, node);
    if (!s)
        return 0;
    unsigned k = S->st.sym[s].kind;
    return k == LIMBA_LSYM_ROUTINE || k == LIMBA_LSYM_BUILTIN ? s : 0;
}

/* the routines of the name s at node, the one with the profile of pt
   (0 if none); *n how many the name has, *only the one if alone */
static limba_sym pick(limba_lxs *S, uint32_t node, uint32_t scope, limba_sym s,
                      limba_ltype pt, uint32_t *n, limba_sym *only)
{
    limba_sym *c;
    uint32_t nc = lxs_ovl_meanings(S, node, scope, s, &c);
    if (!c) {
        c = limba_xmalloc(sizeof(*c));
        c[0] = s;
        nc = 1;
    }
    limba_sym got = 0;
    for (uint32_t i = 0; i < nc && !got; i++) {
        lxs_force(S, c[i]);
        if (S->st.sym[c[i]].type &&
            lxs_profile(S, S->st.sym[c[i]].type, pt, NULL, 0))
            got = c[i];
    }
    *n = nc;
    *only = nc == 1 ? c[0] : 0;
    free(c);
    return got;
}

bool lxs_proc_fits(limba_lxs *S, uint32_t node, limba_ltype pt)
{
    limba_sym s = S->sym[node];
    if (!s || !lxs_is_proc(S, pt))
        return false;
    if (S->st.sym[s].kind == LIMBA_LSYM_BUILTIN) {
        /* str of a type of the program (§ 9.4) */
        const limba_typeinfo *x = lxs_ty(S, pt);
        limba_sym m = S->st.sym[s].value == LXB_STR && x->count == 1
                          ? lxs_str_of(S, S->ts.param[x->first].type)
                          : 0;
        return m && lxs_profile(S, S->st.sym[m].type, pt, NULL, 0);
    }
    uint32_t n;
    limba_sym only;
    return pick(S, node, S->arg_scope, s, pt, &n, &only) != 0;
}

limba_ltype lxs_proc_value(limba_lxs *S, uint32_t node, uint32_t scope,
                           limba_sym s, limba_ltype expected)
{
    size_t n;
    const char *sp = lxs_spell(S, s, &n);
    char tb[128], why[300];
    if (!lxs_is_proc(S, expected)) {
        lxs_error(S, LXE_ROUTINE_VALUE, node,
                  "'%.*s' is a routine: call it with %.*s(...); as a value "
                  "it goes only where a procedural type is expected",
                  (int)n, sp, (int)n, sp);
        S->type[node] = 0;
        return 0;
    }
    limba_sym m = 0;
    if (S->st.sym[s].kind == LIMBA_LSYM_BUILTIN) {
        const limba_typeinfo *x = lxs_ty(S, expected);
        if (S->st.sym[s].value == LXB_STR && x->count == 1)
            m = lxs_str_of(S, S->ts.param[x->first].type);
        if (!m) {
            lxs_error(S, LXE_NOT_A_VALUE_ROUTINE, node,
                      "'%.*s' is a routine of the language: it is not a "
                      "value",
                      (int)n, sp);
            S->type[node] = 0;
            return 0;
        }
        if (!lxs_profile(S, S->st.sym[m].type, expected, why, sizeof(why))) {
            lxs_error(S, LXE_PROFILE, node,
                      "'%.*s' is not of the profile of %s: %s", (int)n, sp,
                      lxs_tname(S, expected, tb), why);
            S->type[node] = 0;
            return 0;
        }
    } else {
        uint32_t nc;
        limba_sym only;
        m = pick(S, node, scope, s, expected, &nc, &only);
        if (!m) {
            if (only) {
                lxs_profile(S, S->st.sym[only].type, expected, why,
                            sizeof(why));
                lxs_error(S, LXE_PROFILE, node,
                          "'%.*s' is not of the profile of %s: %s", (int)n, sp,
                          lxs_tname(S, expected, tb), why);
            } else {
                lxs_error(S, LXE_PROFILE, node,
                          "no '%.*s' has the profile of %s: there are %u",
                          (int)n, sp, lxs_tname(S, expected, tb), nc);
            }
            S->type[node] = 0;
            return 0;
        }
        const limba_lx_node *r = lxs_node(S, S->st.sym[m].node);
        const char *no = NULL;
        if (lxs_outer(S, m))
            no = "a routine nested in another is not a value: its "
                 "environment would have to go with it";
        else if (r->d && lxs_node(S, r->d)->kind == LXN_EXTERNAL)
            no = "a routine of C is not a value (routines go to C as values "
                 "later, as callbacks)";
        if (no) {
            lxs_error(S, LXE_NOT_A_VALUE_ROUTINE, node, "'%.*s': %s", (int)n,
                      sp, no);
            S->type[node] = 0;
            return 0;
        }
    }
    S->sym[node] = m;
    lxs_use_taken(S, m, node);
    LIMBA_GROW(S->ptaken, S->nptaken, S->capptaken);
    S->ptaken[S->nptaken++] = (limba_lxs_taken){expected, m, node};
    lxs_unit_named(S, scope, m);
    uint32_t v = lxs_value_new(S, LXV_FUNC);
    S->v[v].str = m;
    S->val[node] = v;
    S->type[node] = expected;
    return expected;
}
