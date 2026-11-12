#!/bin/sh
# The cache key of the on-device toolchain (XBA-125, docs/android/toolchain.md « The toolchain in CI »): 16 hex
# digits of a SHA-256 over what the toolchain-out stage of tools/docker/android.Dockerfile is built from, so that CI
# rebuilds it (about 30 min) only when one of these changes:
#   - every file in tools/android/toolchain/ (versions.env, the build scripts, check.sh, xbapython.c, ...);
#   - in android.Dockerfile, the base image and NDK version, and the toolchain stages (toolchain-base to toolchain-out).
# The rest of the repository does not count: the APK build takes the cached toolchain as is.
#   sh tools/ci/toolchain_key.sh [repo root]     # prints the key
set -eu
cd "${1:-$(dirname "$0")/../..}"
df=tools/docker/android.Dockerfile
[ -f "$df" ] && [ -f tools/android/toolchain/versions.env ] || { echo "toolchain_key: not a repository root" >&2; exit 1; }
{
    for f in $(find tools/android/toolchain -type f ! -name '*.pyc' | LC_ALL=C sort); do
        printf '%s %s\n' "$f" "$(sha256sum "$f" | cut -d' ' -f1)"
    done
    grep -E '^ARG (DEBIAN|NDK_VERSION)=' "$df"
    sed -n '/^FROM android AS toolchain-base/,/^FROM scratch AS toolchain-out/p' "$df"
} | sha256sum | cut -c1-16
