#!/bin/bash
# The launcher's local checks (there is no macOS CI runner): build, the check executable, and a smoke run of the
# app against a temporary library made from the synthetic tools/conformance/test.xbe (ours, no game content).
#   bash macos/scripts/check.sh
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
REPO="$(cd "$HERE/.." && pwd)"
cd "$HERE"

echo "== swift build"
swift build 2>&1 | grep -v "search path" | grep -E "error|warning: |Build complete" || true
swift build >/dev/null 2>&1   # fails the script on a build error
BIN="$(swift build --show-bin-path)"

echo "== xrecomp-check"
"$BIN/xrecomp-check"

echo "== XRecomp --smoke"
TMP="$(mktemp -d "${TMPDIR:-/tmp}/xrecomp-smoke.XXXXXX")"
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP/library/Synthetic Game" "$TMP/home"
cp "$REPO/tools/conformance/test.xbe" "$TMP/library/Synthetic Game/default.xbe"
OUT="$(XRECOMP_HOME="$TMP/home" "$BIN/XRecomp" --smoke --toolkit "$REPO" --library "$TMP/library")"
echo "$OUT"
echo "$OUT" | grep -q '"title_id" : "12345678"' || { echo "smoke: the synthetic title was not found" >&2; exit 1; }
echo "== all green"
