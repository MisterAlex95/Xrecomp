#!/bin/sh
# dc_hello / dc_rc_hello (XBA-227): our KallistiOS hello world under dc_run.
#   run_hello.sh DC_RUN HELLO.elf WORKDIR interp|rc
# interp: the interpreter alone. rc: dc_tools.recomp + cc, then dc_run --pack, with the discovery loop (the
# addresses the interpreter had to run are lifted in the next round; at most 4 rounds) and at least 99 % of the
# instructions native. Both: the program returns to the BIOS with code 0, its "hello: " serial lines equal
# hello_serial.txt and its framebuffer when it printed "hello: done" equals hello_fb.png (png_compare.py --update
# to rewrite it after an intended change). Skipped (77) when the homebrew is not built.
run=$1
elf=$2
work=$3
mode=$4
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../../.." && pwd)
if [ ! -f "$elf" ]; then
    echo "skipped: $elf not built (tools/consoles/build_homebrew.sh dc)"
    exit 77
fi
py=${PYTHON:-python3}
command -v "$py" >/dev/null 2>&1 || { echo "skipped: no $py"; exit 77; }
rm -rf "$work"
mkdir -p "$work"

check() { # $1: dc_run's output
    echo "$1" | grep -q '^exit 0$' || { echo "$1"; echo "the program did not return 0"; return 1; }
    grep '^hello: ' "$work/serial.txt" >"$work/hello.txt"
    if ! cmp -s "$work/hello.txt" "$here/hello_serial.txt"; then
        echo "serial output differs from hello_serial.txt:"
        diff "$here/hello_serial.txt" "$work/hello.txt"
        return 1
    fi
    echo "serial: $(wc -l <"$work/hello.txt") lines as expected"
    "$py" "$repo/consoles/nds/tests/png_compare.py" "$work/fb.png" "$here/hello_fb.png"
}

if [ "$mode" = interp ]; then
    out=$(timeout 120 "$run" "$elf" --frames 600 --serial "$work/serial.txt" --png "$work/fb.png" \
        --png-when "hello: done")
    check "$out"
    exit $?
fi
cc=${CC:-cc}
command -v "$cc" >/dev/null 2>&1 || { echo "skipped: no C compiler"; exit 77; }
: >"$work/discovered.txt"
round=1
while :; do
    PYTHONPATH="$repo/consoles/dc/tools" timeout 300 "$py" -m dc_tools.recomp "$elf" "$work/src" \
        --discovered "$work/discovered.txt" --no-comments --report "$work/report.json" >/dev/null || exit 1
    "$cc" -shared -fPIC -O1 -DDC_RC_COUNT -I "$repo/consoles/dc/runtime" -o "$work/libtitle.so" \
        "$work"/src/*.c -lm || exit 1
    out=$(timeout 120 "$run" "$elf" --frames 600 --pack "$work/libtitle.so" --serial "$work/serial.txt" \
        --png "$work/fb.png" --png-when "hello: done" --stats --missed "$work/missed.txt")
    new=$(grep -v '^#' "$work/missed.txt" | sort -u | comm -23 - "$work/discovered.txt" | wc -l)
    echo "round $round: $(echo "$out" | grep '^stats'), $new new address(es)"
    if [ "$new" -eq 0 ] || [ "$round" -ge 4 ]; then
        break
    fi
    grep -v '^#' "$work/missed.txt" | cat - "$work/discovered.txt" | sort -u >"$work/d.tmp"
    mv "$work/d.tmp" "$work/discovered.txt"
    round=$((round + 1))
done
"$py" - "$work/report.json" <<'EOF'
import json, sys
r = json.load(open(sys.argv[1]))
print("lifted: %d functions, %d table entries, %d instructions, %d jump tables, %d interpreter fallbacks, spans %s"
      % (r["functions"], r["entries"], r["instructions"], r["jump_tables"], r["interp_fallbacks"],
         ", ".join(r["image"]["spans"])))
EOF
native=$(echo "$out" | sed -n 's/^stats: native \([0-9]*\) interpreted \([0-9]*\).*/\1/p')
interp=$(echo "$out" | sed -n 's/^stats: native \([0-9]*\) interpreted \([0-9]*\).*/\2/p')
if [ -z "$native" ] || [ "$native" -le $((interp * 99)) ]; then
    echo "too little native code: native ${native:-?}, interpreted ${interp:-?} (want 99 % native)"
    exit 1
fi
check "$out"
