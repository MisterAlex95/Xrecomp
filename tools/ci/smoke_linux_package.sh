#!/bin/sh
# Smoke test of a Linux release archive (.github/workflows/desktop.yml, XBA-87), from the archive as a user has it:
#
#   sh tools/ci/smoke_linux_package.sh ARCHIVE [--cmake]
#
# 1. ./xrecomp prepare on the synthetic conformance XBE with a retail-keyed entry point (mkxbe.py --retail): the
#    archive's prebuilt runtime (runtime/<host>/) and the system compiler, no CMake (a cmake that fails comes first
#    on PATH); the title executable then starts headless and loads the XBE.
# 2. ./xrecomp play on a tiny synthetic GBA ROM, PS-X EXE and N64 ROM (a branch to itself each: no game code): the
#    players the archive carries (runtime/<host>/bin/) run 60 frames headless.
# 3. --cmake: the runtime's CMake build from the same archive, frame generation from the tarballs it carries.
#
# The machine needs what README « Linux » lists (tools/linux/install_deps.sh). Environment:
#   PYTHON            a Python with capstone, pefile and numpy (default python3)
#   XBA_RUN_WRAPPER   runs the arm64 executables on another host, e.g. "qemu-aarch64 -L /" with XBA_HOST=linux-arm64
#                     and XBA_CC=aarch64-linux-gnu-gcc (a cross check on x86-64; GitHub's arm64 runner needs none)
set -eu

[ $# -ge 1 ] || { echo "usage: $0 ARCHIVE [--cmake]" >&2; exit 2; }
archive=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
cmake_build=${2:-}
py=${PYTHON:-python3}
wrap=${XBA_RUN_WRAPPER:-}
export SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy

t=$(mktemp -d)
tar -C "$t" -xzf "$archive"
cd "$t/XRecomp"
host=$("$py" -c 'from tools.prepare import runtime; print(runtime.host_id())')
echo "== $archive: host $host"
ls "runtime/$host/runtime.json" "runtime/$host/bin/gba_play" "runtime/$host/bin/ps1_play" "runtime/$host/bin/n64_play"

# 1. Xbox: prepare without CMake, then start the executable.
mkdir -p "$t/game" "$t/nocmake"
"$py" tools/conformance/mkxbe.py --retail "$t/game/default.xbe"
for tool in cmake ninja; do
    printf '#!/bin/sh\necho "%s must not run" >&2\nexit 99\n' "$tool" > "$t/nocmake/$tool"
    chmod +x "$t/nocmake/$tool"
done
PATH="$t/nocmake:$PATH" "$py" xrecomp --home "$t/home" prepare "$t/game" 2>&1 | tee "$t/prepare.log"
grep -q 'compile driver:' "$t/prepare.log" || { echo "prepare did not use the compile driver"; exit 1; }
if grep -q 'must not run' "$t/prepare.log"; then echo "prepare ran CMake"; exit 1; fi
"$py" xrecomp --home "$t/home" list | tee "$t/list.txt"
grep -q '^12345678 .*Prepared' "$t/list.txt"
exe=$t/home/titles/12345678/work/project/build/title_12345678
# shellcheck disable=SC2086 # $wrap is a command line
(cd "$(dirname "$exe")" && timeout 30 $wrap "$exe" > "$t/run.log" 2>&1) || true
grep -q 'XBE loaded' "$t/run.log" || { cat "$t/run.log"; exit 1; }
echo "== title_12345678 started: $(grep -c . "$t/run.log") lines of output"

# 2. The consoles: ./xrecomp play finds the archive's players.
"$py" - "$t/roms" <<'EOF'
import os, struct, sys
d = sys.argv[1]
os.makedirs(d)
gba = bytearray(4096)                       # ARM "b ." at the entry point, the header's complement check
gba[0:4] = struct.pack("<I", 0xEAFFFFFE)
gba[0xA0:0xB0] = b"XBA SMOKE   XBAS"
gba[0xB2] = 0x96
gba[0xBD] = (-sum(gba[0xA0:0xBD]) - 0x19) & 0xFF
open(os.path.join(d, "smoke.gba"), "wb").write(gba)
exe = bytearray(0x1000)                     # PS-X EXE, text at 0x80010000: "j 0x80010000; nop"
exe[:8] = b"PS-X EXE"
struct.pack_into("<10I", exe, 0x10, 0x80010000, 0, 0x80010000, 0x800, 0, 0, 0, 0, 0x801FFFF0, 0)
struct.pack_into("<2I", exe, 0x800, 0x08004000, 0)
open(os.path.join(d, "smoke.exe"), "wb").write(exe)
n64 = bytearray(0x101000)                   # z64, entry 0x80000400 (ROM 0x1000): "j 0x80000400; nop"
n64[0:4] = b"\x80\x37\x12\x40"
n64[8:12] = (0x80000400).to_bytes(4, "big")
n64[0x20:0x34] = b"XBA SMOKE".ljust(20)
n64[0x3B:0x3F] = b"NXSE"
n64[0x1000:0x1008] = struct.pack(">2I", 0x08000100, 0)
open(os.path.join(d, "smoke.z64"), "wb").write(n64)
EOF
for rom in smoke.gba smoke.exe smoke.z64; do
    "$py" xrecomp --home "$t/home" play "$t/roms/$rom" --print > "$t/play.txt"
    player=$(tail -n 1 "$t/play.txt" | cut -d' ' -f1)
    case "$player" in
        "$t/XRecomp/runtime/$host/bin/"*) ;;
        *) cat "$t/play.txt"; echo "$rom: not the archive's player"; exit 1 ;;
    esac
    if [ -z "$wrap" ]; then
        timeout 60 "$py" xrecomp --home "$t/home" play "$t/roms/$rom" -- --headless --frames 60 > "$t/player.log" 2>&1 \
            || { cat "$t/player.log"; echo "$rom: $(basename "$player") failed"; exit 1; }
    else
        # shellcheck disable=SC2086 # $wrap is a command line
        timeout 120 $wrap "$player" "$t/roms/$rom" --headless --frames 60 > "$t/player.log" 2>&1 \
            || { cat "$t/player.log"; echo "$rom: $(basename "$player") failed"; exit 1; }
    fi
    echo "== $(basename "$player") $rom: 60 frames"
done

# 3. The runtime's CMake build from the same archive (frame generation is in the default build, XBA-187: no fetch).
if [ "$cmake_build" = --cmake ]; then
    cmake -S . -B "$t/build" -DCMAKE_BUILD_TYPE=Release | tee "$t/configure.log"
    grep -q 'LSFG: bridge built' "$t/configure.log"
    if grep -q 'LSFG: fetching' "$t/configure.log"; then echo "the archive's tarballs were not used"; exit 1; fi
    cmake --build "$t/build" -j"$(nproc)"
    nm "$t/build/src/framegen/libxbox_framegen.a" | grep -q ' T lsfg_bridge_open$'
fi
rm -rf "$t"
echo "== smoke test passed: $archive"
