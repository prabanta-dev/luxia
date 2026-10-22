/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * test_types.c - the types and the scopes of the front end of Luxia
 * against expected results.
 */
#include "common/strtab.h"
#include "front/symtab.h"
#include "front/types.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(cond, ...)                                                       \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "test_types: line %d: ", __LINE__);                \
            fprintf(stderr, __VA_ARGS__);                                      \
            fputc('\n', stderr);                                               \
            failures++;                                                        \
        }                                                                      \
    } while (0)

static const char *strtab_name(const void *ctx, uint32_t id, size_t *len)
{
    return limba_strtab_get(ctx, id, len);
}

static void test_types(void)
{
    limba_types ts;
    limba_types_init(&ts);
    limba_strtab *names = limba_strtab_new();
    limba_ltype i8 = limba_types_int(&ts, 8, LIMBA_TF_SIGNED);
    limba_ltype i16 = limba_types_int(&ts, 16, LIMBA_TF_SIGNED);
    limba_ltype i64 = limba_types_int(&ts, 64, LIMBA_TF_SIGNED);
    limba_ltype u64 = limba_types_int(&ts, 64, 0);
    limba_ltype f64 = limba_types_float(&ts, 64);
    limba_types_set_name(&ts, i16, limba_strtab_intern(names, "Int16", 5));
    limba_types_set_name(&ts, f64, limba_strtab_intern(names, "Float64", 7));
    CHECK(ts.t[i8].lo == -128 && ts.t[i8].hi == 127, "Int8 bounds");
    CHECK(ts.t[u64].lo == 0 && ts.t[u64].hi == (((__int128)1 << 64) - 1),
          "UInt64 bounds");

    /* identity: a range keeps its root, new makes one */
    limba_ltype pct = limba_types_range(&ts, i16, 0, 100);
    limba_ltype metri = limba_types_distinct(&ts, f64);
    limba_ltype id = limba_types_distinct(&ts, pct);
    CHECK(ts.t[pct].root == ts.t[i16].root, "a range is compatible");
    CHECK(ts.t[metri].root != ts.t[f64].root, "new makes a root");
    CHECK((ts.t[id].flags & LIMBA_TF_RANGE) && ts.t[id].lo == 0 &&
              ts.t[id].hi == 100 && ts.t[id].root != ts.t[i16].root &&
              ts.t[ts.t[id].base].root == ts.t[id].root,
          "new Int16 range 0..100 is a range of a new Int16");

    /* records laid out as C lays them out */
    limba_ltype r = limba_types_record_begin(&ts);
    limba_types_record_field(&ts, r, 0, i8);
    limba_types_record_field(&ts, r, 1, i64);
    limba_types_record_field(&ts, r, 2, i16);
    limba_types_record_end(&ts, r);
    const limba_field *fl = &ts.field[ts.t[r].first];
    CHECK(fl[0].offset == 0 && fl[1].offset == 8 && fl[2].offset == 16 &&
              ts.t[r].size == 24 && ts.t[r].align == 8,
          "record layout");

    bool ok;
    limba_ltype idx = limba_types_range(&ts, i16, 1, 10);
    limba_ltype arr = limba_types_array(&ts, idx, f64, false, &ok);
    CHECK(ok && ts.t[arr].size == 80, "array[1..10] of Float64: 80 bytes");
    limba_types_array(&ts, i64, i64, false, &ok);
    CHECK(!ok, "an array indexed by all of Int64 overflows");
    limba_types_array(&ts, i16, arr, true, &ok);
    CHECK(ok, "a dynamic array has no static size");

    limba_ltype p1 = limba_types_pointer(&ts, r),
                p2 = limba_types_pointer(&ts, r);
    limba_ltype p3 = limba_types_pointer(&ts, i8);
    CHECK(limba_types_same(&ts, p1, p2) && !limba_types_same(&ts, p1, p3),
          "pointers are equal by structure");
    CHECK(!limba_types_same(&ts, i16, pct), "a range is not the same type");

    char buf[128];
    limba_types_show(&ts, arr, strtab_name, names, buf, sizeof(buf));
    CHECK(!strcmp(buf, "array[Int16 range 1..10] of Float64"), "%s", buf);
    limba_types_show(&ts, limba_types_open(&ts, i16, p1), strtab_name, names,
                     buf, sizeof(buf));
    CHECK(!strcmp(buf, "array[Int16 range <>] of ^a record"), "%s", buf);
    limba_types_show(&ts, ts.uint, strtab_name, names, buf, sizeof(buf));
    CHECK(!strcmp(buf, "an integer constant"), "%s", buf);

    limba_strtab_free(names);
    limba_types_free(&ts);
}

static void test_symtab(void)
{
    limba_symtab st;
    limba_symtab_init(&st);
    uint32_t top = limba_scope_new(&st, 0, 0);
    uint32_t inner = limba_scope_new(&st, top, 0);
    limba_sym dup,
        a = limba_sym_declare(&st, top, 7, LIMBA_LSYM_VAR, 1, 1, &dup);
    CHECK(a && !dup, "a first declaration");
    CHECK(!limba_sym_declare(&st, top, 7, LIMBA_LSYM_CONST, 2, 1, &dup) &&
              dup == a,
          "a second one in the same scope is a duplicate");
    limba_sym b = limba_sym_declare(&st, inner, 7, LIMBA_LSYM_VAR, 3, 1, &dup);
    CHECK(b && b != a, "an inner scope may shadow");
    CHECK(limba_sym_lookup(&st, inner, 7) == b, "the inner one is found");
    CHECK(limba_sym_lookup(&st, top, 7) == a, "the outer one from outside");
    CHECK(limba_sym_lookup(&st, inner, 8) == 0, "no name, no symbol");
    CHECK(limba_sym_local(&st, inner, 7) == b &&
              limba_sym_local(&st, top, 8) == 0,
          "local lookups");
    for (uint32_t i = 0; i < 10000; i++)
        limba_sym_declare(&st, top, 100 + i, LIMBA_LSYM_VAR, 1, 1, NULL);
    CHECK(limba_sym_lookup(&st, inner, 100 + 9999) != 0 &&
              limba_sym_get(&st, limba_sym_lookup(&st, inner, 5000))->name ==
                  5000,
          "many names");
    limba_symtab_free(&st);
}

/* the shortest form of a real: known cases as Python's repr writes them,
   then random bits that must read back to themselves */
/* a Float32 in its own shortest form: fixed cases, then random floats
   that must read back with strtof */

int main(void)
{
    test_types();
    test_symtab();
    printf("test_types: types, scopes, %d failures\n", failures);
    return failures ? 1 : 0;
}
