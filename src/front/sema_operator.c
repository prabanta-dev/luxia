/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * sema_operator.c - operators defined by a program (§ 6.9 of the
 * specification): function "+"(a, b: Vector): Vector. An operator stands
 * in the part of a file of the type of one of its operands and holds
 * wherever values of that type are: it is no name a lookup finds, but
 * one of the list ops, which a binary or unary expression searches by
 * its operator. <> is not (a = b); a defined operator with the operands
 * of the type the predefined one works in hides it.
 */
#include "sema.h"

#include "common/xalloc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* the operators that can be defined, as written between the quotes */
static const struct {
    const char *text;
    unsigned op;
    uint8_t arity; /* 1, 2, or 3 for both */
} defined[] = {
    {"+", LX_PLUS, 3},     {"-", LX_MINUS, 3},    {"*", LX_STAR, 2},
    {"/", LX_SLASH, 2},    {"div", LX_KW_DIV, 2}, {"mod", LX_KW_MOD, 2},
    {"rem", LX_KW_REM, 2}, {"**", LX_POWER, 2},   {"&", LX_AMP, 2},
    {"shl", LX_KW_SHL, 2}, {"shr", LX_KW_SHR, 2}, {"xor", LX_KW_XOR, 2},
    {"=", LX_EQ, 2},       {"<", LX_LT, 2},       {"<=", LX_LE, 2},
    {">", LX_GT, 2},       {">=", LX_GE, 2},      {"not", LX_KW_NOT, 1},
    {"abs", LX_KW_ABS, 1},
};

/* the operator a NAME node with quotes names: its index, -1 if none */
static int op_index(limba_lxs *S, uint32_t name_node)
{
    size_t n;
    const char *s = lxs_name(S, lxs_node(S, name_node)->a, &n);
    if (n < 2)
        return -1;
    for (size_t i = 0; i < sizeof(defined) / sizeof(*defined); i++)
        if (strlen(defined[i].text) == n - 2 &&
            !memcmp(defined[i].text, s + 1, n - 2))
            return (int)i;
    return -1;
}

bool lxs_op_is(const limba_lxs *S, limba_sym s)
{
    const limba_symbol *y = &S->st.sym[s];
    if (y->kind != LIMBA_LSYM_ROUTINE || !y->node)
        return false;
    uint32_t nn = S->t->node[y->node].a;
    return S->t->node[nn].kind == LXN_NAME &&
           (S->t->node[nn].flags & LXN_F_OPNAME);
}

static limba_sym add_op(limba_lxs *S, uint32_t scope, uint32_t d)
{
    limba_lx_node *nx = lxs_node(S, lxs_node(S, d)->a);
    limba_sym s = limba_sym_hidden(&S->st, scope, nx->a, LIMBA_LSYM_ROUTINE,
                                   nx->loc, nx->b);
    S->st.sym[s].node = d;
    S->sym[lxs_node(S, d)->a] = s;
    int k = op_index(S, lxs_node(S, d)->a);
    uint32_t n = S->nops;
    LIMBA_GROW(S->ops, S->nops, S->capops);
    LIMBA_GROW(S->opk, n, S->capopk);
    S->ops[S->nops] = s;
    S->opk[S->nops++] = (uint8_t)(k < 0 ? 0 : defined[k].op);
    return s;
}

bool lxs_op_declare(limba_lxs *S, uint32_t scope, uint32_t d, limba_sym *out)
{
    limba_lx_node *x = lxs_node(S, d);
    limba_lx_node *nx = lxs_node(S, x->a);
    *out = 0;
    if (nx->kind != LXN_NAME || !(nx->flags & LXN_F_OPNAME))
        return false;
    if (S->nunits && x->d) {
        bool impl;
        uint32_t u = lxs_unit_of(S, scope, &impl);
        if (S->units[u].intf && scope == S->units[u].impl) {
            /* a body: for an operator of the interface, which one the
               types of the parameters say once every file is declared */
            LIMBA_GROW(S->op_pend, S->nop_pend, S->capop_pend);
            S->op_pend[S->nop_pend++] = d;
            LIMBA_GROW(S->op_pend, S->nop_pend, S->capop_pend);
            S->op_pend[S->nop_pend++] = scope;
            return true;
        }
    }
    *out = add_op(S, scope, d);
    return true;
}

void lxs_op_bind(limba_lxs *S)
{
    for (uint32_t i = 0; i + 1 < S->nop_pend; i += 2) {
        uint32_t d = S->op_pend[i], scope = S->op_pend[i + 1];
        uint32_t name = lxs_node(S, lxs_node(S, d)->a)->a;
        bool impl;
        uint32_t u = lxs_unit_of(S, scope, &impl);
        uint32_t intf = S->units[u].intf;
        limba_ltype *bt = NULL;
        uint32_t bn = lxs_node_params(S, d, scope, &bt);
        limba_sym found = 0;
        for (uint32_t k = 0; k < S->nops && !found; k++) {
            limba_sym m = S->ops[k];
            if (S->st.sym[m].scope != intf || S->st.sym[m].name != name ||
                lxs_heading(S, m) || lxs_node(S, S->st.sym[m].node)->d)
                continue;
            limba_ltype *ht = NULL;
            uint32_t hn = lxs_node_params(S, S->st.sym[m].node, intf, &ht);
            bool same = hn == bn;
            for (uint32_t j = 0; same && j < bn; j++)
                same = lxs_same_param(S, ht[j], bt[j]);
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
            S->sym[lxs_node(S, d)->a] = found;
        } else {
            s = add_op(S, scope, d);
        }
        S->st.sym[s].flags |= LXS_UNRESOLVED | LXS_TOP;
    }
}

/* the part of a file a type is declared in: the scope of its TYPE
   symbol, 0 for a type of the language or one with no name */
static uint32_t type_scope(limba_lxs *S, limba_ltype t)
{
    if (!t)
        return 0;
    for (limba_sym s = 1; s < S->st.nsym; s++) {
        const limba_symbol *y = &S->st.sym[s];
        if (y->kind == LIMBA_LSYM_TYPE && y->type == t &&
            y->scope != S->universe)
            return y->scope;
    }
    return 0;
}

static const char *op_text(limba_lxs *S, limba_sym s, uint32_t *n)
{
    size_t len;
    const char *sp = lxs_spell(S, s, &len);
    *n = (uint32_t)len;
    return sp;
}

static bool relation(unsigned op)
{
    return op == LX_EQ || op == LX_LT || op == LX_LE || op == LX_GT ||
           op == LX_GE;
}

void lxs_op_check(limba_lxs *S)
{
    for (uint32_t i = 0; i < S->nops; i++) {
        limba_sym s = S->ops[i];
        const limba_symbol *y = &S->st.sym[s];
        uint32_t d = y->node;
        limba_lx_node *x = lxs_node(S, d);
        uint32_t n;
        const char *sp = op_text(S, s, &n);
        int k = op_index(S, x->a);
        const char *why = NULL;
        char buf[200];
        if (k < 0) {
            why = "this operator cannot be defined (and, or, in and <> are "
                  "not; <> is not of =)";
        } else if (x->op != LX_KW_FUNCTION) {
            why = "an operator is a function";
        } else if (x->flags & LXN_F_OVERLOAD) {
            why = "an operator takes no overload: it always has more "
                  "meanings";
        }
        limba_ltype sig = y->type;
        const limba_typeinfo *ti = sig ? lxs_ty(S, sig) : NULL;
        if (!why && ti) {
            uint8_t ar = defined[k].arity;
            if (!((ar & 1 && ti->count == 1) || (ar & 2 && ti->count == 2))) {
                snprintf(buf, sizeof(buf), "%.*s takes %s", (int)n, sp,
                         ar == 3   ? "one operand or two"
                         : ar == 1 ? "one operand"
                                   : "two operands");
                why = buf;
            }
        }
        uint32_t plist = x->b;
        for (uint32_t j = 0; !why && j < lxs_node(S, plist)->b; j++) {
            limba_lx_node *px = lxs_node(S, limba_lx_list_at(S->t, plist, j));
            if (px->op == LX_KW_VAR || px->op == LX_KW_OUT)
                why = "the operands of an operator have no mode";
            else if (px->c)
                why = "the operands of an operator have no default value";
        }
        if (!why && ti && relation(defined[k].op) &&
            !lxs_compatible(S, ti->elem, S->ts.bool_))
            why = "a comparison gives a Boolean";
        if (why) {
            lxs_error(S, LXE_OPERATOR_FORM, x->a, "%s", why);
            continue;
        }
        if (!ti)
            continue;
        /* with the type of one of its operands, in the same part of the
           file (§ 6.9) */
        bool program_type = false, here = false;
        for (uint32_t j = 0; j < ti->count; j++) {
            uint32_t ts = type_scope(S, S->ts.param[ti->first + j].type);
            if (ts) {
                program_type = true;
                here = here || ts == y->scope;
            }
        }
        if (!program_type)
            lxs_error(S, LXE_OPERATOR_PLACE, x->a,
                      "%.*s needs an operand of a type of the program: the "
                      "types of the language keep their operators",
                      (int)n, sp);
        else if (!here)
            lxs_error(S, LXE_OPERATOR_PLACE, x->a,
                      "%.*s stands where the type of one of its operands is "
                      "declared, in the same part of the file",
                      (int)n, sp);
        /* two with the same operands */
        for (uint32_t j = 0; j < i; j++) {
            limba_sym o = S->ops[j];
            if (S->opk[j] != S->opk[i] || !lxs_ovl_homograph(S, o, s))
                continue;
            lxs_error(S, LXE_OVERLOAD_CLASH, x->a,
                      "%.*s is defined twice for the same operands", (int)n,
                      sp);
            lxs_note(S, S->st.sym[o].loc, S->st.sym[o].len, "the other one");
            break;
        }
    }
}

limba_sym lxs_op_choose(limba_lxs *S, uint32_t node, unsigned op,
                        const uint32_t *args, uint32_t n, limba_ltype pt,
                        bool *bad)
{
    *bad = false;
    if (!S->nops)
        return 0;
    if (op == LX_NE)
        op = LX_EQ; /* <> is not = */
    /* constants without a type only: the predefined operator (§ 4.2) */
    bool typed = false;
    for (uint32_t i = 0; i < n; i++) {
        limba_ltype t = S->type[args[i]];
        if (!t || lxs_node(S, args[i])->kind == LXN_AGG)
            continue;
        if (t != S->ts.uint && t != S->ts.ureal && t != S->ts.nil)
            typed = true;
        if (!t)
            return 0;
    }
    if (!typed)
        return 0;
    limba_sym *ok = NULL;
    uint32_t nok = 0, cap = 0;
    bool hidden = false;
    for (uint32_t i = 0; i < S->nops; i++) {
        if (S->opk[i] != op)
            continue;
        limba_sym m = S->ops[i];
        lxs_force(S, m);
        limba_ltype sig = S->st.sym[m].type;
        if (!sig || lxs_ty(S, sig)->count != n)
            continue;
        const limba_typeinfo *ti = lxs_ty(S, sig);
        bool fits = true, homograph = pt != 0;
        for (uint32_t j = 0; j < n; j++) {
            limba_param p = S->ts.param[ti->first + j];
            fits = fits && lxs_arg_fits(S, args[j], p);
            homograph = homograph && lxs_same_param(S, p.type, pt);
        }
        hidden = hidden || homograph;
        if (fits) {
            LIMBA_GROW(ok, nok, cap);
            ok[nok++] = m;
        }
    }
    limba_sym chosen = 0;
    uint32_t count = nok + (pt && !hidden ? 1 : 0);
    if (nok == 1 && count == 1) {
        chosen = ok[0];
    } else if (nok) {
        lxs_error(S, LXE_AMBIGUOUS_CALL, node,
                  "this operator is ambiguous here: %u definitions take "
                  "these operands; give a constant its type",
                  count);
        char buf[400];
        for (uint32_t i = 0; i < nok && i < 8; i++) {
            uint32_t ln;
            const char *sp = op_text(S, ok[i], &ln);
            lxs_note(S, S->st.sym[ok[i]].loc, S->st.sym[ok[i]].len, "%.*s%s",
                     (int)ln, sp, lxs_ovl_profile(S, ok[i], buf, sizeof(buf)));
        }
        *bad = true;
    }
    free(ok);
    return chosen;
}

limba_sym lxs_op_eq(limba_lxs *S, limba_ltype t)
{
    for (uint32_t i = 0; i < S->nops; i++) {
        if (S->opk[i] != LX_EQ)
            continue;
        limba_sym m = S->ops[i];
        lxs_force(S, m);
        limba_ltype sig = S->st.sym[m].type;
        if (!sig || lxs_ty(S, sig)->count != 2)
            continue;
        const limba_typeinfo *ti = lxs_ty(S, sig);
        if (lxs_same_param(S, S->ts.param[ti->first].type, t) &&
            lxs_same_param(S, S->ts.param[ti->first + 1].type, t))
            return m;
    }
    return 0;
}

void lxs_op_uses(limba_lxs *S, limba_ltype t, uint32_t node)
{
    if (!S->nops || !t)
        return;
    const limba_typeinfo *x = lxs_ty(S, t);
    if (x->kind == LIMBA_LTK_RECORD) {
        for (uint32_t i = 0; i < x->count; i++) {
            limba_ltype ft = S->ts.field[x->first + i].type;
            limba_sym m = lxs_op_eq(S, ft);
            if (m)
                lxs_use(S, m, node, false);
            else
                lxs_op_uses(S, ft, node);
        }
    } else if (x->kind == LIMBA_LTK_ARRAY || x->kind == LIMBA_LTK_OPEN) {
        limba_sym m = lxs_op_eq(S, x->elem);
        if (m)
            lxs_use(S, m, node, false);
        else
            lxs_op_uses(S, x->elem, node);
    }
}
