/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * lower_exc.c - exceptions in the IR (§ 3.16, § 7.9, § 10.1 of the
 * specification): the table of the exceptions of the module, which gives
 * every run-time error the name of its exception, so that one no handler
 * takes stops the program with it (luxia: IndexError: index out of range
 * at ...), the predefined families too.
 */
#include "lower.h"

#include <string.h>

/* the predefined exceptions: identity (traps.def), name, family */
static const struct {
    int32_t id;
    const char *name;
    int32_t parent;
} predefined[] = {
    {LIMBA_FAMILY_CONSTRAINT, "ConstraintError", -1},
    {LIMBA_TRAP_OVERFLOW, "OverflowError", LIMBA_FAMILY_CONSTRAINT},
    {LIMBA_TRAP_DIVZERO, "DivisionError", LIMBA_FAMILY_CONSTRAINT},
    {LIMBA_TRAP_INDEX, "IndexError", LIMBA_FAMILY_CONSTRAINT},
    {LIMBA_TRAP_RANGE, "RangeError", LIMBA_FAMILY_CONSTRAINT},
    {LIMBA_TRAP_NIL, "NilError", LIMBA_FAMILY_CONSTRAINT},
    {LIMBA_TRAP_CONVERSION, "ConversionError", LIMBA_FAMILY_CONSTRAINT},
    {LIMBA_TRAP_SHIFT, "ShiftError", LIMBA_FAMILY_CONSTRAINT},
    {LIMBA_TRAP_DANGLING, "DanglingError", LIMBA_FAMILY_CONSTRAINT},
    {LIMBA_TRAP_LENGTH, "LengthError", LIMBA_FAMILY_CONSTRAINT},
    {LIMBA_FAMILY_STORAGE, "StorageError", -1},
    {LIMBA_TRAP_NOMEM, "StorageError", LIMBA_FAMILY_STORAGE},
    {LIMBA_TRAP_STACK, "StorageError", LIMBA_FAMILY_STORAGE},
    {LIMBA_FAMILY_PROGRAM, "ProgramError", -1},
    {LIMBA_TRAP_INVALID_FREE, "ProgramError", LIMBA_FAMILY_PROGRAM},
    {LIMBA_TRAP_INVALID_HANDLE, "ProgramError", LIMBA_FAMILY_PROGRAM},
    {LIMBA_FAMILY_INPUT_OUTPUT, "InputOutputError", -1},
    {LIMBA_TRAP_IO, "InputOutputError", LIMBA_FAMILY_INPUT_OUTPUT},
};

void lxl_exc_table(lxl *L)
{
    for (size_t i = 0; i < sizeof(predefined) / sizeof(predefined[0]); i++)
        limba_exception_add(L->m, predefined[i].id,
                            limba_str_intern(L->m, predefined[i].name,
                                             strlen(predefined[i].name)),
                            predefined[i].parent);
}
