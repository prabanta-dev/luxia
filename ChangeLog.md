# Changelog

## Unreleased

- Overloading (§ 8.8): `overload`, routines told apart by the types of
  their parameters, a call to its only candidate (codes L0093-L0096,
  L0099, warning L0101), bodies found by their parameters, one name from
  the routines of several units, a routine hiding only the one with its
  parameters; in the IR the name of such a routine carries its
  parameters. `overload` is now a keyword: a program that used it as a
  name renames it. Operators defined by a program (§ 6.9) and `str` for
  its types (§ 9.4) are specified, not yet compiled.
- The specification (§ 9.1) says the sign of a negative real in fixed
  notation: `x:0:2` writes `-0.00` for −0.004 and for a negative zero,
  as the output already did; `tests/run/zero`.
- Typed constants and variables ready at once are the initial bytes of
  their globals (version 7 of Limba's IR, data and relocations), no
  longer stores at the start of `main`; a String is a relocation to an
  immortal string. Whatever the size of the global: a run of one value
  of 256 bytes or more is a loop at the start of `main` (nothing for
  zeros), and `main` stores a BigInt other than 0 and what lies past a
  long run. `lx_gen` gives half of the global records and arrays an
  aggregate of literals and prints them first; its oracle computes the
  mathematical functions with Limba's library, the bits of every engine.
- Arrays of more indices, `array[I, J] of T`, as in Ada (specification
  § 3.7.2): `a[i, j]`, laid out by rows as in C; `low(a, k)`,
  `high(a, k)`, `length(a, k)`; aggregates nested by index; open
  parameters of more indices, `array[I range <>, J range <>] of T`.
  Arrays of arrays stay a form of their own. Code L0092. `lx_gen` writes
  arrays of two indices.
- Arrays of more indices with computed bounds, made by
  `new(M range a..b, c..d)`, compared whole when their bounds are open or
  computed (the same lengths index by index, then the cells), and given
  aggregates when open. Not yet: an aggregate as the initial value of one
  with computed bounds (L0054). `lx_gen` writes them too.
- Fixed: comparing arrays of arrays as a whole made `luxia` refuse the
  program ("function 2 is not declared"): every comparison function is
  now declared before the first body is given.
- Arguments by name, `f(x => 1)`, and default values of parameters,
  `from: Int64 := 1` (specification § 8.6, § 8.7): those by position
  first, evaluated as written; a default value is a constant, only
  without a mode, on the last parameters, the same in an interface and
  its body. Codes L0086-L0091; `lx_gen` writes both.
- Every message of `luxia` begins with `luxia:`, also the diagnoses of
  no place and the line of the error limit, which said `limba:`; every
  program of Luxia gives its name to Limba (`limba_set_program`).
- `luxia --help` keeps the options of who programs in Luxia apart from
  those of who develops the compiler (`--emit`, `-o`, `--target`,
  `--verify`, `--stats`, `--skip`).
- The specification (§ 9.6): the mathematical functions give the same
  bits on every engine and platform, a `Float32` through `Float64`; they
  may change between versions only to become better, declared.
- Luxia gets a repository of its own, from Limba and Prabanta: the
  specification (`doc/luxia-0.md`), the diagnostics and the API of the
  front end (`doc/`, `include/limba/limba_luxia.h`); the front end
  (`src/front/`, into `libluxia.a`): sources, diagnostics, lexer, parser,
  types, scopes, semantic analysis, units, IR generation, random
  programs; `lx_gen`; the suites (`test_luxia`, `test_luxia_api`,
  `test_types`, `fuzz_luxia`, and `test_meri_api` from Meri) and their
  data (`tests/benchmarks`, with those of Meri that call C in `tests/benchmarks/c`,
  `tests/ffi`, `tests/units`, `tests/run`).
- `luxia` is the program of the language: what `prabanta` was (a source
  compiled and run in one process, by Meri, `-O1` by default), and with
  `--emit=lir|lit|tokens|ast` what `limba` did on a Luxia source (the IR
  written, or the tokens, or the tree, and nothing run), `-o`,
  `--target`, `--stats`, `--skip`. The options come before the source.
- `build.sh` builds Meri, which builds Limba, in the same variant, then
  `libluxia.a` and the programs; `test` runs the suites, the programs of
  `tests/run`, the benchmarks against `luxia --emit=lir` then `meri`,
  and the probes of the calls of C through `meri`.
