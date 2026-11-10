#!/usr/bin/env bash
# libcapstone.so for arm64-v8a (X86 only, the one architecture the pipeline disassembles), as a jniLib, and its
# pure-Python ctypes bindings for the Python asset (build_python.sh puts them in site-packages; the app points
# LIBCAPSTONE_PATH at nativeLibraryDir).
#   tools/android/toolchain/build_capstone.sh        (env: see common.sh)
. "$(dirname "$0")/common.sh"

log "capstone $CAPSTONE_VERSION"
src=$XBA_TC_WORK/capstone-src
unpack "$(fetch "$CAPSTONE_URL" "$CAPSTONE_SHA256")" "$src"
cmake -S "$src" -B "$XBA_TC_WORK/capstone" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM="$XBA_TC_API" -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_SHARED_LIBS=ON -DCAPSTONE_ARCHITECTURE_DEFAULT=OFF -DCAPSTONE_X86_SUPPORT=ON \
    -DCAPSTONE_BUILD_TESTS=OFF -DCAPSTONE_BUILD_CSTOOL=OFF \
    -DCMAKE_PLATFORM_NO_VERSIONED_SONAME=ON \
    -DCMAKE_SHARED_LINKER_FLAGS="-Wl,-z,max-page-size=16384 -Wl,--build-id=sha1"
cmake --build "$XBA_TC_WORK/capstone" -j"$JOBS"
"$NDK_BIN/llvm-strip" --strip-unneeded -o "$JNI_DIR/libcapstone.so" "$XBA_TC_WORK/capstone/libcapstone.so"
check_jnilib "$JNI_DIR/libcapstone.so"

# The bindings: capstone/*.py only (no prebuilt library, no setup.py); they find the library through
# LIBCAPSTONE_PATH.
rm -rf "$XBA_TC_WORK/capstone-py" && mkdir -p "$XBA_TC_WORK/capstone-py/capstone"
cp "$src"/bindings/python/capstone/*.py "$XBA_TC_WORK/capstone-py/capstone/"
cp "$src/LICENSE.TXT" "$XBA_TC_WORK/capstone-py/capstone/LICENSE.TXT"
rm -rf "$src" "$XBA_TC_WORK/capstone"
