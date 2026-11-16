#!/bin/sh
# dc_naomi_hello (XBA-235): our KallistiOS hello world booted from a synthetic Naomi cartridge.
#   run_naomi_hello.sh DC_RUN HELLO.bin WORKDIR
# Wraps the raw binary in a cartridge image (a "NAOMI" header whose load list copies it to 0x8C010000 and whose
# entry point is there), runs dc_run --naomi on it (the Naomi memory map, the Naomi BIOS in C) and checks what
# run_hello.sh checks: exit code 0, the "hello: " serial lines equal hello_serial.txt, the framebuffer at
# "hello: done" equals hello_fb.png. Skipped (77) when the homebrew is not built.
run=$1
bin=$2
work=$3
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../../.." && pwd)
if [ ! -f "$bin" ]; then
    echo "skipped: $bin not built (tools/consoles/build_homebrew.sh dc)"
    exit 77
fi
py=${PYTHON:-python3}
command -v "$py" >/dev/null 2>&1 || { echo "skipped: no $py"; exit 77; }
rm -rf "$work"
mkdir -p "$work"

"$py" -I - "$bin" "$work/cart.bin" <<'EOF' || exit 1
import struct, sys
prog = open(sys.argv[1], "rb").read()
hdr = bytearray(0x1000)
hdr[0:16] = b"NAOMI           "
hdr[0x10:0x30] = b"XRECOMP SYNTHETIC TEST CART     "
struct.pack_into("<III", hdr, 0x360, len(hdr), 0x8C010000, len(prog))  # load: ROM offset, RAM address, length
struct.pack_into("<I", hdr, 0x36C, 0xFFFFFFFF)                          # end of the load list
struct.pack_into("<I", hdr, 0x3C0, 0xFFFFFFFF)                          # no test mode program
struct.pack_into("<II", hdr, 0x420, 0x8C010000, 0x8C010000)             # game / test entry points
open(sys.argv[2], "wb").write(bytes(hdr) + prog)
EOF

out=$(timeout 120 "$run" --naomi "$work/cart.bin" --frames 600 --serial "$work/serial.txt" --png "$work/fb.png" \
    --png-when "hello: done")
echo "$out" | grep -q '^exit 0$' || { echo "$out"; echo "the program did not return 0"; exit 1; }
grep '^hello: ' "$work/serial.txt" >"$work/hello.txt"
if ! cmp -s "$work/hello.txt" "$here/hello_serial.txt"; then
    echo "serial output differs from hello_serial.txt:"
    diff "$here/hello_serial.txt" "$work/hello.txt"
    exit 1
fi
echo "serial: $(wc -l <"$work/hello.txt") lines as expected (Naomi mode)"
"$py" "$repo/consoles/nds/tests/png_compare.py" "$work/fb.png" "$here/hello_fb.png"
