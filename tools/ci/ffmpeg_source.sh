#!/bin/sh
# ffmpeg_source.sh OUT_DIR -- FFmpeg's source tarball for a release page (XBA-299). The APK ships libavcodec and libavutil
# under the LGPL, so each release carries the exact, unmodified source they were built from. The version and the checksum
# are tools/android/build_ffmpeg.sh's. Prints the file's path.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
script=$here/../android/build_ffmpeg.sh
version=$(sed -n 's/^VERSION=//p' "$script")
sha256=$(sed -n 's/^SHA256=//p' "$script")
file=$1/ffmpeg-$version.tar.xz
mkdir -p "$1"
[ -f "$file" ] || curl -fsSL -o "$file" "https://ffmpeg.org/releases/ffmpeg-$version.tar.xz"
echo "$sha256  $file" | sha256sum -c - >&2
echo "$file"
