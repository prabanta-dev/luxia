/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * sema_alias.c - one object given to a routine by two ways, one of which
 * can change it (§ 8.1 of the specification). A record or an array
 * without a mode is passed by address, so the routine would see the
 * change: where the text decides it, the call is an error (L0102):
 *  - the same variable, or a part of it, given to a var or out parameter
 *    and to another var or out parameter, or to a record or an array
 *    without a mode, of the same call: the designators are compared by
 *    their variable, fields and constant indices; an index computed at
 *    run time decides nothing, and the call stands;
 *  - a variable of a file given so to a routine that can write it: the
 *    call is kept here, and judged once every use is known
 *    (lxs_alias_globals in sema_unit.c, through the calls as the order of
 *    the initialisations follows them).
 * External routines and the routines of the language are not judged.
 */
#include "sema.h"

#include "common/xalloc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_STEPS 32

/* the steps of a designator, from its variable (or parameter) to the
   node itself: their number, 0 if it is no designator the compiler can
   follow (an expression, a call, anything through a pointer) */
static uint32_t steps_of(limba_lxs *S, uint32_t node, uint32_t *step)
{
    uint32_t n = 0;
    for (;;) {
        const limba_lx_node *x = lxs_node(S, node);
        if (n == MAX_STEPS)
            return 0;
        if (x->kind == LXN_REF) {
            limba_sym s = S->sym[node];
            if (!s || (S->st.sym[s].kind != LIMBA_LSYM_VAR &&
                       S->st.sym[s].kind != LIMBA_LSYM_PARAM))
                return 0;
            step[n++] = node;
            break;
        }
        if (x->kind != LXN_SEL && x->kind != LXN_INDEX)
            return 0;
        limba_ltype bt = S->type[x->a];
        if (!bt || S->ts.t[bt].kind == LIMBA_LTK_POINTER)
            return 0;
        step[n++] = node;
        node = x->a;
    }
    /* from the variable on */
    for (uint32_t i = 0; i < n / 2; i++) {
        uint32_t t = step[i];
        step[i] = step[n - 1 - i];
        step[n - 1 - i] = t;
    }
    return n;
}

/* a constant index: its value, NULL if computed at run time */
static const limba_lxs_value *const_index(limba_lxs *S, uint32_t node)
{
    uint32_t v = S->val[node];
    return v && S->v[v].kind == LXV_NUM ? &S->v[v] : NULL;
}

/* do two designators reach the same object, or one a part of the other,
   by what the text says */
static bool overlap(limba_lxs *S, const uint32_t *a, uint32_t na,
                    const uint32_t *b, uint32_t nb)
{
    if (S->sym[a[0]] != S->sym[b[0]])
        return false;
    for (uint32_t i = 1; i < na && i < nb; i++) {
        const limba_lx_node *x = lxs_node(S, a[i]), *y = lxs_node(S, b[i]);
        if (x->kind != y->kind)
            return false;
        if (x->kind == LXN_SEL) {
            if (x->b != y->b)
                return false;
            continue;
        }
        const limba_lxs_value *u = const_index(S, x->b);
        const limba_lxs_value *w = const_index(S, y->b);
        if (!u || !w || limba_rat_cmp(&u->num, &w->num) != 0)
            return false; /* not decided, or two elements */
    }
    return true;
}

/* passed by address: var, out, or a record or an array without a mode */
static bool by_address(limba_lxs *S, limba_param p)
{
    if (p.mode != LXS_IN)
        return true;
    unsigned k = S->ts.t[p.type].kind;
    return k == LIMBA_LTK_RECORD || k == LIMBA_LTK_ARRAY || k == LIMBA_LTK_OPEN;
}

/* a variable of a file: the program or a unit */
static bool file_var(limba_lxs *S, limba_sym s)
{
    const limba_symbol *y = &S->st.sym[s];
    if (y->kind != LIMBA_LSYM_VAR || !S->nunits)
        return false;
    bool impl;
    uint32_t u = lxs_unit_of(S, y->scope, &impl);
    return y->scope == S->units[u].intf || y->scope == S->units[u].impl;
}

static bool is_external(limba_lxs *S, limba_sym s)
{
    const limba_lx_node *r = lxs_node(S, S->st.sym[s].node);
    return r->d && lxs_node(S, r->d)->kind == LXN_EXTERNAL;
}

void lxs_alias_call(limba_lxs *S, limba_sym s, const uint32_t *given,
                    const uint32_t *pname, uint32_t first, uint32_t count)
{
    if (!count || is_external(S, s))
        return;
    uint32_t(*st)[MAX_STEPS] = limba_xcalloc(count, sizeof(*st));
    uint32_t *ns = limba_xcalloc(count, sizeof(*ns));
    size_t n;
    const char *rp = lxs_spell(S, s, &n);
    for (uint32_t j = 0; j < count; j++) {
        limba_param p = S->ts.param[first + j];
        if (!given[j] || !by_address(S, p))
            continue;
        ns[j] = steps_of(S, given[j], st[j]);
        if (!ns[j])
            continue;
        limba_sym v = S->sym[st[j][0]];
        if (file_var(S, v)) {
            LIMBA_GROW(S->galias, S->ngalias, S->capgalias);
            S->galias[S->ngalias++] =
                (limba_lxs_galias){S->who, s, v, given[j]};
        }
        for (uint32_t i = 0; i < j; i++) {
            limba_param q = S->ts.param[first + i];
            if (!ns[i] || (p.mode == LXS_IN && q.mode == LXS_IN) ||
                !overlap(S, st[i], ns[i], st[j], ns[j]))
                continue;
            uint32_t ch = p.mode != LXS_IN ? j : i;
            const char *pn =
                pname[ch] ? lxs_text_at(S, lxs_node(S, pname[ch])->loc) : "";
            size_t vn;
            const char *vp = lxs_spell(S, v, &vn);
            lxs_error(S, LXE_ALIASED, given[j],
                      "'%.*s' is given twice to '%.*s', which can change it "
                      "through '%.*s': pass a copy",
                      (int)vn, vp, (int)n, rp, (int)lxs_ident_len(pn), pn);
            break;
        }
    }
    free(st);
    free(ns);
}
