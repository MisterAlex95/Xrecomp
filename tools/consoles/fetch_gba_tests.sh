#!/usr/bin/env bash
# Fetches the jsmolka/gba-tests ROMs (MIT, https://github.com/jsmolka/gba-tests) used as external CPU / memory /
# BIOS / save conformance suites by consoles/gba/tests (XBA-152, XBA-156). Never committed: they land in build/.
#
#   tools/consoles/fetch_gba_tests.sh [OUTDIR]        (default build/gba-tests)
#       -> OUTDIR/{arm,thumb,memory,bios}.gba and, from save/, OUTDIR/{sram,flash64,flash128,none}.gba
#
# The suites are written for FASMARM, which no image of ours carries, so this takes the ROMs the repository
# ships at a pinned commit (the tarball is checked against its sha256) instead of assembling them. Each ROM
# leaves the number of its first failed test in r12 and spins: gba_run ROM --reg-result 12.
set -euo pipefail

COMMIT=a7113b67e63f83a9b321696ddd7042ccfad6c881 # 2025-04-19
SHA256=b9373ef18700c48ccab0940ec480517fba47accc86516df5fd43ed9ec6cdf082
URL=https://codeload.github.com/jsmolka/gba-tests/tar.gz/$COMMIT

out=${1:-build/gba-tests}
mkdir -p "$out"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
curl -fsSL --retry 3 -o "$work/src.tgz" "$URL"
echo "$SHA256  $work/src.tgz" | sha256sum -c --quiet -
mkdir "$work/src"
tar xzf "$work/src.tgz" -C "$work/src" --strip-components=1
for t in arm thumb memory bios; do
    cp "$work/src/$t/$t.gba" "$out/$t.gba"
done
for t in sram flash64 flash128 none; do
    cp "$work/src/save/$t.gba" "$out/$t.gba"
done
cp "$work/src/LICENSE" "$out/LICENSE"
echo "jsmolka/gba-tests $COMMIT -> $out"
ls -l "$out"
