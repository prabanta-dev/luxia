/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * main.c - the luxia program: a Luxia source compiled by its front end
 * (limba/limba_luxia.h) and run by Meri (meri/meri.h), in one process.
 * The IR never goes through a file: Meri compiles each function to
 * bytecode as the front end gives it, before its body is freed; then the
 * program runs, ends and reports as meri runs and reports a .lir. With
 * --emit the source is compiled and written (emit.c), not run.
 */
#include "emit.h"
#include "limba/limba_luxia.h"
#include "meri/meri.h"

#include <errno.h>
#include <malloc.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(FILE *out)
{
    fputs("Usage: luxia [options] <program.luxia> [arguments of the "
          "program]\n"
          "       luxia --emit=lir|lit|tokens|ast [options] <program.luxia>\n"
          "\n"
          "Compiles a Luxia source and runs it, in one process: the front\n"
          "end makes the IR, Meri compiles it to bytecode a\n"
          "function at a time and runs its main. Errors in the source are\n"
          "reported and nothing runs (exit status 1). A run-time error\n"
          "stops the program with a message and exit status 1; halt(code)\n"
          "exits with code.\n"
          "\n"
          "With --emit the source is compiled and written, not run: its\n"
          "IR (lir, binary, next to the source unless -o; lit, text, on\n"
          "the standard output unless -o), its tokens or its syntax tree.\n"
          "\n"
          "Options (before the source)\n"
          "  -O0, -O1   optimise: no, or yes (default)\n"
          "  --check    compile only: report the errors, run nothing\n"
          "  --verify   verify the IR of each function before it is run\n"
          "             (always on in the builds that are not release)\n"
          "  --suppress=a,b\n"
          "             checks off in the whole source (index_check,\n"
          "             range_check, overflow_check, division_check,\n"
          "             conversion_check, shift_check, nil_check,\n"
          "             dangling_check, all_checks); where one would\n"
          "             fail, the behaviour is undefined\n"
          "  -I DIR     where the units of the program are looked for\n"
          "             after the directory of the program (repeat it\n"
          "             for more, in order)\n"
          "  --stdlib=DIR\n"
          "             the directory of the units of the standard\n"
          "             library\n"
          "  --restrict=no_external\n"
          "             no boundary with C: an external routine is an error\n"
          "  --lib-path=DIR\n"
          "             a directory where the C libraries of the program are\n"
          "             looked for before the system's (the option may be\n"
          "             given more than once)\n"
          "  --max-memory=N[K|M|G]\n"
          "             the memory the program may use (blocks, strings,\n"
          "             globals, slots); past it, \"out of memory\". By\n"
          "             default half of the physical memory\n"
          "  --emit=K   write, do not run: lir, lit, tokens or ast\n"
          "  -o FILE    with --emit, where to write\n"
          "  --target=P with --emit, the platform whose C the C types by\n"
          "             name follow: x86_64-linux, aarch64-linux,\n"
          "             x86_64-windows (by default this one)\n"
          "  --stats    with --emit, what each pass changed, on standard\n"
          "             error\n"
          "  --skip=a,b passes not to run (also LIMBA_OPTSKIP)\n"
          "  --version  print the version and exit\n"
          "  --help     print this text and exit\n",
          out);
}

/* a size with an optional K, M or G (powers of 1024); false if it is not
   one, 0, or past 64 bits */
static bool parse_size(const char *s, uint64_t *out)
{
    char *end;
    unsigned long long v;
    unsigned shift = 0;

    if (*s < '0' || *s > '9')
        return false;
    errno = 0;
    v = strtoull(s, &end, 10);
    if (errno || end == s)
        return false;
    if (*end == 'K' || *end == 'M' || *end == 'G')
        shift = *end == 'K' ? 10 : *end == 'M' ? 20 : 30, end++;
    if (*end || !v || v > (UINT64_MAX >> shift))
        return false;
    *out = (uint64_t)v << shift;
    return true;
}

/* the consumer of the front end: Meri compiles each function as it is
   given, while its body is still there */
typedef struct {
    meri_program *p;
    meri_diag d;
    bool failed; /* Meri could not compile: the message in d */
} feed;

static void begin(void *ctx, const limba_module *m)
{
    feed *f = ctx;
    f->p = meri_compile_begin(m, &f->d);
    f->failed = !f->p;
}

static int func(void *ctx, limba_module *m, limba_id fid)
{
    feed *f = ctx;
    (void)m;
    if (!f->failed && !meri_compile_func(f->p, fid, &f->d))
        f->failed = true;
    return f->failed; /* stop the front end: nothing will run */
}

static void end(void *ctx, const limba_module *m, int status)
{
    feed *f = ctx;
    (void)m;
    if (status == LIMBA_LUXIA_OK && !f->failed &&
        !meri_compile_end(f->p, &f->d))
        f->failed = true;
}

/* the command, with room for the directories of -I */
static int run(int argc, char **argv, const char **unit_dirs)
{
    const char *input = NULL, *suppress = NULL, *restrict_ = NULL;
    const char *stdlib = NULL, *emit = NULL, *outpath = NULL, *target = NULL;
    size_t nunit_dirs = 0;
    const char *libdirs[64];
    size_t nlibdirs = 0;
    bool check = false, verify = false;
    int level = 1, i, status;
    uint64_t max_memory = 0;
    limba_luxia_options o;
    limba_luxia_consumer c;
    limba_module *m = NULL;
    feed f = {0};
    limba_opt_options opt = {false, NULL, NULL, NULL};
    meri_result r;
    meri_env env;

#ifdef __GLIBC__
    /* a compiler: the heap grows by 64 MiB at a time and keeps what is
       freed, so its pages are touched once, by the front end and by the
       program it runs */
    mallopt(M_TOP_PAD, 64 << 20);
    mallopt(M_MMAP_THRESHOLD, 1 << 30);
    mallopt(M_TRIM_THRESHOLD, 1 << 30);
#endif

    for (i = 1; i < argc && !input; i++) {
        const char *a = argv[i];

        if (!strcmp(a, "--help") || !strcmp(a, "-h")) {
            usage(stdout);
            return 0;
        } else if (!strcmp(a, "--version")) {
            printf("luxia %s (meri %s)\n", LUXIA_VERSION, meri_version());
            return 0;
        } else if (!strcmp(a, "-O0") || !strcmp(a, "-O1")) {
            level = a[2] - '0';
        } else if (!strcmp(a, "--check")) {
            check = true;
        } else if (!strcmp(a, "--verify")) {
            verify = true;
        } else if (!strncmp(a, "--suppress=", 11)) {
            suppress = a + 11;
        } else if (!strncmp(a, "--restrict=", 11)) {
            restrict_ = a + 11;
            if (strcmp(restrict_, "no_external")) {
                fprintf(stderr, "luxia: --restrict: the restriction is "
                                "no_external\n");
                return 2;
            }
        } else if (!strcmp(a, "-I") && i + 1 < argc) {
            unit_dirs[nunit_dirs++] = argv[++i];
        } else if (!strncmp(a, "-I", 2) && a[2]) {
            unit_dirs[nunit_dirs++] = a + 2;
        } else if (!strncmp(a, "--stdlib=", 9)) {
            stdlib = a + 9;
        } else if (!strncmp(a, "--emit=", 7)) {
            emit = a + 7;
            if (strcmp(emit, "lir") && strcmp(emit, "lit") &&
                strcmp(emit, "tokens") && strcmp(emit, "ast")) {
                fprintf(stderr,
                        "luxia: --emit takes lir, lit, tokens or ast\n");
                return 2;
            }
        } else if (!strcmp(a, "-o") && i + 1 < argc) {
            outpath = argv[++i];
        } else if (!strncmp(a, "--target=", 9)) {
            target = a + 9;
        } else if (!strcmp(a, "--stats")) {
            opt.stats = stderr;
        } else if (!strncmp(a, "--skip=", 7)) {
            opt.skip = a + 7;
        } else if (!strncmp(a, "--lib-path=", 11)) {
            if (nlibdirs < sizeof(libdirs) / sizeof(libdirs[0]))
                libdirs[nlibdirs++] = a + 11;
        } else if (!strncmp(a, "--max-memory=", 13)) {
            if (!parse_size(a + 13, &max_memory)) {
                fprintf(stderr, "luxia: --max-memory: not a size: %s\n",
                        a + 13);
                return 2;
            }
        } else if (a[0] == '-' && a[1]) {
            fprintf(stderr, "luxia: unknown option: %s\n", a);
            usage(stderr);
            return 2;
        } else {
            input = a;
        }
    }
    if (!input) {
        usage(stderr);
        return 2;
    }

    if (emit || outpath || target || opt.stats) {
        if (!emit) {
            fputs("luxia: -o, --target and --stats go with --emit\n", stderr);
            return 2;
        }
        if (i < argc) {
            fputs("luxia: --emit compiles one source and runs nothing: no "
                  "arguments after it\n",
                  stderr);
            return 2;
        }
        unit_dirs[nunit_dirs] = NULL;
        opt.verify_each = verify;
        return limba_luxia_emit(input, emit, outpath, check, level, &opt,
                                suppress, target, restrict_, unit_dirs, stdlib);
    }
#ifndef NDEBUG
    verify = true;
#endif
    o = (limba_luxia_options){.level = level,
                              .verify = verify,
                              .suppress = suppress,
                              .no_external = restrict_ != NULL,
                              .diag_out = stderr,
                              .unit_path = unit_dirs,
                              .stdlib_path = stdlib};
    /* with --check the front end alone: no consumer, no module kept */
    c = (limba_luxia_consumer){&f, begin, func, end, NULL, false};
    status = limba_luxia_compile_file(input, &o, check ? NULL : &c,
                                      check ? NULL : &m);
    if (check || status == LIMBA_LUXIA_ERRORS ||
        status == LIMBA_LUXIA_OPTIONS) {
        meri_program_free(f.p);
        limba_module_free(m);
        return status;
    }
    if (f.failed || status != LIMBA_LUXIA_OK) {
        if (f.failed)
            fprintf(stderr, "luxia: %s: %s\n", input, f.d.msg);
        else
            fprintf(stderr,
                    "luxia: %s: internal error of the front end "
                    "(%d)\n",
                    input, status);
        meri_program_free(f.p);
        limba_module_free(m);
        return 1;
    }
    /* every library and symbol before the first instruction */
    if (!meri_link(f.p, libdirs, nlibdirs, &f.d)) {
        fprintf(stderr, "luxia: %s: %s\n", input, f.d.msg);
        meri_program_free(f.p);
        limba_module_free(m);
        return 1;
    }

    /* a closed output is EPIPE at the write, not a signal (§ 11g) */
    signal(SIGPIPE, SIG_IGN);
    env = (meri_env){argc - i, argv + i, stdin, stdout, max_memory};
    meri_run(f.p, "main", &env, &r);
    meri_flush(stdout, &r);
    status = meri_report(m, &r, stderr);
    meri_program_free(f.p);
    limba_module_free(m);
    return status;
}

int main(int argc, char **argv)
{
    /* the directories of -I, ended by NULL: at most one an argument */
    const char **unit_dirs = calloc((size_t)argc + 1, sizeof(*unit_dirs));
    int status;

    if (!unit_dirs) {
        fputs("luxia: out of memory\n", stderr);
        return 1;
    }
    status = run(argc, argv, unit_dirs);
    free(unit_dirs);
    return status;
}
