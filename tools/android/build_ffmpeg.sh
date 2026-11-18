#!/bin/sh
# build_ffmpeg.sh OUT [JOBS] -- the two shared libraries the app's Xbox 360 XMA decoder links (XBA-299): libavcodec.so
# and libavutil.so for arm64-v8a, API 30, from FFmpeg's pinned release tarball, with the NDK of $ANDROID_NDK_HOME.
#
# LGPL-2.1-or-later only: no --enable-gpl, no --enable-nonfree, and the script stops if the configuration says
# otherwise. Only the XMA decoders are built. The libraries stay separate files in the APK, so a user can replace them
# (NOTICE « FFmpeg »). OUT gets include/ and jniLibs/arm64-v8a/; XBA_FFMPEG_DIR=OUT is what the build reads.
set -eu
VERSION=7.0.2
SHA256=8646515b638a3ad303e23af6a3587734447cb8fc0a0c064ecdb8e95c4fd8b389
API=30
out=$1
jobs=${2:-4}
: "${ANDROID_NDK_HOME:?ANDROID_NDK_HOME is not set}"
bin=$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/bin
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
tarball=${FFMPEG_TARBALL:-$work/ffmpeg-$VERSION.tar.xz}
[ -f "$tarball" ] || curl -fsSL -o "$tarball" "https://ffmpeg.org/releases/ffmpeg-$VERSION.tar.xz"
echo "$SHA256  $tarball" | sha256sum -c -
tar -xJf "$tarball" -C "$work"
cd "$work/ffmpeg-$VERSION"
./configure --prefix="$work/inst" --target-os=android --arch=aarch64 --enable-cross-compile \
    --cc="$bin/aarch64-linux-android$API-clang" --cxx="$bin/aarch64-linux-android$API-clang++" \
    --ar="$bin/llvm-ar" --nm="$bin/llvm-nm" --ranlib="$bin/llvm-ranlib" --strip="$bin/llvm-strip" \
    --enable-shared --disable-static --enable-pic \
    --disable-everything --enable-decoder=xma1 --enable-decoder=xma2 \
    --disable-autodetect --disable-programs --disable-doc --disable-network \
    --disable-avdevice --disable-avformat --disable-avfilter --disable-swresample --disable-swscale \
    --disable-debug >"$work/configure.log" 2>&1 || { tail -n 30 "$work/configure.log"; exit 1; }
grep -q '^CONFIG_GPL=yes\|^CONFIG_NONFREE=yes' ffbuild/config.mak && { echo "not an LGPL build" >&2; exit 1; }
grep -q '^!CONFIG_GPL=yes' ffbuild/config.mak
make -j"$jobs" >"$work/make.log" 2>&1 || { tail -n 30 "$work/make.log"; exit 1; }
make install >>"$work/make.log" 2>&1
rm -rf "$out"
mkdir -p "$out/jniLibs/arm64-v8a"
cp -R "$work/inst/include" "$out/include"
for lib in avcodec avutil; do
    cp -L "$work/inst/lib/lib$lib.so" "$out/jniLibs/arm64-v8a/lib$lib.so"
    "$bin/llvm-strip" --strip-unneeded "$out/jniLibs/arm64-v8a/lib$lib.so"
    "$bin/llvm-readelf" -d "$out/jniLibs/arm64-v8a/lib$lib.so" | grep -q "SONAME.*\[lib$lib.so\]"
done
ls -l "$out/jniLibs/arm64-v8a"
