# Changelog

## Unreleased

- Adopts the one change of Meri's API (Meri `4906d68`): `meri_compile_begin`
  takes the options (none: the VM), `meri_env` is written with designated
  initialisers, and the result of a run is freed with `meri_result_free`.
  When the stacks of a run cannot be reserved, `luxia` now says so
  (`luxia: cannot reserve ... for the run: ...`, status 1) instead of
  reporting an out of memory trap.
- `lx_gen` writes routines nested in routines (variables of the routine
  without a value, assigned by the first nested routine) and procedural
  types: variables and a typed constant table of them, calls through
  them (nil stopping with 102), comparisons, conversions; its oracle
  follows them.
- § 11.5 and § 10.4 with procedural values: taking the value of a
  routine counts as a call of it there (where a typed constant is
  declared, for a constant); a call through a value of a type `T` is a
  call of every routine given as a value of `T` or of a type joined to it
  by a conversion; the circle of the initialisations and the chain of
  `restrictions(no_external)` show that step. A variable of a file given
  by address to a call through a value whose routines can write it is an
  alias (L0102). `tests/run/circle`.
- Procedural types (§ 3.15): `type Compare = function(a, b: Int32):
  Boolean;`, declared with a name only (L0106), types by name with a
  conversion between those of one profile (modes, types with their
  ranges, result; L0105). The name of a routine is a value where a
  procedural type is expected (L0103 elsewhere), the routine of the name
  with the profile of the type; nested, external and predefined routines
  are not values (L0104), a `str` added for a type is. A call through a
  value (`cmp(1, 2)`, `ops[i](n)`, `Pick()(1, 2)`) gives its arguments by
  position and stops on `nil` (trap 102) before them; `=` and `<>`
  compare routines. Routine values are constants: typed constants,
  default values, data of globals (relocations to functions); a constant
  of an interface may name a routine of the implementation. A routine
  given as a value is reached. `tests/run/procedures`.
- Nested routines (§ 8.9): a routine of a file declares routines among
  its declarations, one level deep, which see its parameters, constants,
  types and the variables declared before them, and call each other in
  any order. With `overload` a nested routine joins the routines of its
  name outside and hides the homograph; without it hides the name. What
  they name of the routine around them lives in one slot, its
  environment, whose address is their first hidden parameter; their
  names in the IR are `Outer/Nested`. A call of a nested routine reads
  what it may read before it assigns it and assigns what it assigns on
  every path that returns (§ 5.5, L0053 at the call); a variable of the
  routine given by address to a nested routine that can write it is an
  alias (L0102). A routine inside a nested one, a nested routine of C,
  an operator or a `str` nested are L0017, now of the semantic phase.
  `tests/run/nested` and `nested_names`.
- Aliasing (§ 8.1): a record or an array without a mode is always
  passed by address; one object given to a routine by two ways, one of
  which can change it, is an error where the text decides it (the same
  variable, field or constant element to a `var` or `out` parameter and
  another parameter by address; a variable of a file given by address
  to a routine that can write it, through its calls), code L0102; with
  indices computed at run time the call stands, and scalar `out`
  parameters go back from the left. External routines and the routines
  of the language are not judged. `lx_gen` writes no such call.
- § 11.3: `nil` and an aggregate, too, may become ambiguous when a unit
  adds a routine with `overload`. `tests/run/names` fixes the names in
  the IR of routines of one name and of operators.
- IR version 9 of Limba: the arithmetic of `UInt8`..`UInt64` uses the
  unsigned overflow operations of the IR (`uadd.ov`, `usub.ov`,
  `umul.ov`, trap 6) in place of its own checks; the sizes of pointers,
  strings and open arrays follow the width of a pointer of the module
  (`ptr_bits`, 64 on every target known). A read of freed memory with
  its check off is a wrong module for `lir_run`.
- `luxia` runs on Meri with exceptions in the VM: under AddressSanitizer
  it lets Meri handle the faults of a run, so an access the IR left
  unchecked (`--suppress=nil_check,dangling_check`) ends as a wrong
  module, not as a report of the sanitizer.
- `tests/run/badmem`: a field read through nil with its checks off ends
  as `luxia: wrong module: memory accessed with no check of nil`,
  status 1, after what was printed, in every variant.
- Overloading (§ 8.8): `overload`, routines told apart by the types of
  their parameters, a call to its only candidate (codes L0093-L0096,
  L0099, warning L0101), bodies found by their parameters, one name from
  the routines of several units, a routine hiding only the one with its
  parameters; in the IR the name of such a routine carries its
  parameters. `overload` is now a keyword: a program that used it as a
  name renames it.
- `str` for the types of a program (§ 9.4): `function str(v: Vec):
  String; overload;` beside the type; `write`, `writeln` and `str` use it
  wherever values of the type are, a width on the string it gives. Code
  L0100.
- `lx_gen` writes routines of one name (pairs told apart by their first
  parameter), operators on the records of the program and a `str` for
  some of them, called from the program; `test_luxia` verifies the IR of
  every program it runs, so that a wrong module is an error, not a
  crash.
- Operators defined by a program (§ 6.9): `function "+"(a, b: Vec): Vec`,
  unary and binary, with the type of an operand and valid wherever its
  values are; one with the operands of the predefined operator hides it,
  constants without a type keep the predefined one; `<>` is `not` of `=`;
  the `=` a program defines composes in records and arrays. Codes L0097,
  L0098.
- The specification (§ 9.1) says the sign of a negative real in fixed
  notation: `x:0:2` writes `-0.00` for −0.004 and for a negative zero,
  as the output already did; `tests/run/zero`.
- Typed constants and variables ready at once are the initial bytes of
  their globals (version 7 of Limba's IR, data and relocations), no
  longer stores at the start of `main`; a String is a relocation to an
  immortal string. Whatever the size of the global: a run of one value
  of 256 bytes or more is one element written once with its count (a
  fill of version 8 of the IR), zeros are not written, and `main` stores
  only a BigInt other than 0 (and fills in a loop a run whose element
  holds one); an element past 1 MiB is written element by element,
  each with the fills of its own runs. `lx_gen` gives half of the global records and arrays an
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
