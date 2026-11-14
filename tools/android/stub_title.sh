#!/usr/bin/env bash
# The stub title (XBA-52): templates/new-game around the synthetic XBE of tools/conformance (ours: five leaf
# functions, no game content), lifted by the pipeline. It proves that a title built from the template links into
# the app shell's libmain.so and boots there; on a desktop it builds as the template's executable.
#
#   tools/android/stub_title.sh OUT_DIR
#
# OUT_DIR gets: CMakeLists.txt and src/ (the template), src/recomp/gen/ (the pipeline's C, plus xbe_entry.c, which
# calls the five functions and prints their results), and game/default.xbe (the synthetic XBE, to push to the
# device). Then:
#   Android:  cd android && gradle assembleDebug -PxbaTitleDir=OUT_DIR -PxbaApplicationId=fr.adserver.xrecomp.stub
#             adb push OUT_DIR/game/default.xbe /sdcard/Android/data/fr.adserver.xrecomp.stub/files/game/
#   Desktop:  cmake -S OUT_DIR -B OUT_DIR/build -DXBOXRECOMP_DIR=$PWD && cmake --build OUT_DIR/build
#             (cd OUT_DIR && RECOMP_HEADLESS=1 build/your_game_recomp)
#
# Needs python3 with capstone. Runs the pipeline from this repository; its intermediate files go to the usual
# (git-ignored) tools/*/output directories.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=${1:?usage: stub_title.sh OUT_DIR}
PY=${PYTHON:-python3}

mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
cp -r "$ROOT/templates/new-game/." "$OUT/"
mkdir -p "$OUT/game" "$OUT/src/recomp/gen"
# The template's three constants (GETTING_STARTED step 6). On Android the two paths come from the app's storage.
sed -i -e 's|^#define YOUR_GAME_ENTRY_POINT .*|#define YOUR_GAME_ENTRY_POINT   0x00011000|' \
       -e 's|^#define YOUR_GAME_XBE_PATH .*|#define YOUR_GAME_XBE_PATH      "game/default.xbe"|' \
       -e 's|^#define YOUR_GAME_DIR .*|#define YOUR_GAME_DIR           "game"|' "$OUT/src/main.c"
grep -q '^#define YOUR_GAME_DIR           "game"$' "$OUT/src/main.c"

cd "$ROOT"
XBE="$OUT/game/default.xbe"
"$PY" tools/conformance/mkxbe.py "$XBE"
"$PY" -m tools.xbe_parser "$XBE" --json "$OUT/game/stub_analysis.json" >/dev/null
"$PY" -m tools.disasm "$XBE" --text-only >/dev/null
"$PY" -m tools.func_id "$XBE" >/dev/null
"$PY" -m tools.abi_analysis "$XBE" >/dev/null
"$PY" -m tools.recomp "$XBE" --all --split 250 --gen-dir "$OUT/src/recomp/gen" | tail -4

# The synthetic XBE's entry point field is not XOR-encoded like a retail one, so no lifted function is named
# xbe_entry_point. Ours calls the five as a caller in the guest would (arguments and a return address on the guest
# stack; each pops the return address) and checks what they return.
cat > "$OUT/src/recomp/gen/xbe_entry.c" <<'EOF'
/* Stub title entry (tools/android/stub_title.sh): the five functions of tools/conformance/mkxbe.py. */
#define RECOMP_GENERATED_CODE
#include "recomp_funcs.h"
#include <stdio.h>

static uint32_t guest_call(void (*fn)(void), uint32_t a, uint32_t b, uint32_t c)
{
    PUSH32(esp, c);
    PUSH32(esp, b);
    PUSH32(esp, a);
    PUSH32(esp, 0);          /* return address, popped by the callee's ret */
    fn();
    esp += 12;               /* cdecl: the caller drops the arguments */
    return eax;
}

void xbe_entry_point(void)
{
    static const struct { const char *name; void (*fn)(void); uint32_t a, b, c, want; } t[] = {
        { "add3(1, 2, 3)",   sub_00011000, 1, 2, 3, 6 },
        { "submul(7, 2)",    sub_00011010, 7, 2, 0, 15 },
        { "shifts(1, 4)",    sub_00011020, 1, 4, 0, 16 },
        { "cmpsel(5, 9)",    sub_00011030, 5, 9, 0, 5 },
        { "bits(6, 3)",      sub_00011040, 6, 3, 0, 9 },
    };
    int bad = 0;
    for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++) {
        uint32_t got = guest_call(t[i].fn, t[i].a, t[i].b, t[i].c);
        printf("stub title: %-14s = %u%s\n", t[i].name, got, got == t[i].want ? "" : "  MISMATCH");
        bad += got != t[i].want;
    }
    printf("stub title: %s\n", bad ? "FAILED" : "all 5 lifted functions OK");
}
EOF
echo "stub title in $OUT ($(ls "$OUT/src/recomp/gen" | wc -l) files in src/recomp/gen)"
