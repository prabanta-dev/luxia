# Luxia

[![Status: experimental](https://img.shields.io/badge/status-experimental-orange.svg)]()
[![Platform: Linux x86-64](https://img.shields.io/badge/platform-Linux%20x86--64-lightgrey.svg)]()
[![Licence: GPL-3.0-or-later](https://img.shields.io/badge/licence-GPL--3.0--or--later-blue.svg)](LICENSE)
[![LinkedIn](https://img.shields.io/badge/LinkedIn-Maurizio%20Cammalleri-0077B5?logo=linkedin)](https://www.linkedin.com/in/maurizio-cammalleri-80a89a11/)
[![Substack](https://img.shields.io/badge/Substack-Maurizio%20Cammalleri-FF6719?logo=substack)](https://cammalleri.substack.com/)

Luxia is an extended Pascal with the rigour of Ada and without its
verbosity. This repository holds the language and everything of its own:
the specification, the front end, written in C with no dependencies beyond
the C library, its tests and the program `luxia`, which compiles a source
and runs it in one process.

The front end makes the intermediate representation (IR) of
[Limba](https://github.com/prabanta-dev/limba), the part of the Prabanta
family that any language shares (the IR, its optimiser, a reference
interpreter); [Meri](https://github.com/prabanta-dev/meri), the back end,
runs it. Their libraries are linked statically, and each function goes
from the front end to Meri in memory, as soon as it is complete.

---

## ⚠️ Read this first

**This is a project in development, not a usable compiler.**

| | |
|---|---|
| **Stage** | Early development. The language, its file formats and the command line **change without notice**. |
| **The language** | Luxia 0 is specified in [`doc/luxia-0.md`](doc/luxia-0.md): a draft, whose rules may still change. |
| **Platform** | Developed and tested on Linux x86-64 only (Debian 13, GCC and Clang). |

Nothing here is ready for use in real work. It is public so that the design
and its progress can be followed.

## What Luxia looks like

```pascal
program Hello;

type
  Tree = ^Node;
  Node = record
    left, right: Tree;
  end;

function Check(t: Tree): Int64;
begin
  if t.left = nil then
    return 1;
  end;
  return 1 + Check(t.left) + Check(t.right);
end Check;

begin
  var n: Int32 := 10;
  for var i := 1 to n do
    writeln(i, TAB, i * i);
  end;
end Hello.
```

Some of its rules:

- Only sized types: `Int8`..`Int64`, `UInt8`..`UInt64`, `Bits8`..`Bits64`,
  `Float32`, `Float64`. There is no `Integer` whose size depends on the
  platform, no implicit conversion and no promotion: both operands of an
  operator have the same type.
- Arithmetic on numbers is checked (overflow, index, range, `nil`,
  division by zero, conversion). Wrapping arithmetic and bit operators
  exist only on the `BitsN` types.
- Names are unique without regard to case, but every use must be spelled
  as the declaration.
- Every structured statement ends with `end`; `;` ends every statement, as
  in Ada.
- Constant expressions are computed exactly (integers and rationals of any
  size) and rounded once, when they take a type.

## What works now

- **Luxia 0** as specified: sized integers, reals, Booleans, characters,
  counted Strings, ranges, enumerations, records, arrays (fixed, with
  computed bounds, open, made by `new`), pointers with `nil` and dangling
  checks, routines with `in`, `var` and `out` parameters, functions
  returning records and arrays, integers of arbitrary precision
  (`BigInt`), calls to C. Its library writes and reads the standard
  streams by line and in blocks of bytes (`readbytes`, `writebytes`), and
  works on arrays of bytes as Python does in C (`move`, `translate`,
  `reverse`, `occurrences`); an error of input or output is never
  ignored. Every error has a stable code
  ([`doc/diagnostics.md`](doc/diagnostics.md)).
- **Nine benchmarks from the Computer Language Benchmarks Game**, translated
  to Luxia, print the expected output: binary-trees, fannkuch-redux, fasta,
  k-nucleotide, mandelbrot, n-body, pidigits, reverse-complement and
  spectral-norm ([`tests/benchmarks/`](tests/benchmarks/)).
- **Speed of the front end**: about 1 µs per line of source without
  optimisation, 1.7 µs with it (a file of 108 000 lines, one core).

## Using it

    luxia program.luxia [arguments]    # compiles it and runs it
    luxia -O0 program.luxia            # without optimising (-O1 by default)
    luxia --check program.luxia        # reports its errors, runs nothing
    luxia --lib-path=DIR program.luxia # where its C libraries are looked for
    luxia -I DIR program.luxia         # where its units are looked for
    luxia --stdlib=DIR program.luxia   # the units of the standard library
    luxia --emit=lir program.luxia     # writes program.lir, runs nothing
    luxia --emit=lit -o program.lit program.luxia   # the IR as text
    luxia --help                       # every option

The options come before the source; what follows it goes to the program.
Errors in the source are reported, each with a stable code
([`doc/diagnostics.md`](doc/diagnostics.md)), and nothing runs (exit
status 1). Once it runs, a program behaves as under `meri`: a run-time
error stops it with a message on the standard error and exit status 1,
`luxia: index out of range at program.luxia:12:5`; `halt(code)` exits
with `code`; past the memory `--max-memory` gives it, "out of memory",
never a crash; a program whose output is closed (`program | head`) ends
with status 141 and no message.

## How the front end is built

```
 program.luxia
      │
      ▼
   lexer ──► parser ──► semantic analysis ──► IR generation (SSA)
                                                     │  one function
                                                     ▼  at a time
                          Limba: verifier ──► optimiser ──► Meri (luxia)
                                                        └─► program.lir (--emit)
```

- the **lexer** reads the tokens from a table (`tokens.def`); positions are
  32-bit offsets into the loaded sources, turned into lines and columns
  only for a message;
- the **parser** is recursive descent; expressions go through a Pratt
  engine driven by a table of operators. The grammar is LL(1). The tree is a flat array of nodes that refer to
  each other by index;
- the **semantic analysis** resolves names in scopes, declarations on
  demand (types and routines may be used before their declaration,
  without `forward`), and types.
  Constant expressions are computed exactly, with integers and rationals
  of any size, and rounded once, when they take a type;
- the **IR generation** builds SSA directly from the tree (Braun et al.,
  *Simple and Efficient Construction of Static Single Assignment Form*),
  and on the way proves that every variable is assigned before it is
  read and that every function returns a value.

The front end is also a library (`include/limba/limba_luxia.h`): a
program that runs the IR in memory compiles a source and receives each
function as soon as it is complete, verified and optimised, as `luxia`
does ([`doc/api.md`](doc/api.md)).

**How it is tested** (`tests/`, `tools/`): a generator of random,
well-typed programs that knows what each must print (`lx_gen`), run
against the reference interpreter of Limba without optimisation, with all
the passes and with each pass alone; the programs of `tests/run` and the
benchmarks under `luxia`, against `luxia --emit=lir` then `meri` and
against their expected output; **sabotage**: every net is tried with a bug
put in on purpose, and must catch it; the release, debug,
AddressSanitizer and UndefinedBehaviorSanitizer builds all run the
suites; a fuzzer reads broken Luxia.

**Where things are:**

| | |
|---|---|
| `include/limba/limba_luxia.h` | the front end as a library |
| `src/front/` | the front end: sources and positions, diagnostics, lexer, parser and its Pratt engine, types, scopes, semantic analysis, IR generation, units, random programs |
| `src/luxia/` | the `luxia` command |
| `tools/` | `lx_gen` |
| `tests/` | the suites and their data: `benchmarks/` (those that call C libraries in `benchmarks/c/`), `ffi/` (calls to C), `units/`, `run/` (programs run by `luxia`) |
| `doc/` | the specification of Luxia, its diagnostics, the API of the front end |

## Building

Luxia needs Limba and Meri next to it (`../Limba` and `../Meri`, or
`LIMBA_DIR` and `MERI_DIR`); its build script builds Meri first, in the
same variant, and Meri builds Limba.

    ./build.sh               # release build: bin/<cpu>-<os>/luxia
    ./build.sh release test  # and the tests
    ./build.sh --help        # variants (debug, asan, ubsan, tsan), actions

A C11 compiler (GCC or Clang) and a POSIX shell are all it needs.

## Licence

GPL-3.0-or-later: see [LICENSE](LICENSE). Authors and the code of others
in Limba and Meri: [AUTHORS](AUTHORS).
