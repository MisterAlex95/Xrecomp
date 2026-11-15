#!/bin/sh
# Fetches the pinned frame generation sources (src/framegen/lsfg_sources.txt: framegen with volk, DXVK with
# SPIRV-Headers) as the tarballs src/framegen/lsfg_sources.cmake looks for, <name>-<sha256:16>.tar.gz, each checked
# against its SHA-256. A tarball already there and intact is kept. Used by tools/ci/package_toolkit.sh (the desktop
# downloads build offline) and handy before an offline build (docs/technical/framegen.md « Build »).
#
#   sh tools/lsfg/fetch_sources.sh [DIR]     # default: $XBOXRECOMP_DEPS_DIR, else third_party/lsfg-sources
set -eu
root=$(cd "$(dirname "$0")/../.." && pwd)
dir=${1:-${XBOXRECOMP_DEPS_DIR:-$root/third_party/lsfg-sources}}
pins=$root/src/framegen/lsfg_sources.txt
mkdir -p "$dir"

sha256() {
    if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | cut -d' ' -f1; else shasum -a 256 "$1" | cut -d' ' -f1; fi
}

grep -v '^#' "$pins" | while read -r name sha url; do
    [ -n "$name" ] || continue
    f="$dir/$name-$(printf '%s' "$sha" | cut -c1-16).tar.gz"
    if [ -f "$f" ] && [ "$(sha256 "$f")" = "$sha" ]; then
        echo "$f: ok"
        continue
    fi
    curl -fsSL --retry 3 -o "$f.part" "$url"
    got=$(sha256 "$f.part")
    if [ "$got" != "$sha" ]; then
        rm -f "$f.part"
        echo "$name: SHA-256 $got, expected $sha ($url)" >&2
        exit 1
    fi
    mv "$f.part" "$f"
    echo "$f: fetched"
done
