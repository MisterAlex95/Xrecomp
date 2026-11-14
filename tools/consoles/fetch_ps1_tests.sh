#!/usr/bin/env bash
# Fetches JaCzekanski/ps1-tests (MIT, https://github.com/JaCzekanski/ps1-tests), the PlayStation hardware test
# programs, for the PS1 core's tests (consoles/ps1/tests: ps1_jcz_*). XBA-162 / XBA-164.
#
#   tools/consoles/fetch_ps1_tests.sh [OUTDIR]        (default build/ps1-tests -> OUTDIR/cpu/cop/cop.exe, ...)
#
# Takes the project's own CI release build-158 (2021-04-22, built from the MIT sources with PSn00bSDK; the last
# release, the source is unchanged since but for docs), checked against its sha256. The zip's MIT licence travels with
# it (OUTDIR/LICENSE, fetched from the same commit). Our own test code, our BIOS (OpenBIOS) and no game data: nothing
# here is a Sony file. The output is never committed (build/ is ignored).
set -euo pipefail

TAG=build-158
SHA256=5de0e82854c1a511697a7ade381c2d778ec4b2fff4a7d7ac79e8098c35eb859b
URL=https://github.com/JaCzekanski/ps1-tests/releases/download/$TAG/tests.zip
SRC_COMMIT=f727802fead11f1daa7549285548392ef87749cb
LICENSE_URL=https://raw.githubusercontent.com/JaCzekanski/ps1-tests/$SRC_COMMIT/LICENSE

out=${1:-build/ps1-tests}
mkdir -p "$out"
out=$(cd "$out" && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

echo "== JaCzekanski/ps1-tests $TAG"
curl -fsSL --retry 3 -o "$work/tests.zip" "$URL"
echo "$SHA256  $work/tests.zip" | sha256sum -c --quiet -
python3 -I - "$work/tests.zip" "$out" <<'EOF'
import os, sys, zipfile
z = zipfile.ZipFile(sys.argv[1])
dest = os.path.realpath(sys.argv[2])
for info in z.infolist():
    target = os.path.realpath(os.path.join(dest, info.filename))
    if not target.startswith(dest + os.sep):
        sys.exit("unsafe path in archive: " + info.filename)
z.extractall(dest)
EOF
curl -fsSL --retry 3 -o "$out/LICENSE" "$LICENSE_URL"
echo "$TAG ($URL, sha256 $SHA256)" >"$out/VERSION"
echo "== $out ($(find "$out" -name '*.exe' | wc -l) programs)"
