/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * sema_nested.c - routines nested in a routine of a file (§ 8.9 of the
 * specification): one level deep, neither external nor operators. A
 * nested routine is a routine of the scope of the routine around it; its
 * body is checked after the declarations of that routine, before its
 * statements. What a nested routine reads and writes of the variables
 * and parameters of the routine around it is kept: the generator puts
 * those in the environment, and judges definite assignment through the
 * calls (§ 5.5); the aliases are judged with the calls (§ 8.1).
 */
#include "sema.h"

#include "common/xalloc.h"

#include <stdlib.h>
#include <string.h>

limba_sym lxs_outer(const limba_lxs *S, limba_sym s)
{
    return s < S->nouter ? S->outer[s] : 0;
}

uint32_t lxs_outer_scope(const limba_lxs *S, limba_sym s)
{
    limba_sym o = lxs_outer(S, s);
    return o ? S->st.scope[S->st.sym[o].value].parent : 0;
}

static void set_outer(limba_lxs *S, limba_sym s, limba_sym r)
{
    if (s >= S->nouter) {
        uint32_t n = s + 256;
        S->outer = limba_xrealloc(S->outer, n, sizeof(*S->outer));
        memset(S->outer + S->nouter, 0, (n - S->nouter) * sizeof(*S->outer));
        S->nouter = n;
    }
    S->outer[s] = r;
}

void lxs_capture(limba_lxs *S, limba_sym s, uint32_t node, bool write)
{
    if (!S->nest || !s)
        return;
    const limba_symbol *y = &S->st.sym[s];
    if (y->kind != LIMBA_LSYM_VAR && y->kind != LIMBA_LSYM_PARAM)
        return;
    limba_sym o = lxs_outer(S, S->nest);
    if (!o || y->scope != S->st.sym[o].value)
        return;
    LIMBA_GROW(S->caps, S->ncaps, S->capcaps);
    S->caps[S->ncaps++] = (limba_lxs_capture){S->nest, s, node, write};
}

void lxs_nested_declared(limba_lxs *S, limba_sym r, uint32_t list)
{
    for (uint32_t i = 0; i < lxs_node(S, list)->b; i++) {
        uint32_t d = limba_lx_list_at(S->t, list, i);
        limba_lx_node *x = lxs_node(S, d);
        if (x->kind != LXN_ROUTINE || !S->sym[x->a])
            continue;
        limba_sym m = S->sym[x->a];
        if (lxs_outer(S, r)) {
            lxs_error(S, LXE_NESTED_ROUTINE, x->a,
                      "a nested routine contains no routine: routines nest "
                      "one level only");
            continue;
        }
        if (x->d && lxs_node(S, x->d)->kind == LXN_EXTERNAL) {
            lxs_error(S, LXE_NESTED_ROUTINE, x->a,
                      "a routine of C is declared at the level of a file, "
                      "not inside a routine");
            continue;
        }
        set_outer(S, m, r);
    }
}

void lxs_nested_bodies(limba_lxs *S, limba_sym r, uint32_t list, uint32_t scope)
{
    if (lxs_outer(S, r))
        return; /* reported: nothing is nested in a nested routine */
    bool any = false;
    for (uint32_t i = 0; i < lxs_node(S, list)->b && !any; i++) {
        uint32_t d = limba_lx_list_at(S->t, list, i);
        any = lxs_node(S, d)->kind == LXN_ROUTINE;
    }
    if (!any)
        return;
    lxs_ovl_check_list(S, list, scope);
    for (uint32_t i = 0; i < lxs_node(S, list)->b; i++) {
        uint32_t d = limba_lx_list_at(S->t, list, i);
        limba_lx_node *x = lxs_node(S, d);
        if (x->kind != LXN_ROUTINE || !S->sym[x->a] || !x->d)
            continue;
        limba_sym m = S->sym[x->a];
        if (lxs_outer(S, m) != r)
            continue;
        uint32_t who = S->who;
        limba_sym nest = S->nest;
        limba_ltype result = S->result;
        bool in_routine = S->in_routine;
        unsigned loops = S->loops;
        S->who = m;
        S->nest = m;
        lxs_routine_body(S, m);
        S->who = who;
        S->nest = nest;
        S->result = result;
        S->in_routine = in_routine;
        S->loops = loops;
    }
}
