#!/usr/bin/env bash
set -euo pipefail

: "${OO_PS4_TOOLCHAIN:?set OO_PS4_TOOLCHAIN to the OpenOrbis PS4Toolchain root}"
: "${LLVM_BIN:?set LLVM_BIN to a directory containing clang and ld.lld}"

CLANG=${CLANG:-$LLVM_BIN/clang}
LLD=${LLD:-$LLVM_BIN/ld.lld}
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

compile_only=0
out=a.out
args=()
inputs=()
next_is_out=0

for arg in "$@"; do
    if [[ $next_is_out == 1 ]]; then
        out=$arg
        args+=("$arg")
        next_is_out=0
        continue
    fi
    case "$arg" in
        -c) compile_only=1; args+=("$arg") ;;
        -o) next_is_out=1; args+=("$arg") ;;
        *.c|*.o|*.a) inputs+=("$arg"); args+=("$arg") ;;
        -Wl,*|-l*|-L*) ;; # final PS4 link uses the fixed audited set below
        *) args+=("$arg") ;;
    esac
done

if [[ $compile_only == 1 ]]; then
    exec "$CLANG" -target x86_64-scei-ps4-elf -funwind-tables \
        -isysroot "$OO_PS4_TOOLCHAIN" -isystem "$OO_PS4_TOOLCHAIN/include" \
        "${args[@]}"
fi

tmpdir=$(mktemp -d)
trap 'rm -rf "$tmpdir"' EXIT
objs=()
for input in "${inputs[@]}"; do
    case "$input" in
        *.c)
            obj="$tmpdir/$(basename "${input%.c}").o"
            "$CLANG" -target x86_64-scei-ps4-elf -funwind-tables \
                -isysroot "$OO_PS4_TOOLCHAIN" -isystem "$OO_PS4_TOOLCHAIN/include" \
                -I. -O2 -D__ORBIS__ -D_GNU_SOURCE -c "$input" -o "$obj"
            objs+=("$obj")
            ;;
        *.o|*.a) objs+=("$input") ;;
    esac
done

compat_obj=$tmpdir/ps4_compat.o
"$CLANG" -target x86_64-scei-ps4-elf -funwind-tables \
    -isysroot "$OO_PS4_TOOLCHAIN" -isystem "$OO_PS4_TOOLCHAIN/include" \
    -O2 -D__ORBIS__ -D_GNU_SOURCE -c "$ROOT/build-aux/ps4_compat.c" -o "$compat_obj"

exec "$LLD" -o "$out" -m elf_x86_64 -pie --eh-frame-hdr \
    --script "$OO_PS4_TOOLCHAIN/link.x" \
    "$OO_PS4_TOOLCHAIN/lib/crt1.o" "$OO_PS4_TOOLCHAIN/lib/crti.o" \
    -L"$OO_PS4_TOOLCHAIN/lib" "${objs[@]}" "$compat_obj" \
    -lkernel -lc "$OO_PS4_TOOLCHAIN/lib/crtn.o"
