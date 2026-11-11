#!/usr/bin/env bash
# clang + lld (+ llvm-ar) for arm64-v8a as one multicall binary, for the on-device Prepare:
#   jniLibs  libxbacc.so: LLVM's llvm-driver (LLVM_TOOL_LLVM_DRIVER_BUILD), linked fully static against bionic
#            (-static, libc++ static), AArch64 target only, default target aarch64-linux-android30. Static, so it
#            needs nothing from the device and runs as is under qemu-aarch64 (check.sh).
#            Called as `libxbacc.so clang ...`, `libxbacc.so ld.lld ...`, or through symlinks named clang / ld.lld.
#   assets   prepare/sysroot.zip, the --sysroot of tools.prepare --android (the app extracts it to
#            files/sysroot-<sha12>): the NDK layout (usr/include: the generic and aarch64 headers; usr/lib/
#            aarch64-linux-android/30: crt objects and the libc, libm, libdl, liblog, libz, libandroid stubs), plus
#            lib/clang/<major>/: this clang's resource headers, the NDK's compiler-rt builtins and libunwind.a
#            (tools/prepare/compile.py passes -resource-dir and takes the builtins from there).
#   tools/android/toolchain/build_llvm.sh        (env: see common.sh; LLVM_JOBS overrides JOBS here)
. "$(dirname "$0")/common.sh"

J=${LLVM_JOBS:-$JOBS}
MAJOR=${LLVM_VERSION%%.*}
src=$XBA_TC_WORK/llvm-src
native=$XBA_TC_WORK/llvm-native
cross=$XBA_TC_WORK/llvm-cross
T0=$(date +%s)

log "LLVM $LLVM_VERSION sources"
if [ ! -f "$src/.unpacked-$LLVM_VERSION" ]; then
    rm -rf "$src" && mkdir -p "$src"
    tar -xf "$(fetch "$LLVM_URL" "$LLVM_SHA256")" -C "$src" --strip-components=1 \
        --exclude='*/test' --exclude='*/unittests' --exclude='llvm-project-*/lldb' --exclude='llvm-project-*/mlir' \
        --exclude='llvm-project-*/flang' --exclude='llvm-project-*/openmp' --exclude='llvm-project-*/polly' \
        --exclude='llvm-project-*/bolt' --exclude='llvm-project-*/libc' --exclude='llvm-project-*/clang-tools-extra'
    touch "$src/.unpacked-$LLVM_VERSION"
fi

COMMON=(-G Ninja -DCMAKE_BUILD_TYPE=Release -DLLVM_ENABLE_PROJECTS="clang;lld" -DLLVM_TARGETS_TO_BUILD=AArch64
    -DLLVM_INCLUDE_TESTS=OFF -DLLVM_INCLUDE_EXAMPLES=OFF -DLLVM_INCLUDE_BENCHMARKS=OFF -DLLVM_INCLUDE_DOCS=OFF
    -DCLANG_INCLUDE_TESTS=OFF -DCLANG_INCLUDE_DOCS=OFF -DLLVM_ENABLE_BINDINGS=OFF
    -DLLVM_ENABLE_ZLIB=OFF -DLLVM_ENABLE_ZSTD=OFF -DLLVM_ENABLE_LIBXML2=OFF -DLLVM_ENABLE_TERMINFO=OFF
    -DLLVM_ENABLE_LIBEDIT=OFF -DLLVM_ENABLE_LIBPFM=OFF -DLLVM_ENABLE_CURL=OFF -DLLVM_ENABLE_HTTPLIB=OFF
    -DCLANG_ENABLE_ARCMT=OFF -DCLANG_ENABLE_STATIC_ANALYZER=OFF -DCLANG_PLUGIN_SUPPORT=OFF
    -DLLVM_PARALLEL_LINK_JOBS=1)

log "host tablegen (native)"
cmake -S "$src/llvm" -B "$native" "${COMMON[@]}" -DLLVM_TARGETS_TO_BUILD=AArch64 >/dev/null
ninja -C "$native" -j"$J" llvm-tblgen clang-tblgen llvm-min-tblgen
T1=$(date +%s)

log "clang + lld for $XBA_TC_TRIPLE$XBA_TC_API (llvm-driver, static)"
cmake -S "$src/llvm" -B "$cross" "${COMMON[@]}" \
    -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM="$XBA_TC_API" -DANDROID_STL=c++_static \
    -DCMAKE_EXE_LINKER_FLAGS="-static -Wl,--gc-sections" \
    -DLLVM_HOST_TRIPLE="$XBA_TC_TRIPLE" -DLLVM_DEFAULT_TARGET_TRIPLE="$XBA_TC_TRIPLE$XBA_TC_API" \
    -DLLVM_NATIVE_TOOL_DIR="$native/bin" -DLLVM_TABLEGEN="$native/bin/llvm-tblgen" \
    -DCLANG_TABLEGEN="$native/bin/clang-tblgen" -DLLVM_USE_HOST_TOOLS=OFF \
    -DLLVM_TOOL_LLVM_DRIVER_BUILD=ON -DLLVM_DISTRIBUTION_COMPONENTS="clang;lld;llvm-ar" \
    -DLLVM_BUILD_LLVM_DYLIB=OFF -DLLVM_ENABLE_PIC=OFF -DLLVM_ENABLE_PLUGINS=OFF -DLLVM_BUILD_UTILS=OFF \
    -DCMAKE_C_FLAGS="-ffunction-sections -fdata-sections" -DCMAKE_CXX_FLAGS="-ffunction-sections -fdata-sections"
grep -q '"clang"' "$cross/tools/llvm-driver/LLVMDriverTools.def"
ninja -C "$cross" -j"$J" llvm-driver clang-resource-headers
T2=$(date +%s)
"$NDK_BIN/llvm-strip" -o "$JNI_DIR/libxbacc.so" "$cross/bin/llvm"
check_jnilib "$JNI_DIR/libxbacc.so"
echo "multicall tools: $(sed -n 's/LLVM_DRIVER_TOOL("\([^"]*\)".*/\1/p' "$cross/tools/llvm-driver/LLVMDriverTools.def" | tr '\n' ' ')"

log "sysroot + resource dir asset"
stage=$XBA_TC_WORK/cc-stage
rm -rf "$stage" && mkdir -p "$stage/usr/lib/$XBA_TC_TRIPLE/$XBA_TC_API" "$stage/lib/clang/$MAJOR/lib/linux/aarch64"
# Headers: the generic ones plus this target's asm/ directory (the NDK has one per ABI); no libc++.
mkdir -p "$stage/usr/include"
(cd "$NDK_SYSROOT/usr/include" && for e in *; do
    case $e in c++|aarch64-linux-android|arm-linux-androideabi|i686-linux-android|x86_64-linux-android|riscv64-linux-android) ;;
               *) cp -a "$e" "$stage/usr/include/" ;; esac; done)
cp -a "$NDK_SYSROOT/usr/include/$XBA_TC_TRIPLE" "$stage/usr/include/"
# API 30: crt objects and the stub libraries a title links against (the real ones are the device's).
for f in crtbegin_so.o crtend_so.o crtbegin_dynamic.o crtend_android.o \
         libc.so libm.so libdl.so liblog.so libz.so libandroid.so; do
    cp "$NDK_SYSROOT/usr/lib/$XBA_TC_TRIPLE/$XBA_TC_API/$f" "$stage/usr/lib/$XBA_TC_TRIPLE/$XBA_TC_API/"
done
# compiler-rt builtins and libunwind.a where the NDK keeps them (lib/linux/, lib/linux/aarch64/): the link takes
# libunwind.a next to the builtins.
ndk_rt=$(echo "$NDK_PREBUILT"/lib/clang/*/lib/linux)
cp "$ndk_rt/libclang_rt.builtins-aarch64-android.a" "$stage/lib/clang/$MAJOR/lib/linux/"
cp "$ndk_rt/aarch64/libunwind.a" "$stage/lib/clang/$MAJOR/lib/linux/aarch64/"
cp -a "$cross/lib/clang/$MAJOR/include" "$stage/lib/clang/$MAJOR/"
printf 'clang + lld %s (llvm-driver), sysroot: NDK %s %s API %s\n' "$LLVM_VERSION" "$(basename "$ANDROID_NDK_HOME")" \
    "$XBA_TC_TRIPLE" "$XBA_TC_API" > "$stage/XBA_CC_VERSION"
zip_dir "$stage" "$ASSET_DIR/sysroot.zip"
du -sh "$stage"
rm -rf "$stage"
echo "LLVM build times: tablegen $((T1 - T0)) s, cross $((T2 - T1)) s"
# The trees stay for a rebuild unless XBA_TC_KEEP=0 (the Docker stage removes them).
if [ "${XBA_TC_KEEP:-1}" = 0 ]; then rm -rf "$src" "$native" "$cross"; fi
