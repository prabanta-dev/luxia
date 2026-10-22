/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * lower_data.c - the initial value of a global known before any code
 * runs, written as its bytes (progetto_ir.md § 11h): a typed constant, a
 * variable ready at once. The bytes are those the stores of the aggregate
 * would leave, in the layout of its type; a String is a relocation to an
 * immortal string of the module.
 *
 * What makes the IR large is a value repeated, not a large global
 * (decision of 22 Oct 2026): a run of one value of at least LXL_RUN bytes
 * (else, or a range of indices) is not written out. A run of zeros is
 * nothing, as a global starts as zeros; any other run is a loop at the
 * start of main. The data stop where a hole of LXL_RUN bytes or more
 * would be written as zeros: what lies past it is stored by main, one
 * component at a time. A BigInt other than 0 is stored by main too (the
 * data hold only the BigInt 0).
 */
#include "lower.h"

#include "common/xalloc.h"

#include <stdlib.h>
#include <string.h>

/* a run of one value counts as repeated from this many bytes: a loop in
   the IR is about ten instructions, some 100 bytes */
#define LXL_RUN 256u

/* one scalar component: its bits, or a String, or a value main stores */
typedef struct {
    uint64_t off;
    uint64_t bits;
    uint32_t width;
    uint32_t str; /* in the string table of the source; UINT32_MAX none */
    uint32_t vn;  /* its constant */
    limba_ltype t;
    bool store; /* not data: main stores it */
    uint32_t seq;
} data_put;

/* count elements of type et from off, each the value vn: a loop in main */
typedef struct {
    uint64_t off, count;
    uint32_t vn;
    limba_ltype et;
} data_run;

typedef struct {
    lxl *L;
    data_put *puts;
    uint32_t nputs, capputs;
    data_run *runs;
    uint32_t nruns, capruns;
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

static void add_put(data *D, data_put p)
{
    LIMBA_GROW(D->puts, D->nputs, D->capputs);
    p.seq = D->nputs;
    D->puts[D->nputs++] = p;
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
    data_put p = {off, 0, L->m->types[it].size, UINT32_MAX, vn, t, false, 0};
    if (v->kind == LXV_STR) {
        if (it != LIMBA_T_STR) {
            D->ok = false;
            return;
        }
        p.str = v->str;
        add_put(D, p);
        return;
    }
    if (v->kind == LXV_NIL) {
        if (it != LIMBA_T_PTR)
            D->ok = false;
        return; /* zeros */
    }
    if (x->kind == LIMBA_LTK_BIGINT) {
        /* a ref of the data is the BigInt 0, the only one there (voce 8) */
        if (!limba_big_is_zero(&v->num.num)) {
            p.store = true;
            add_put(D, p);
        }
        return;
    }
    if (x->kind == LIMBA_LTK_FLOAT) {
        if (x->bits == 32) {
            float f;
            limba_rat_to_f32(&v->num, &f);
            uint32_t u;
            memcpy(&u, &f, sizeof(u));
            p.bits = u;
        } else {
            double d;
            limba_rat_to_f64(&v->num, &d);
            memcpy(&p.bits, &d, sizeof(p.bits));
        }
    } else {
        __int128 i = 0;
        if (!lxs_value_to_int(S, S->val[vn], &i) || it == LIMBA_T_STR ||
            it == LIMBA_T_PTR || it == LIMBA_T_REF) {
            D->ok = false;
            return;
        }
        /* a value outside its type: the stores and their check give the
           error at run time, as they did */
        if (limba_types_is_discrete(&S->ts, t) && (i < x->lo || i > x->hi)) {
            D->ok = false;
            return;
        }
        p.bits = (uint64_t)(unsigned __int128)i;
    }
    if (p.bits)
        add_put(D, p);
}

static void put(data *D, uint64_t off, uint32_t vn, limba_ltype t);

/* is the value vn of type t all zeros: nothing to write */
static bool all_zero(lxl *L, uint32_t vn, limba_ltype t)
{
    data Z = {.L = L, .ok = true};
    put(&Z, 0, vn, t);
    bool zero = Z.ok && !Z.nputs && !Z.nruns;
    free(Z.puts);
    free(Z.runs);
    return zero;
}

/* count elements of type et from off, all the value vn: one by one, or a
   run when they are LXL_RUN bytes or more */
static void put_span(data *D, uint64_t off, uint64_t count, uint32_t vn,
                     limba_ltype et)
{
    lxl *L = D->L;
    uint64_t esize = ti(L, et)->size;
    if (count > 1 && esize && count * esize >= LXL_RUN) {
        if (all_zero(L, vn, et))
            return;
        LIMBA_GROW(D->runs, D->nruns, D->capruns);
        D->runs[D->nruns++] = (data_run){off, count, vn, et};
        return;
    }
    for (uint64_t j = 0; j < count && D->ok; j++)
        put(D, off + j * esize, vn, et);
}

static __int128 index_value(lxl *L, uint32_t node)
{
    __int128 v = 0;
    lxs_value_to_int(L->S, L->S->val[node], &v);
    return v;
}

/* the elements of an array aggregate of fixed bounds, as lxl_agg_fill
   gives them: positional, named, else */
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
        if (a < lo || b >= lo + len || b < a) {
            D->ok = false;
            break;
        }
        LIMBA_GROW(skip, nskip, cap);
        skip[nskip++] = (struct span){a, b};
        put_span(D, off + (uint64_t)(a - lo) * esize, (uint64_t)(b - a + 1),
                 c->c, et);
    }
    if (other && D->ok) {
        uint32_t e = nd(L, other)->c;
        if (!named) {
            if (npos < len)
                put_span(D, off + (uint64_t)npos * esize,
                         (uint64_t)(len - npos), e, et);
        } else {
            /* the runs between the indices named */
            for (uint32_t i = 1; i < nskip; i++)
                for (uint32_t j = i; j > 0 && skip[j].lo < skip[j - 1].lo;
                     j--) {
                    struct span s2 = skip[j];
                    skip[j] = skip[j - 1];
                    skip[j - 1] = s2;
                }
            __int128 from = lo;
            for (uint32_t i = 0; i <= nskip && D->ok; i++) {
                __int128 to = i < nskip ? skip[i].lo - 1 : lo + len - 1;
                if (to >= from)
                    put_span(D, off + (uint64_t)(from - lo) * esize,
                             (uint64_t)(to - from + 1), e, et);
                if (i < nskip)
                    from = skip[i].hi + 1;
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
    const data_put *x = a, *y = b;
    if (x->off != y->off)
        return x->off < y->off ? -1 : 1;
    return x->seq < y->seq ? -1 : x->seq > y->seq;
}

bool lxl_data(lxl *L, const limba_id *globals, uint32_t n, uint32_t init,
              limba_ltype t)
{
    const limba_typeinfo *x = ti(L, t);
    if (!n || (x->flags & LIMBA_TF_DYNAMIC) || x->kind == LIMBA_LTK_OPEN)
        return false;
    data D = {.L = L, .ok = true};
    put(&D, 0, init, t);
    if (!D.ok) {
        free(D.puts);
        free(D.runs);
        return false;
    }
    if (D.nputs)
        qsort(D.puts, D.nputs, sizeof(*D.puts), by_offset);
    /* the data end before the first hole of LXL_RUN bytes: what comes
       after it main stores, as zeros there would be a run */
    uint64_t end = 0, len = 0;
    bool cut = false;
    for (uint32_t i = 0; i < D.nputs; i++) {
        data_put *p = &D.puts[i];
        if (p->store)
            continue;
        if (cut || p->off - end >= LXL_RUN) {
            cut = true;
            p->store = true;
            continue;
        }
        end = p->off + p->width;
        len = end;
    }
    uint8_t *bytes = limba_xcalloc(len ? len : 1, 1);
    uint32_t nstr = 0;
    limba_id *ids = limba_xcalloc(D.nputs ? D.nputs : 1, sizeof(*ids));
    uint64_t *at = limba_xcalloc(D.nputs ? D.nputs : 1, sizeof(*at));
    for (uint32_t i = 0; i < D.nputs; i++) {
        const data_put *p = &D.puts[i];
        if (p->store)
            continue;
        if (p->str != UINT32_MAX) {
            size_t sn;
            const char *s = limba_strtab_get(L->S->lx->strings, p->str, &sn);
            ids[nstr] = limba_str_intern(L->m, s, sn);
            at[nstr++] = p->off;
            continue;
        }
        for (uint32_t b = 0; b < p->width; b++)
            bytes[p->off + b] = (uint8_t)(p->bits >> (8 * b)); /* LE */
    }
    while (len && !bytes[len - 1])
        len--;
    for (uint32_t k = 0; k < n; k++) {
        if (len || nstr) {
            limba_global_set_data(L->m, globals[k], bytes, (size_t)len);
            for (uint32_t i = 0; i < nstr; i++)
                limba_global_add_reloc(L->m, globals[k], (uint32_t)at[i],
                                       LIMBA_RELOC_STR, ids[i], 0);
        }
        if (!D.nruns && D.nputs == 0)
            continue;
        /* what is not data: the runs, then the components main stores */
        limba_id g = LIMBA_NONE;
        for (uint32_t i = 0; i < D.nruns; i++) {
            if (g == LIMBA_NONE)
                g = lxl_emit(L, LIMBA_OP_GADDR, LIMBA_T_PTR, 0, globals[k], 0,
                             NULL, 0);
            const data_run *r = &D.runs[i];
            lxl_put_run(L, g, r->off, r->count, r->vn, r->et);
        }
        for (uint32_t i = 0; i < D.nputs; i++) {
            const data_put *p = &D.puts[i];
            if (!p->store)
                continue;
            if (g == LIMBA_NONE)
                g = lxl_emit(L, LIMBA_OP_GADDR, LIMBA_T_PTR, 0, globals[k], 0,
                             NULL, 0);
            lxl_put_at(L, g, p->off, p->vn, p->t);
        }
    }
    free(bytes);
    free(ids);
    free(at);
    free(D.puts);
    free(D.runs);
    return true;
}
