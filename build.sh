#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Maurizio Cammalleri
# Build script for Luxia: ./build.sh --help says it all.
# No -march=native: nothing here depends on the CPU it is built on.
set -euo pipefail

cd "$(dirname "$0")"

usage()
{
    cat <<'EOF'
Luxia build script.

  ./build.sh [variant] [action]

  variant: release (default) | debug | asan | ubsan | tsan
  action:  build (default) | test | clean | clean-build | clean-test

Luxia is the language: its front end (src/front/, into libluxia.a) makes
the IR of Limba, and the program luxia runs it with the engine of Meri in
one process, or writes it (--emit). It links liblimba.a and libmeri.a,
and reads the private headers of Limba (LIMBA_DIR/src). build and test
first build Meri with its own build.sh, in the same variant; Meri builds
Limba.

Variants
  release  -O2, assertions off. What is shipped, and the only build a
           speed number may come from.
  debug    -O0 -g. Slow but honest under a debugger.
  asan     AddressSanitizer: out-of-bounds reads and writes, use after
           free, leaks at exit. About 2x slower.
  ubsan    UndefinedBehaviorSanitizer: signed overflow, oversized shifts,
           misaligned or null pointers. Stops at the first one.
  tsan     ThreadSanitizer: data races.

  The programs of a variant carry a -<variant> suffix (bin/<cpu>-<os>/
  luxia-asan), so they never overwrite the release build, and each
  variant keeps its own objects.

Actions
  build        build Meri and Limba, then the library and the programs
  test         build, then run the tests from the root of the repository:
               every test_* program of the variant; then luxia, with
               --verify, on every program of tests/run against its
               expected output, and on the benchmarks without external
               routines, against their expected output and against
               luxia --emit=lir and meri run one after the other; then
               the probes of the calls of C (tests/ffi) through meri.
               The outputs go to lib/<cpu>-<os>-<variant>/;
               exit 1 if one fails
  clean        remove the objects of the variant and the programs it made,
               and nothing else (Limba and Meri are left alone)
  clean-build  the two in a row, for a build from nothing
  clean-test   the same, then the tests: the honest answer before a commit

Output
  Objects and libluxia.a go to lib/<cpu>-<os>-<variant>/, programs to
  bin/<cpu>-<os>/, with a -<variant> suffix for every variant but release.
  Each src/<tool>/main.c becomes the program <tool> (luxia), with the other
  .c files of its directory; every other .c under src/ goes into
  libluxia.a. Each tests/*.c and tools/*.c becomes a program too. Every
  program is linked with libluxia.a, libmeri.a and liblimba.a.

Environment
  CC              the compiler, gcc by default
  LIMBA_DIR       where Limba is, ../Limba by default
  MERI_DIR        where Meri is, ../Meri by default
  LUXIA_CFLAGS    flags added to the variant, to try a compiler out
                  (Limba and Meri take their own, LIMBA_CFLAGS and
                  MERI_CFLAGS)

Before a commit: ./build.sh release test and at least asan.

Examples
  ./build.sh                 a release build
  ./build.sh release test    build it and run the tests
  ./build.sh asan test       the same under AddressSanitizer
  ./build.sh asan clean      throw the asan objects away
EOF
}

case "${1:-}" in
-h | --help | help)
    usage
    exit 0
    ;;
esac

VARIANT="${1:-release}"
ACTION="${2:-build}"
CC="${CC:-gcc}"

CPU="$(uname -m)"
OS="$(uname -s | tr '[:upper:]' '[:lower:]')"
OBJ="lib/$CPU-$OS-$VARIANT"
BIN="bin/$CPU-$OS"
SUFFIX=""
[ "$VARIANT" != release ] && SUFFIX="-$VARIANT"

# the flags of Meri: the same objects, the same rules
CFLAGS=(-std=gnu11 -Wall -Wextra -Werror -pthread -ffp-contract=off -Isrc
    -Iinclude)
case "$VARIANT" in
release) CFLAGS+=(-O2 -DNDEBUG) ;;
debug) CFLAGS+=(-O0 -g) ;;
asan) CFLAGS+=(-O1 -g -fsanitize=address -fno-omit-frame-pointer) ;;
ubsan) CFLAGS+=(-O1 -g -fsanitize=undefined -fno-sanitize-recover=all) ;;
tsan) CFLAGS+=(-O1 -g -fsanitize=thread) ;;
*)
    echo "build.sh: unknown variant: $VARIANT" >&2
    case "$VARIANT" in
    build | test | clean)
        echo "build.sh: the variant comes first: ./build.sh release" \
            "$VARIANT" >&2 ;;
    esac
    echo "build.sh: --help says which variants there are" >&2
    exit 2
    ;;
esac

case "$ACTION" in
build | test | clean | clean-build | clean-test) ;;
*)
    echo "build.sh: unknown action: $ACTION" >&2
    echo "build.sh: --help says which actions there are" >&2
    exit 2
    ;;
esac
LDLIBS=(-lm)
if [ -n "${LUXIA_CFLAGS:-}" ]; then
    # shellcheck disable=SC2206
    CFLAGS+=($LUXIA_CFLAGS)
    LDLIBS+=($LUXIA_CFLAGS)
fi
VERSION="$(git describe --always --dirty 2>/dev/null || echo unknown)"
CFLAGS+=(-DLUXIA_VERSION="\"$VERSION\"")

# Throw away what this variant made, and only what this variant made.
clean()
{
    rm -rf "$OBJ"
    if [ -n "$SUFFIX" ]; then
        rm -f "$BIN"/*"$SUFFIX"
        return
    fi
    for f in "$BIN"/*; do
        [ -e "$f" ] || continue
        case "$f" in
        *-debug | *-asan | *-ubsan | *-tsan) continue ;;
        esac
        rm -f "$f"
    done
}

case "$ACTION" in
clean)
    clean
    exit 0
    ;;
clean-build)
    clean
    ACTION=build
    ;;
clean-test)
    clean
    ACTION=test
    ;;
esac

# Meri first, in the same variant: it builds Limba, the same variant too
LIMBA_DIR="${LIMBA_DIR:-../Limba}"
MERI_DIR="${MERI_DIR:-../Meri}"
for d in "$LIMBA_DIR" "$MERI_DIR"; do
    if [ ! -x "$d/build.sh" ]; then
        echo "build.sh: $d not found (set LIMBA_DIR and MERI_DIR)" >&2
        exit 2
    fi
done
LIMBA_DIR="$(cd "$LIMBA_DIR" && pwd)"
MERI_DIR="$(cd "$MERI_DIR" && pwd)"
for d in "$LIMBA_DIR" "$MERI_DIR"; do
    echo "$(basename "$d" | tr '[:lower:]' '[:upper:]') $d" \
        "($(git -C "$d" describe --always --dirty 2>/dev/null || echo unknown))"
done
LIMBA_DIR="$LIMBA_DIR" "$MERI_DIR/build.sh" "$VARIANT" build >/dev/null
LIBLIMBA="$LIMBA_DIR/lib/$CPU-$OS-$VARIANT/liblimba.a"
LIBMERI="$MERI_DIR/lib/$CPU-$OS-$VARIANT/libmeri.a"
CFLAGS+=(-I"$MERI_DIR/include" -I"$LIMBA_DIR/include" -I"$LIMBA_DIR/src")

mkdir -p "$OBJ" "$BIN"

# one object, rebuilt when its source or any header is newer, those of
# Limba and Meri too
srcdirs=()
for d in src include "$MERI_DIR/include" "$LIMBA_DIR/include" \
    "$LIMBA_DIR/src"; do
    [ -d "$d" ] && srcdirs+=("$d")
done
headers_newest=$(find "${srcdirs[@]}" \( -name '*.h' -o -name '*.def' \) \
    -printf '%T@\n' | sort -n | tail -1)
headers_newest="${headers_newest:-0}"
compile()
{ # source, object, then the flags
    local src="$1" obj="$2"
    shift 2
    if [ ! -f "$obj" ] || [ "$src" -nt "$obj" ] ||
        [ "$(stat -c %Y "$obj")" -lt "${headers_newest%.*}" ]; then
        echo "CC  $src"
        "$CC" "$@" -c "$src" -o "$obj"
    fi
}
objname() { echo "$OBJ/$(echo "${1#src/}" | tr '/' '_' | sed 's/\.c$/.o/')"; }

# the library: every .c of src/ that is not part of a program
tool_dirs=$(for m in src/*/main.c; do [ -f "$m" ] && dirname "$m"; done)
objs=()
while IFS= read -r c; do
    dir=$(dirname "$c")
    case " $(echo $tool_dirs) " in *" $dir "*) continue ;; esac
    obj=$(objname "$c")
    objs+=("$obj")
    compile "$c" "$obj" "${CFLAGS[@]}"
done < <(find src -name '*.c' ! -name main.c | sort)
rm -f "$OBJ/libluxia.a"
ar rcs "$OBJ/libluxia.a" "${objs[@]}"

# Luxia, then Meri, then Limba: each library uses the next
libs=("$OBJ/libluxia.a" "$LIBMERI" "$LIBLIMBA")
for src in tests/*.c tools/*.c src/*/main.c; do
    [ -f "$src" ] || continue
    exe="$BIN/$(basename "${src%.c}")$SUFFIX"
    own=()
    if [ "$(basename "$src")" = main.c ]; then
        dir=$(dirname "$src")
        exe="$BIN/$(basename "$dir")$SUFFIX"
        while IFS= read -r c; do
            obj=$(objname "$c")
            own+=("$obj")
            compile "$c" "$obj" "${CFLAGS[@]}"
        done < <(find "$dir" -maxdepth 1 -name '*.c' ! -name main.c | sort)
    fi
    echo "LD  $exe"
    "$CC" "${CFLAGS[@]}" "$src" "${own[@]}" "${libs[@]}" \
        "${LDLIBS[@]}" -o "$exe"
done

if [ "$ACTION" = test ]; then
    status=0
    for t in "$BIN"/test_*"$SUFFIX"; do
        [ -e "$t" ] || continue
        # in release, skip the sanitizer builds (test_x-asan and the like)
        [ -n "$SUFFIX" ] || [[ "$(basename "$t")" != *-* ]] || continue
        echo "RUN $t"
        "$t" || status=1
    done

    luxia="$BIN/luxia$SUFFIX"
    meri="$MERI_DIR/bin/$CPU-$OS/meri$SUFFIX"
    out="$OBJ/out"
    mkdir -p "$out"

    # tests/run/<name>.luxia: its output in expected/<name>.out, and if
    # there, its standard error in <name>.err and its exit status in
    # <name>.status (0 otherwise); its input <name>.in (nothing otherwise);
    # the options of luxia before the source in <name>.opts, one a line
    # (none otherwise), paths from the root of the repository
    n=0
    for src in tests/run/*.luxia; do
        [ -f "$src" ] || continue
        name=$(basename "$src" .luxia)
        exp="tests/run/expected/$name"
        in=/dev/null
        [ -f "tests/run/$name.in" ] && in="tests/run/$name.in"
        opts=()
        [ -f "tests/run/$name.opts" ] && mapfile -t opts <"tests/run/$name.opts"
        want=0
        [ -f "$exp.status" ] && want=$(cat "$exp.status")
        got=0
        "$luxia" --verify "${opts[@]}" "$src" <"$in" >"$out/$name.out" \
            2>"$out/$name.err" || got=$?
        n=$((n + 1))
        if [ "$got" != "$want" ] || ! cmp -s "$exp.out" "$out/$name.out" ||
            { [ -f "$exp.err" ] && ! cmp -s "$exp.err" "$out/$name.err"; }; then
            echo "tests/run/$name: exit $got (expected $want), or" \
                "not the expected output"
            status=1
        fi
    done
    echo "RUN tests/run: $n programs"

    # the benchmarks: luxia, the IR verified in every variant, must print,
    # and end, as luxia --emit=lir and meri one after the other, and as
    # expected (those of c/ against the output of the program they
    # translate). Those with external routines need their libraries: they
    # are left to the measures
    bench=tests/benchmarks
    n=0
    for src in "$bench"/*.luxia "$bench"/c/*.luxia; do
        [ -f "$src" ] || continue
        grep -q '^ *external\|external "' "$src" && continue
        name=$(basename "$src" .luxia)
        e=${name%-c}
        in=/dev/null
        [ -f "$bench/expected/$e.in" ] && in="$bench/expected/$e.in"
        for o in 0 1; do
            n=$((n + 1))
            got=0
            "$luxia" --verify -O$o "$src" <"$in" >"$out/$name-O$o.out" \
                2>"$out/$name-O$o.err" || got=$?
            want=0
            if ! "$luxia" --emit=lir -O$o -o "$out/$name-O$o.lir" "$src"; then
                want=emit
            else
                "$meri" "$out/$name-O$o.lir" <"$in" \
                    >"$out/$name-O$o.meri.out" 2>"$out/$name-O$o.meri.err" ||
                    want=$?
            fi
            if [ "$got" != "$want" ] ||
                ! cmp -s "$out/$name-O$o.meri.out" "$out/$name-O$o.out" ||
                ! cmp -s "$out/$name-O$o.meri.err" "$out/$name-O$o.err" ||
                ! cmp -s "$bench/expected/$e.out" "$out/$name-O$o.out"; then
                echo "$name -O$o: exit $got (luxia --emit=lir and meri:" \
                    "$want), or not their output, or not the expected one"
                status=1
            fi
        done
    done
    echo "RUN benchmarks: $n runs against luxia --emit=lir and meri"

    # the calls of C (Limba's IR, progetto_ir.md § 11e): the probes against
    # their library of known signatures, built here, the output computed
    # by hand (lir_run does not call C)
    ffi=tests/ffi
    if [ -f "$ffi/probe.c" ] && [ -x "$meri" ]; then
        # the library built by gcc and by clang at -O2: the callees of
        # clang rely on the caller's extension of the narrow integers
        for cc in gcc clang; do
            command -v "$cc" >/dev/null || continue
            echo "RUN probes.luxia against libprobe built by $cc"
            mkdir -p "$OBJ/probe-$cc"
            "$cc" -O2 -shared -fPIC -o "$OBJ/probe-$cc/libprobe.so" \
                "$ffi/probe.c" || status=1
            for o in 0 1; do
                if ! "$luxia" --emit=lir -O$o -o "$OBJ/probes-O$o.lir" \
                    "$ffi/probes.luxia" ||
                    ! "$meri" --lib-path="$OBJ/probe-$cc" \
                        "$OBJ/probes-O$o.lir" >"$OBJ/probes-O$o.out" ||
                    ! cmp -s "$ffi/expected/probes.out" \
                        "$OBJ/probes-O$o.out"; then
                    echo "probes.luxia -O$o, libprobe by $cc:" \
                        "not the expected output"
                    status=1
                fi
            done
        done
    fi
    exit $status
fi
