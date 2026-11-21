#!/bin/sh
# run_vmx_ref_matrix.sh WORKDIR [CC [RUN]] -- x360_vmx_ref at -O0, -O1 and -O2 with one compiler, for a developer
# machine; prints the float digest of each build (it must be the same everywhere: every compiler, -O level and host).
# From the repository root, for instance:
#
#   sh consoles/x360/tests/run_vmx_ref_matrix.sh /tmp/vr gcc
#   docker run --rm -v "$PWD:/src" -w /src xba-arm64-deps sh -c \
#     'sh consoles/x360/tests/run_vmx_ref_matrix.sh /tmp/vr aarch64-linux-gnu-gcc \
#        "qemu-aarch64 -L $(dirname $(dirname $(aarch64-linux-gnu-gcc -print-file-name=libc.so.6)))"'
#   NDK clang (xba-android), static, run natively (x86_64) or under qemu-aarch64:
#     CC="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/clang --target=aarch64-linux-android29 -static"
set -e
work=$1
cc=${2:-cc}
run=${3:-}
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../../.." && pwd)
mkdir -p "$work"
for o in -O0 -O1 -O2; do
    exe="$work/vmx_ref$o"
    $cc -std=gnu11 $o -Wall -Wextra -Wno-unused-function -ffp-contract=off $XV_CFLAGS -I "$repo/consoles/x360/runtime" \
        -I "$repo/consoles/gc/runtime" -I "$here" "$here/x360_vmx_ref_test.c" "$here/x360_vmx_ref_scalar.c" -lm -o "$exe"
    out=$($run "$exe" | tail -2 | tr '\n' ' ')
    echo "$o: $out"
done
