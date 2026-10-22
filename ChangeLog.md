# Changelog

## Unreleased

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
