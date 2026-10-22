/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * emit.h - luxia --emit: a Luxia source to its IR, tokens or tree, written
 * and not run.
 */
#ifndef LUXIA_TOOL_EMIT_H
#define LUXIA_TOOL_EMIT_H

#include "limba/opt.h"

#include <stdbool.h>

/* compile a Luxia source: tokens, tree, or the IR (lir by default, lit),
   optimised at level, with the checks named in suppress (index_check,...,
   separated by commas) off in the whole file, the C types by name of
   platform target (NULL: the one that compiles), and with restrict
   "no_external" no boundary with C; emit, suppress, target and restrict
   may be NULL; the exit status of the program */
int limba_luxia_emit(const char *in, const char *emit, const char *outpath,
                     bool check, int level, const limba_opt_options *opt,
                     const char *suppress, const char *target,
                     const char *restrict_, const char *const *unit_path,
                     const char *stdlib);

#endif
