/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * lower_data.c - the initial value of a global known before any code
 * runs, written as its bytes (progetto_ir.md § 11h): a typed constant, a
 * variable ready at once. The bytes are those the stores of the aggregate
 * would leave, in the layout of its type; a String is a relocation to an
 * immortal string of the module. What cannot be data (a BigInt other than
 * 0, a component that is not a constant, a global too large) is left to
 * the stores at the start of main, as before.
 */
#include "lower.h"

#include "common/xalloc.h"

#include <stdlib.h>
#include <string.h>

/* past this size a global keeps the stores (and their loops for else):
   its bytes would make the module as large */
#define LXL_DATA_MAX (1u << 20)

typedef struct {
    uint64_t off;
    uint32_t str; /* in the string table of the source */
    uint32_t seq; /* the order written: the last at an offset stays */
} data_str;

typedef struct {
    lxl *L;
    uint8_t *b;
    uint64_t size;
    data_str *strs;
    uint32_t nstrs, capstrs;
    bool ok;
} data;

static const limba_typeinfo *ti(const lxl *L, limba_ltype t)
{
    return &L->S->ts.t[t];
}

static const limba_lx_node *nd(const lxl *L, uint32_t node)
{
    return &L->S->t->node[node];
}

static uint32_t list_at(const lxl *L, uint32_t list, uint32_t i)
{
    return limba_lx_list_at(L->S->t, list, i);
}

static void put_bytes(data *D, uint64_t off, uint64_t bits, uint32_t width)
{
    if (off + width > D->size) {
        D->ok = false;
        return;
    }
    for (uint32_t i = 0; i < width; i++)
        D->b[off + i] = (uint8_t)(bits >> (8 * i)); /* little endian */
}

/* the constant vn, of a scalar type t, at off */
static void put_scalar(data *D, uint64_t off, uint32_t vn, limba_ltype t)
{
    lxl *L = D->L;
    limba_lxs *S = L->S;
    limba_ltype vt = S->type[vn];
    limba_id it = lxl_type(L, t);
    if (!S->val[vn] || !vt || lxl_type(L, vt) != it) {
        D->ok = false;
        return;
    }
    const limba_lxs_value *v = &S->v[S->val[vn]];
    const limba_typeinfo *x = ti(L, t);
    uint32_t width = L->m->types[it].size;
    if (v->kind == LXV_STR) {
        if (it != LIMBA_T_STR) {
            D->ok = false;
            return;
        }
        put_bytes(D, off, 0, width);
        LIMBA_GROW(D->strs, D->nstrs, D->capstrs);
        D->strs[D->nstrs] = (data_str){off, v->str, D->nstrs};
        D->nstrs++;
        return;
    }
    if (v->kind == LXV_NIL) {
        if (it != LIMBA_T_PTR)
            D->ok = false;
        put_bytes(D, off, 0, width);
        return;
    }
    if (x->kind == LIMBA_LTK_BIGINT) {
        /* a ref of the data is the BigInt 0, the only one there (voce 8) */
        if (!limba_big_is_zero(&v->num.num))
            D->ok = false;
        put_bytes(D, off, 0, width);
        return;
    }
    if (x->kind == LIMBA_LTK_FLOAT) {
        uint64_t bits;
        if (x->bits == 32) {
            float f;
            limba_rat_to_f32(&v->num, &f);
            uint32_t u;
            memcpy(&u, &f, sizeof(u));
            bits = u;
        } else {
            double d;
            limba_rat_to_f64(&v->num, &d);
            memcpy(&bits, &d, sizeof(bits));
        }
        put_bytes(D, off, bits, width);
        return;
    }
    __int128 i = 0;
    if (!lxs_value_to_int(S, S->val[vn], &i) || it == LIMBA_T_STR ||
        it == LIMBA_T_PTR || it == LIMBA_T_REF) {
        D->ok = false;
        return;
    }
    /* a value outside its type: the stores and their check give the error
       at run time, as they did */
    if (limba_types_is_discrete(&S->ts, t) && (i < x->lo || i > x->hi)) {
        D->ok = false;
        return;
    }
    put_bytes(D, off, (uint64_t)(unsigned __int128)i, width);
}

static void put(data *D, uint64_t off, uint32_t vn, limba_ltype t);

static __int128 index_value(lxl *L, uint32_t node)
{
    __int128 v = 0;
    lxs_value_to_int(L->S, L->S->val[node], &v);
    return v;
}

/* the elements of an array aggregate of fixed bounds, in the order the
   stores of lxl_agg_fill write them: positional, named, else */
static void put_elems(data *D, uint64_t off, uint32_t node, limba_ltype t)
{
    lxl *L = D->L;
    const limba_typeinfo *x = ti(L, t);
    limba_ltype et = x->elem;
    const limba_typeinfo *ix = ti(L, x->index);
    uint64_t esize = ti(L, et)->size;
    __int128 lo = ix->lo, len = ix->hi - ix->lo + 1;
    uint32_t list = nd(L, node)->a, n = nd(L, list)->b, npos = 0, other = 0;
    bool named = false;
    for (uint32_t i = 0; i < n; i++) {
        const limba_lx_node *c = nd(L, list_at(L, list, i));
        if (c->op == LX_KW_ELSE)
            other = list_at(L, list, i);
        else if (c->a)
            named = true;
        else
            npos++;
    }
    struct span {
        __int128 lo, hi;
    } *skip = NULL;
    uint32_t nskip = 0, cap = 0, k = 0;
    for (uint32_t i = 0; i < n && D->ok; i++) {
        const limba_lx_node *c = nd(L, list_at(L, list, i));
        if (c->op == LX_KW_ELSE)
            continue;
        if (!c->a) {
            put(D, off + k * esize, c->c, et);
            k++;
            continue;
        }
        __int128 a = index_value(L, c->a), b = c->b ? index_value(L, c->b) : a;
        if (a < lo || b >= lo + len) {
            D->ok = false;
            break;
        }
        LIMBA_GROW(skip, nskip, cap);
        skip[nskip++] = (struct span){a, b};
        for (__int128 j = a; j <= b && D->ok; j++)
            put(D, off + (uint64_t)(j - lo) * esize, c->c, et);
    }
    if (other && D->ok) {
        uint32_t e = nd(L, other)->c;
        if (!named) {
            for (__int128 j = npos; j < len && D->ok; j++)
                put(D, off + (uint64_t)j * esize, e, et);
        } else {
            for (__int128 j = lo; j < lo + len && D->ok; j++) {
                bool given = false;
                for (uint32_t s = 0; s < nskip && !given; s++)
                    given = j >= skip[s].lo && j <= skip[s].hi;
                if (!given)
                    put(D, off + (uint64_t)(j - lo) * esize, e, et);
            }
        }
    }
    free(skip);
}

/* the value vn of type t at off: a constant, or an aggregate of them */
static void put(data *D, uint64_t off, uint32_t vn, limba_ltype t)
{
    lxl *L = D->L;
    const limba_typeinfo *x = ti(L, t);
    if (nd(L, vn)->kind != LXN_AGG) {
        if (lxl_scalar(L, t))
            put_scalar(D, off, vn, t);
        else
            D->ok = false; /* a record or an array not written out */
        return;
    }
    if (x->kind == LIMBA_LTK_RECORD) {
        uint32_t list = nd(L, vn)->a;
        for (uint32_t i = 0; i < nd(L, list)->b && D->ok; i++) {
            const limba_lx_node *c = nd(L, list_at(L, list, i));
            const limba_field *f = &L->S->ts.field[x->first + c->d - 1];
            put(D, off + f->offset, c->c, f->type);
        }
        return;
    }
    if (x->kind == LIMBA_LTK_ARRAY && !(x->flags & LIMBA_TF_DYNAMIC))
        put_elems(D, off, vn, t);
    else
        D->ok = false;
}

static int by_offset(const void *a, const void *b)
{
    const data_str *x = a, *y = b;
    if (x->off != y->off)
        return x->off < y->off ? -1 : 1;
    return x->seq < y->seq ? -1 : x->seq > y->seq;
}

bool lxl_data(lxl *L, const limba_id *globals, uint32_t n, uint32_t init,
              limba_ltype t)
{
    const limba_typeinfo *x = ti(L, t);
    if (!n || x->size > LXL_DATA_MAX || (x->flags & LIMBA_TF_DYNAMIC) ||
        x->kind == LIMBA_LTK_OPEN)
        return false;
    data D = {.L = L, .size = x->size, .ok = true};
    D.b = limba_xcalloc(D.size ? D.size : 1, 1);
    put(&D, 0, init, t);
    if (D.ok) {
        uint64_t len = D.size;
        while (len && !D.b[len - 1])
            len--;
        if (D.nstrs)
            qsort(D.strs, D.nstrs, sizeof(*D.strs), by_offset);
        uint32_t m = 0; /* the last string written at each offset */
        for (uint32_t i = 0; i < D.nstrs; i++)
            if (i + 1 == D.nstrs || D.strs[i + 1].off != D.strs[i].off)
                D.strs[m++] = D.strs[i];
        D.nstrs = m;
        limba_id *ids = limba_xcalloc(m ? m : 1, sizeof(*ids));
        for (uint32_t i = 0; i < m; i++) {
            size_t sn;
            const char *s =
                limba_strtab_get(L->S->lx->strings, D.strs[i].str, &sn);
            ids[i] = limba_str_intern(L->m, s, sn);
        }
        for (uint32_t k = 0; k < n; k++) {
            if (!len && !D.nstrs)
                break; /* all zero: as a global without a value */
            limba_global_set_data(L->m, globals[k], D.b, (size_t)len);
            for (uint32_t i = 0; i < m; i++)
                limba_global_add_reloc(L->m, globals[k],
                                       (uint32_t)D.strs[i].off, LIMBA_RELOC_STR,
                                       ids[i], 0);
        }
        free(ids);
    }
    free(D.b);
    free(D.strs);
    return D.ok;
}
