#!/bin/sh
# Name the frames of an Android tombstone backtrace with a release's crash symbols (docs/releases/README.md
# « Crash symbols »).
#
#   sh tools/symbolize.sh LOG SYMBOLS
#
#   LOG      a log with the backtrace (« Share the last game's log », logcat, a tombstone); - for stdin
#   SYMBOLS  xrecomp-v<version>-symbols.tar.gz, or a directory with lib/arm64-v8a/*.so in it
#
# Each "#NN pc OFFSET .../libX.so" frame whose library is in SYMBOLS is printed with its function (and file:line
# when the library has them). Needs llvm-symbolizer (LLVM, or the NDK's toolchains/llvm/prebuilt/*/bin) and,
# to check BuildIds, llvm-readelf; LLVM_BIN can name their directory.
set -eu
[ $# -eq 2 ] || { echo "usage: $0 LOG SYMBOLS" >&2; exit 2; }
log=$1 sym=$2
tool() {
    for t in ${LLVM_BIN:+"$LLVM_BIN/$1"} "$1" $(ls -d /usr/lib/llvm-*/bin/"$1" 2>/dev/null | sort -V | tail -1) \
        ${ANDROID_NDK_HOME:+"$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/bin/$1"}; do
        command -v "$t" >/dev/null 2>&1 && { echo "$t"; return; }
    done
}
symbolizer=$(tool llvm-symbolizer)
[ -n "$symbolizer" ] || { echo "llvm-symbolizer not found (set LLVM_BIN)" >&2; exit 1; }
readelf=$(tool llvm-readelf)

if [ -d "$sym" ]; then
    dir=$sym
else
    dir=$(mktemp -d)
    trap 'rm -rf "$dir"' EXIT
    tar -C "$dir" -xzf "$sym"
fi
libs="$dir/lib/arm64-v8a"
[ -d "$libs" ] || { echo "$sym: no lib/arm64-v8a" >&2; exit 1; }

tmp=$(mktemp)
trap 'rm -f "$tmp"; [ -d "$sym" ] || rm -rf "$dir"' EXIT
if [ "$log" = - ]; then cat > "$tmp"; else cat "$log" > "$tmp"; fi

# One frame a line: a pasted log may have lost its line breaks, so split before each "#NN pc".
sed 's/\(#[0-9][0-9]* pc \)/\n\1/g' "$tmp" | grep -E '^#[0-9]+ pc [0-9a-fA-F]+ ' | while read -r num _ pc path rest; do
    lib=$(basename "$path")
    so="$libs/$lib"
    if [ ! -f "$so" ]; then
        echo "$num $lib+0x$pc"
        continue
    fi
    want=$(printf '%s' "$rest" | sed -n 's/.*BuildId: \([0-9a-f]*\).*/\1/p')
    if [ -n "$want" ] && [ -n "$readelf" ]; then
        have=$("$readelf" -n "$so" | sed -n 's/.*Build ID: *\([0-9a-f]*\).*/\1/p')
        [ "$have" = "$want" ] || echo "warning: $lib BuildId $want in the log, $have in the symbols: another version" >&2
    fi
    fn=$("$symbolizer" --obj="$so" --functions=linkage --inlining=false "0x$pc" | head -2 | paste -sd ' ' - | sed 's/ ??:0:0$//')
    echo "$num $lib+0x$pc  $fn"
done
