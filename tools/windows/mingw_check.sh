#!/bin/sh
# The Windows route of the desktop prepare, checked from Linux (XBA-89, docs/desktop/windows.md): the runtime
# cross-built with MinGW-w64 into runtime/windows-x86_64 (tools/prepare/runtime.py under XBA_HOST=windows-x86_64),
# the synthetic retail XBE (tools/conformance/mkxbe.py --retail) prepared with the compile driver and
# x86_64-w64-mingw32-gcc, and, when wine is there, the executable started until « XBE loaded ».
#
#   sh tools/windows/mingw_check.sh [WORK]      # WORK: default a temporary folder (kept: it holds the .exe)
#
# Needs x86_64-w64-mingw32-gcc, cmake, ninja and Python with capstone, pefile and numpy. In the xba-deps image:
#   docker run --rm -v $PWD:/src:ro --tmpfs /work:exec xba-deps:latest sh -c \
#     'apt-get update -qq && apt-get install -y -qq gcc-mingw-w64-x86-64 >/dev/null &&
#      cp -r /src /work/t && cd /work/t && sh tools/windows/mingw_check.sh /work/check'
# then run the .exe it names with wine (from its folder: the game data is game/default.xbe beside it).
# The Windows download itself is built with MSVC on GitHub's windows runner (.github/workflows/desktop.yml).
set -eu
root=$(cd "$(dirname "$0")/../.." && pwd)
work=${1:-$(mktemp -d)}
py=${PYTHON:-python3}
command -v x86_64-w64-mingw32-gcc >/dev/null || { echo "no x86_64-w64-mingw32-gcc (apt install gcc-mingw-w64-x86-64)" >&2; exit 2; }

export XBA_HOST=windows-x86_64
cd "$root"
"$py" -m tools.prepare.runtime --out "$work/runtime" -- -DCMAKE_TOOLCHAIN_FILE="$root/cmake/mingw-w64-x86_64.cmake"
mkdir -p "$work/game"
"$py" tools/conformance/mkxbe.py --retail "$work/game/default.xbe"
XBA_RUNTIME="$work/runtime" "$py" -m tools.prepare "$work/game/default.xbe" --work "$work/w" \
    --cc x86_64-w64-mingw32-gcc | tee "$work/prepare.log"
grep -q 'compile driver:' "$work/prepare.log"
exe=$work/w/project/build/title_12345678.exe
[ -f "$exe" ] || { echo "no $exe" >&2; exit 1; }
echo "built $exe"
if command -v wine >/dev/null 2>&1; then
    (cd "$(dirname "$exe")" && WINEDEBUG=-all timeout 60 wine "$(basename "$exe")" > "$work/run.log" 2>&1) || true
    grep -q 'XBE loaded' "$work/run.log" || { cat "$work/run.log"; exit 1; }
    echo "wine: XBE loaded"
else
    echo "no wine here: run it with wine from $(dirname "$exe")"
fi
