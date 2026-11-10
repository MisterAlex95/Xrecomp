#!/usr/bin/env bash
# CPython for arm64-v8a, API 30, through its official Android build (Android/android.py), for the on-device
# Prepare:
#   jniLibs  libpython3.13.so, libxbapython.so (xbapython.c: the interpreter executable around Py_BytesMain)
#   assets   prepare/python-stdlib.zip: lib/python3.13/ (the standard library without what the pipeline never uses,
#            lib-dynload/, site-packages/capstone): the app extracts it to files/python-<sha12>, its PYTHONHOME
# Runs after build_capstone.sh (it takes the capstone bindings from its work directory).
#   tools/android/toolchain/build_python.sh        (env: see common.sh)
. "$(dirname "$0")/common.sh"

PYV=${PYTHON_VERSION%.*}                    # 3.13
src=$XBA_TC_WORK/python-src
cross=$XBA_TC_WORK/python-cross
[ -d "$XBA_TC_WORK/capstone-py/capstone" ] || { echo "run build_capstone.sh first" >&2; exit 1; }

log "CPython $PYTHON_VERSION for $XBA_TC_TRIPLE, API $XBA_TC_API"
unpack "$(fetch "$PYTHON_URL" "$PYTHON_SHA256")" "$src"
# android.py downloads its prebuilt dependencies into --cache-dir unless they are there: fetch them first, checked.
echo "$PYTHON_DEPS" | while read -r dep sum; do
    if [ -n "$dep" ]; then fetch "$PYTHON_DEPS_URL/$dep/$dep-$XBA_TC_TRIPLE.tar.gz" "$sum" >/dev/null; fi
done
# Our NDK (r28c, the image's) instead of the one android-env.sh pins (it would install that one through sdkmanager).
ndk_version=$(basename "$ANDROID_NDK_HOME")
sed -i "s/^ndk_version=.*/ndk_version=$ndk_version/" "$src/Android/android-env.sh"
grep -q "^ndk_version=$ndk_version\$" "$src/Android/android-env.sh"

export ANDROID_HOME=${ANDROID_HOME:?} ANDROID_API_LEVEL=$XBA_TC_API CROSS_BUILD_DIR=$cross
export PYTHON_CPU_COUNT=$JOBS             # android.py runs make -j os.cpu_count() (honoured by Python 3.13+)
rm -rf "$cross"
python3 "$src/Android/android.py" build build
# No ensurepip, no test modules: what the app ships. --with-pydebug off, the default -O3 release flags.
python3 "$src/Android/android.py" build "$XBA_TC_TRIPLE" --cache-dir "$XBA_TC_CACHE" -- --disable-test-modules
prefix=$cross/$XBA_TC_TRIPLE/prefix
ls "$prefix/lib/libpython$PYV.so" >/dev/null

log "jniLibs"
cc="$NDK_BIN/$XBA_TC_TRIPLE$XBA_TC_API-clang"
"$NDK_BIN/llvm-strip" --strip-unneeded -o "$JNI_DIR/libpython$PYV.so" "$prefix/lib/libpython$PYV.so"
# shellcheck disable=SC2016  # $ORIGIN is for the dynamic linker
"$cc" -O2 -I"$prefix/include/python$PYV" "$TC_DIR/xbapython.c" -o "$JNI_DIR/libxbapython.so" \
    -L"$prefix/lib" -lpython$PYV -Wl,-rpath,'$ORIGIN' -Wl,--build-id=sha1 -Wl,-z,max-page-size=16384 -s
check_jnilib "$JNI_DIR/libpython$PYV.so"
check_jnilib "$JNI_DIR/libxbapython.so"

log "standard library asset"
stage=$XBA_TC_WORK/python-stage
rm -rf "$stage" && mkdir -p "$stage/lib"
cp -a "$prefix/lib/python$PYV" "$stage/lib/"
lib=$stage/lib/python$PYV
# What the pipeline (tools/prepare and the steps it runs) never imports, and the build-time leftovers.
rm -rf "$lib"/{test,idlelib,tkinter,turtledemo,ensurepip,lib2to3,pydoc_data,venv,sqlite3,__phello__} \
       "$lib"/turtle.py "$lib"/config-"$PYV"-* "$lib"/site-packages/* "$lib"/_sysconfig_vars_*.json
find "$lib" -name __pycache__ -prune -exec rm -rf {} +
find "$lib" -name '*.exe' -delete
# Extension modules: stripped, and only those that need nothing beyond what an app process has (the OpenSSL and
# SQLite libraries are not shipped: hashlib falls back to its built-in SHA-2, the pipeline uses neither).
for so in "$lib"/lib-dynload/*.so; do
    if "$NDK_BIN/llvm-readelf" -d "$so" | grep -E 'NEEDED.*\[lib(ssl|crypto|sqlite3)_python\.so\]' >/dev/null; then
        echo "dropped $(basename "$so")"; rm "$so"; continue
    fi
    "$NDK_BIN/llvm-strip" --strip-unneeded "$so"
done
for m in _hashlib _ssl _sqlite3; do ! ls "$lib"/lib-dynload/$m.* >/dev/null 2>&1; done
cp -a "$XBA_TC_WORK/capstone-py/capstone" "$lib/site-packages/"
printf 'CPython %s for %s API %s (Android/android.py), capstone %s bindings\n' \
    "$PYTHON_VERSION" "$XBA_TC_TRIPLE" "$XBA_TC_API" "$CAPSTONE_VERSION" > "$stage/XBA_PYTHON_VERSION"
zip_dir "$stage" "$ASSET_DIR/python-stdlib.zip"
rm -rf "$src" "$cross" "$stage"
