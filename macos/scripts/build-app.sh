#!/bin/bash
# Builds macos/build/XRecomp.app: a release build of the SwiftPM package, an Info.plist pointing at this checkout
# (XBAToolkitPath), the icon from docs/images/xrecomp-icon.svg, and an ad-hoc signature (built and run on the same
# Mac: no Developer ID, no notarization). Command Line Tools are enough.
#   bash macos/scripts/build-app.sh
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
REPO="$(cd "$HERE/.." && pwd)"
cd "$HERE"

echo "== swift build -c release"
swift build -c release 2>&1 | grep -v "search path" | grep -E "error|warning: |Build complete" || true
swift build -c release >/dev/null 2>&1
BIN="$(swift build -c release --show-bin-path)"

APP="$HERE/build/XRecomp.app"
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
cp "$BIN/XRecomp" "$APP/Contents/MacOS/XRecomp"

VERSION="0.1.0"
BUILD="$(git -C "$REPO" rev-list --count HEAD 2>/dev/null || echo 1)"
esc() { printf '%s' "$1" | sed -e 's/[&|\\]/\\&/g' -e 's/</\&lt;/g' -e 's/>/\&gt;/g'; }
sed -e "s|@VERSION@|$VERSION|" -e "s|@BUILD@|$BUILD|" -e "s|@TOOLKIT@|$(esc "$REPO")|" \
    "$HERE/Info.plist.in" > "$APP/Contents/Info.plist"
plutil -lint "$APP/Contents/Info.plist" >/dev/null

echo "== icon"
ICONSET="$HERE/build/AppIcon.iconset"
rm -rf "$ICONSET"
if swift "$HERE/scripts/mkicon.swift" "$REPO/docs/images/xrecomp-icon.svg" "$ICONSET" \
   && iconutil -c icns "$ICONSET" -o "$APP/Contents/Resources/AppIcon.icns"; then
    rm -rf "$ICONSET"
else
    echo "warning: no icon (mkicon or iconutil failed); the app still works" >&2
fi

echo "== codesign (ad hoc)"
codesign --force --sign - --timestamp=none "$APP"
codesign --verify "$APP"
echo "$APP"
