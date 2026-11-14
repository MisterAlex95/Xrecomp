# syntax=docker/dockerfile:1
# Android toolchain image (XBA-16): everything needed to build the toolkit for arm64-v8a and an APK.
#   docker build -f tools/docker/android.Dockerfile --target android -t xba-android .   # the toolchain image
#   docker build -f tools/docker/android.Dockerfile --target toolkit .                  # + the toolkit for arm64-v8a
# Pinned versions (also in docs/android/toolchain.md; change both together):
ARG DEBIAN=trixie-slim
FROM debian:${DEBIAN} AS android
ARG CLT_VERSION=13114758
ARG NDK_VERSION=28.2.13676358
ARG BUILD_TOOLS=35.0.0
ARG GRADLE_VERSION=8.10.2
ARG SDL2_TAG=release-2.32.10
ENV ANDROID_HOME=/opt/android-sdk \
    ANDROID_NDK_HOME=/opt/android-sdk/ndk/${NDK_VERSION} \
    ANDROID_NDK_ROOT=/opt/android-sdk/ndk/${NDK_VERSION} \
    JAVA_HOME=/usr/lib/jvm/java-17-openjdk-amd64 \
    GRADLE_HOME=/opt/gradle \
    SDL2_SRC=/opt/SDL2
ENV PATH=$GRADLE_HOME/bin:$ANDROID_HOME/cmdline-tools/latest/bin:$ANDROID_HOME/platform-tools:$PATH

# trixie has no JDK 17 package: Temurin 17 from Adoptium's apt repository.
RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates curl unzip git gnupg cmake ninja-build python3 file \
    && curl -fsSL https://packages.adoptium.net/artifactory/api/gpg/key/public | gpg --dearmor -o /usr/share/keyrings/adoptium.gpg \
    && echo "deb [signed-by=/usr/share/keyrings/adoptium.gpg] https://packages.adoptium.net/artifactory/deb trixie main" > /etc/apt/sources.list.d/adoptium.list \
    && apt-get update && apt-get install -y --no-install-recommends temurin-17-jdk \
    && ln -sfn /usr/lib/jvm/temurin-17-jdk-amd64 /usr/lib/jvm/java-17-openjdk-amd64 \
    && rm -rf /var/lib/apt/lists/*

# SDK + NDK through sdkmanager (it checks the archives' checksums).
RUN curl -fsSL -o /tmp/clt.zip "https://dl.google.com/android/repository/commandlinetools-linux-${CLT_VERSION}_latest.zip" \
    && mkdir -p "$ANDROID_HOME/cmdline-tools" && unzip -q /tmp/clt.zip -d "$ANDROID_HOME/cmdline-tools" \
    && mv "$ANDROID_HOME/cmdline-tools/cmdline-tools" "$ANDROID_HOME/cmdline-tools/latest" && rm /tmp/clt.zip \
    && yes | sdkmanager --licenses >/dev/null \
    && sdkmanager "platform-tools" "build-tools;${BUILD_TOOLS}" "platforms;android-34" "platforms;android-35" \
                  "ndk;${NDK_VERSION}" >/dev/null \
    && rm -rf "$ANDROID_HOME/emulator" /root/.android/cache

# Gradle, so builds do not download a wrapper distribution at run time.
RUN curl -fsSL -o /tmp/gradle.zip "https://services.gradle.org/distributions/gradle-${GRADLE_VERSION}-bin.zip" \
    && unzip -q /tmp/gradle.zip -d /opt && mv "/opt/gradle-${GRADLE_VERSION}" "$GRADLE_HOME" && rm /tmp/gradle.zip

# SDL2 sources at a pinned tag (android-project/ is the SDLActivity template the app shell starts from).
RUN git clone -q --depth 1 --branch "${SDL2_TAG}" https://github.com/libsdl-org/SDL.git "$SDL2_SRC" \
    && rm -rf "$SDL2_SRC/.git"

# A debug key made once per image, so successive debug APKs from the same image update the app in place.
RUN mkdir -p /root/.android && keytool -genkeypair -keystore /root/.android/debug.keystore -storepass android \
        -alias androiddebugkey -keypass android -keyalg RSA -keysize 2048 -validity 10000 \
        -dname "CN=Android Debug,O=Android,C=US" 2>/dev/null

# What is inside, for docs/android/toolchain.md and the CI log.
RUN { echo "NDK      $(grep Pkg.Revision $ANDROID_NDK_HOME/source.properties | cut -d= -f2 | tr -d ' ')"; \
      echo "clang    $($ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/bin/clang --version | head -1)"; \
      echo "SDK      $(ls $ANDROID_HOME/platforms | tr '\n' ' ')"; \
      echo "build    $(ls $ANDROID_HOME/build-tools | tr '\n' ' ')"; \
      echo "CMake    $(cmake --version | head -1)"; \
      echo "Gradle   $(gradle --version -q 2>/dev/null | grep '^Gradle' )"; \
      echo "JDK      $(java -version 2>&1 | head -1)"; \
      echo "SDL2     ${SDL2_TAG}"; } | tee /etc/xba-toolchain.txt
# The release keystore never goes in the image: it comes in as a build secret when an APK is signed.
WORKDIR /src

# The toolkit's static libraries for arm64-v8a, API 30 (XBA-17). Checks that no desktop dependency is linked.
FROM android AS toolkit
ARG JOBS=4
COPY . /src
RUN mkdir -p /reports \
    && cmake -S /src -B /work/android -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DXBOXRECOMP_BUILD_TESTS=OFF \
         -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake" \
         -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=30 \
    && cmake --build /work/android -j"$JOBS" \
    && ! grep -qE 'epoxy|X11|libssl|libcrypto' /work/android/build.ninja \
    && cd /work/android \
    && find . \( -name '*.a' -o -name '*.so' \) -not -path '*/CMakeFiles/*' | sort \
         | while read -r f; do printf '%-42s %9s  %s\n' "$f" "$(stat -c%s "$f")" "$(file -b "$f" | cut -c1-70)"; done \
         | tee /reports/android-artefacts.txt \
    && rm -rf /work

# OpenBIOS (MIT, pcsx-redux/nugget), the PlayStation BIOS the app ships (XBA-168, assets/ps1-bios/openbios.bin, its
# licence LICENSES/MIT-OpenBIOS.txt): tools/consoles/build_ps1_bios.sh's recipe, same pinned source, same build image
# (PCSX-Redux's CI image by digest: mipsel-none-elf GCC), same fixed path and SOURCE_DATE_EPOCH, so the same bytes,
# checked against the oracle-compatible build's sha256 (consoles/README.md « PS1 core »). It copies nothing of this
# repository, so BuildKit keeps it cached until these lines change; CI may also hand a cached copy over with
# --build-context xba-ps1-bios=DIR (DIR/openbios.bin). Never a Sony BIOS.
FROM ghcr.io/pcsx-redux/pcsx-redux-build:noble@sha256:20fece3d1e3b3c3e0a42bce9b986fb659b12bd441352df55abd862c381fb48f2 AS ps1-bios
ARG JOBS=4
ADD --checksum=sha256:43ffd423caefe3f07ea9713df453ee96c06cc467795e942ab0a89ef736249c0c \
    https://codeload.github.com/pcsx-redux/nugget/tar.gz/f044c6b863116cf851acb360ba09adaa54381bd7 /tmp/nugget.tgz
RUN set -e; mkdir -p /tmp/nugget /out; \
    tar xzf /tmp/nugget.tgz -C /tmp/nugget --strip-components=1; \
    cd /tmp/nugget; \
    SOURCE_DATE_EPOCH=0 make -C openbios -j"$JOBS" all >/tmp/build.log 2>&1 || { tail -n 40 /tmp/build.log; exit 1; }; \
    cp openbios/openbios.bin LICENSE /out/; \
    echo "68942dc22a85dd7f809d7a99043575096d95d656925dfcd3f056036e22d84085  /out/openbios.bin" | sha256sum -c -; \
    cd /; rm -rf /tmp/nugget /tmp/nugget.tgz /tmp/build.log

FROM scratch AS ps1-bios-out
COPY --from=ps1-bios /out/ /

# What the APK stages take (CI: --build-context xba-ps1-bios=DIR overrides it).
FROM ps1-bios-out AS xba-ps1-bios

# The app shell APK (XBA-18), debug-signed. Out of the image with:
#   docker build -f tools/docker/android.Dockerfile --target apk-out --output type=local,dest=out .
# Its own application id, fr.adserver.xrecomp.dev, and label « XRecomp shell » (XBA-134): it installs next to
# the XRecomp APK (fr.adserver.xrecomp.shell, xrecomp-latest) instead of over it (or failing on its other key).
FROM android AS apk
ARG XBA_VERSION=dev
COPY --from=xba-ps1-bios /openbios.bin /opt/xba-ps1-bios/openbios.bin
COPY . /src
WORKDIR /src/android
RUN printf 'sdk.dir=%s\ncmake.dir=/usr\n' "$ANDROID_HOME" > local.properties \
    && gradle --no-daemon --console=plain -PxbaVersion="$XBA_VERSION" -PxbaPs1Bios=/opt/xba-ps1-bios/openbios.bin \
         -PxbaApplicationId=fr.adserver.xrecomp.dev -PxbaLabel="XRecomp shell" assembleDebug \
    && mkdir -p /out \
    && cp app/build/outputs/apk/debug/app-debug.apk /out/xboxrecomp-shell.apk \
    && unzip -l /out/xboxrecomp-shell.apk | grep -E 'lib/|classes|assets/' | tee /out/apk-contents.txt \
    && grep -q 'assets/selftest.xbe' /out/apk-contents.txt \
    && grep -q 'assets/ps1-bios/openbios.bin' /out/apk-contents.txt \
    && unzip -p /out/xboxrecomp-shell.apk lib/arm64-v8a/libmain.so > /tmp/libmain.so \
    && "$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-nm" -D --defined-only /tmp/libmain.so \
         | grep -E ' (R xba_shell_abi|W recomp_lookup|W recomp_lookup_manual|W fe_menu_get_pb_state|V g_fe_cursor)$' \
         | tee -a /out/apk-contents.txt \
    && [ "$(grep -cE ' (R xba_shell_abi|W recomp_lookup|W recomp_lookup_manual|W fe_menu_get_pb_state|V g_fe_cursor)$' /out/apk-contents.txt)" = 5 ] \
    && rm /tmp/libmain.so \
    && rm -rf /root/.gradle/caches /root/.gradle/daemon app/build app/.cxx build .gradle

FROM scratch AS apk-out
COPY --from=apk /out/ /

# The app's JVM unit tests (Robolectric: the launcher's Prepare screens, PrepareService, the pack check, the tool
# install; docs/android/app-shell.md « Prepare on the device »). No NDK build. The first layer resolves Gradle's and
# Robolectric's downloads against the build files only (a stub manifest and one empty test), so it stays cached until
# they change; the second runs the real tests. Maven Central through Google's mirror (gradle-central-mirror.gradle).
# Screenshots of the launcher's states out of the image with:
#   docker build -f tools/docker/android.Dockerfile --target app-unit-tests-out --output type=local,dest=out .
FROM android AS app-unit-tests-deps
ARG CENTRAL_MIRROR=https://maven-central.storage-download.googleapis.com/maven2
COPY tools/docker/gradle-central-mirror.gradle /root/.gradle/init.d/central-mirror.gradle
COPY android/build.gradle android/settings.gradle android/gradle.properties /src/android/
COPY android/app/build.gradle /src/android/app/
COPY tools/conformance/test.xbe /src/tools/conformance/
WORKDIR /src/android
RUN printf 'sdk.dir=%s\ncmake.dir=/usr\n' "$ANDROID_HOME" > local.properties \
    && mkdir -p app/src/main app/src/test/java/w \
    && printf '<manifest/>\n' > app/src/main/AndroidManifest.xml \
    && printf 'package w;\n@org.junit.runner.RunWith(org.robolectric.RobolectricTestRunner.class)\n@org.robolectric.annotation.Config(sdk = 34)\npublic class W { @org.junit.Test public void t() {} }\n' \
         > app/src/test/java/w/W.java \
    && gradle --no-daemon --console=plain -ProbolectricRepoUrl="$CENTRAL_MIRROR" testDebugUnitTest \
         -x externalNativeBuildDebug \
    && rm -rf app/src app/build build .gradle /root/.gradle/daemon

FROM app-unit-tests-deps AS app-unit-tests
ARG CENTRAL_MIRROR=https://maven-central.storage-download.googleapis.com/maven2
COPY . /src
WORKDIR /src/android
RUN printf 'sdk.dir=%s\ncmake.dir=/usr\n' "$ANDROID_HOME" > local.properties \
    && gradle --no-daemon --console=plain -ProbolectricRepoUrl="$CENTRAL_MIRROR" testDebugUnitTest \
         -x externalNativeBuildDebug \
    && mkdir -p /out \
    && cp -r app/build/screenshots /out/ \
    && cp -r app/build/test-results/testDebugUnitTest /out/test-results \
    && grep -ho 'tests="[0-9]*" skipped="[0-9]*" failures="[0-9]*" errors="[0-9]*"' /out/test-results/*.xml \
         | awk -F'"' '{t+=$2; s+=$4; f+=$6; e+=$8} END {printf "app unit tests: %d run, %d skipped, %d failed, %d errors\n", t, s, f, e}' \
         | tee /out/app-unit-tests.txt \
    && rm -rf app/build app/.cxx build .gradle /root/.gradle/daemon

FROM scratch AS app-unit-tests-out
COPY --from=app-unit-tests /out/ /

# The stub title APK (XBA-52): templates/new-game around the synthetic XBE of tools/conformance (ours, no game
# content), lifted by the pipeline and linked into libmain.so through -PxbaTitleDir. The first end-to-end link of
# generated C for Android. Out of the image with:
#   docker build -f tools/docker/android.Dockerfile --target stub-apk-out --output type=local,dest=out .
FROM android AS stub-apk
ARG XBA_VERSION=dev
RUN apt-get update && apt-get install -y --no-install-recommends python3-capstone && rm -rf /var/lib/apt/lists/*
COPY . /src
WORKDIR /src/android
RUN /src/tools/android/stub_title.sh /work/stub \
    && printf 'sdk.dir=%s\ncmake.dir=/usr\n' "$ANDROID_HOME" > local.properties \
    && gradle --no-daemon --console=plain -PxbaVersion="$XBA_VERSION" -PxbaTitleDir=/work/stub \
         -PxbaApplicationId=fr.adserver.xrecomp.stub -PxbaLabel="XRecomp stub" assembleDebug \
    && mkdir -p /out \
    && cp app/build/outputs/apk/debug/app-debug.apk /out/xboxrecomp-stub.apk \
    && cp /work/stub/game/default.xbe /out/stub-default.xbe \
    && unzip -l /out/xboxrecomp-stub.apk | grep -E 'lib/|classes|assets/' | tee /out/stub-apk-contents.txt \
    && unzip -p /out/xboxrecomp-stub.apk lib/arm64-v8a/libmain.so > /tmp/libmain.so \
    && "$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-nm" -D --defined-only /tmp/libmain.so \
         | grep -E ' T (recomp_title_main|xbe_entry_point|sub_00011000)$' | tee -a /out/stub-apk-contents.txt \
    && [ "$(grep -cE ' T (recomp_title_main|xbe_entry_point|sub_00011000)$' /out/stub-apk-contents.txt)" = 3 ] \
    && rm /tmp/libmain.so \
    && rm -rf /root/.gradle/caches /root/.gradle/daemon app/build app/.cxx build .gradle /work

FROM scratch AS stub-apk-out
COPY --from=stub-apk /out/ /

# The C test projects as Android executables, in a bundle that runs on a device over adb (XBA-19).
#   docker build -f tools/docker/android.Dockerfile --target device-tests-out --output type=local,dest=out .
#   tools/android/run_tests.sh out/xba-device-tests.tar.gz
FROM android AS device-tests
ARG JOBS=4
COPY . /src
RUN mkdir -p /out /work/tests \
    && A="-DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake -DANDROID_ABI=arm64-v8a \
          -DANDROID_PLATFORM=30 -DCMAKE_BUILD_TYPE=RelWithDebInfo -DXBOXRECOMP_SDL2_SOURCE_DIR=$SDL2_SRC" \
    && { echo "| Project | Built for Android |"; echo "|---|---|"; } > /out/build-report.md \
    && for d in /src/tests/*/; do \
         n=$(basename "$d"); [ -f "$d/CMakeLists.txt" ] || continue; \
         if ! cmake -S "$d" -B "/work/tests/$n" -G Ninja $A > "/work/$n.log" 2>&1; then \
           why=$(grep -m1 -A3 'CMake Error' "/work/$n.log" | sed -n '2,4p' | tr -s ' \n' ' ' | cut -c1-110); \
           echo "| $n | no: configure ($why) |" >> /out/build-report.md; rm -rf "/work/tests/$n"; continue; fi; \
         if ! cmake --build "/work/tests/$n" -j"$JOBS" >> "/work/$n.log" 2>&1; then \
           why=$(grep -m1 -E 'error:|fatal error' "/work/$n.log" | sed 's#.*/src/##' | cut -c1-110); \
           echo "| $n | no: build ($why) |" >> /out/build-report.md; rm -rf "/work/tests/$n"; continue; fi; \
         echo "| $n | yes |" >> /out/build-report.md; \
       done \
    && python3 /src/tools/android/bundle_tests.py --build /work/tests --src /src --out /out/xba-device-tests \
    && cp /out/build-report.md /out/xba-device-tests/ \
    && tar -C /out -czf /out/xba-device-tests.tar.gz xba-device-tests \
    && rm -rf /work /out/xba-device-tests

FROM scratch AS device-tests-out
COPY --from=device-tests /out/ /

# The on-device toolchain of the embedded Prepare (phase 1, docs/android/toolchain.md): CPython + capstone and
# clang + lld for arm64-v8a, as jniLibs and assets in /opt/xba-toolchain, which the app build picks up with
# -PxbaToolchainDir. Versions and checksums are in tools/android/toolchain/versions.env. Only that directory is
# copied into the build stages, so a change elsewhere in the repository keeps them cached. Out of the image with:
#   docker build -f tools/docker/android.Dockerfile --target toolchain-out --output type=local,dest=out/toolchain .
FROM android AS toolchain-base
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential pkg-config zip xz-utils qemu-user e2fsprogs \
    && rm -rf /var/lib/apt/lists/*
ENV XBA_TC_OUT=/opt/xba-toolchain XBA_TC_WORK=/work/toolchain XBA_TC_CACHE=/work/cache

# LLVM in its own stage: the longest build (20 min at JOBS=6 on the CI host), cached while the scripts do not change.
FROM toolchain-base AS toolchain-llvm
ARG JOBS=6
COPY tools/android/toolchain/ /src/tools/android/toolchain/
RUN ulimit -c 0 && JOBS=$JOBS XBA_TC_KEEP=0 /src/tools/android/toolchain/build_llvm.sh && rm -rf /work

# BuildKit runs this stage beside toolchain-llvm: 3 jobs, so that both together stay within the host's budget.
FROM toolchain-base AS toolchain-python
ARG JOBS=3
COPY tools/android/toolchain/ /src/tools/android/toolchain/
RUN ulimit -c 0 && export JOBS=$JOBS \
    && /src/tools/android/toolchain/build_capstone.sh \
    && /src/tools/android/toolchain/build_python.sh \
    && rm -rf /work

# Both together, checked (check.sh): aarch64 ELFs that need only system libraries; under qemu-aarch64, clang compiles
# and links, and tools.prepare --android builds the stub title's pack with it against the real libmain.so.
FROM toolchain-base AS toolchain
COPY --from=toolchain-llvm /opt/xba-toolchain/ /opt/xba-toolchain/
COPY --from=toolchain-python /opt/xba-toolchain/ /opt/xba-toolchain/
RUN apt-get update && apt-get install -y --no-install-recommends python3-capstone && rm -rf /var/lib/apt/lists/*
COPY . /src
RUN ulimit -c 0 && (/src/tools/android/toolchain/check.sh /opt/xba-toolchain > /opt/xba-toolchain/check.txt 2>&1; \
        rc=$?; cat /opt/xba-toolchain/check.txt; exit $rc) \
    && rm -rf /work

FROM scratch AS toolchain-out
COPY --from=toolchain /opt/xba-toolchain/ /

# The shell APK with the toolchain inside (-PxbaToolchainDir), to see what it adds.
#   docker build -f tools/docker/android.Dockerfile --target prepare-apk-out --output type=local,dest=out .
FROM toolchain AS prepare-apk
ARG XBA_VERSION=dev
WORKDIR /src/android
RUN printf 'sdk.dir=%s\ncmake.dir=/usr\n' "$ANDROID_HOME" > local.properties \
    && gradle --no-daemon --console=plain -PxbaVersion="$XBA_VERSION" -PxbaToolchainDir=/opt/xba-toolchain \
         assembleDebug \
    && mkdir -p /out \
    && cp app/build/outputs/apk/debug/app-debug.apk /out/xboxrecomp-shell-prepare.apk \
    && unzip -lv /out/xboxrecomp-shell-prepare.apk | grep -E 'lib/|assets/' | tee /out/prepare-apk-contents.txt \
    && for f in lib/arm64-v8a/libxbacc.so lib/arm64-v8a/libxbapython.so lib/arm64-v8a/libpython3.13.so \
                lib/arm64-v8a/libcapstone.so assets/prepare/python-stdlib.zip assets/prepare/sysroot.zip \
                assets/prepare/toolkit.zip assets/prepare/toolkit.zip.sha256; do \
         grep -q " $f\$" /out/prepare-apk-contents.txt || { echo "missing $f"; exit 1; }; done \
    && rm -rf /root/.gradle/caches /root/.gradle/daemon app/build app/.cxx build .gradle

FROM scratch AS prepare-apk-out
COPY --from=prepare-apk /out/ /

# The XRecomp APK (XBA-125, docs/android/toolchain.md « The XRecomp APK in CI »): the release build of the app with
# the on-device toolchain (Prepare), optimised native code, debuggable (run-as) unless XBA_DEBUGGABLE=0, the version
# code from the commit time. CI publishes it as xrecomp-latest (debuggable) and release.yml as a release (not). The toolchain comes from the stage xba-toolchain below, which CI replaces
# with its cached toolchain-out (--build-context xba-toolchain=DIR); without that the toolchain is built here.
# Signing: the BuildKit secrets xba_keystore (the keystore file), xba_keystore_password and xba_key_alias (optional,
# default xrecomp). Without them the APK is signed with this image's debug key and the build says so; with
# XBA_SIGNING=release a missing release key fails the build. Never a key in the image or its layers.
#   docker build -f tools/docker/android.Dockerfile --target xrecomp-apk-out --build-context xba-toolchain=out/toolchain \
#     --secret id=xba_keystore,src=xrecomp-release.p12 --secret id=xba_keystore_password,env=XBA_KEYSTORE_PASSWORD \
#     --build-arg XBA_VERSION=$(git rev-parse --short=8 HEAD) --build-arg XBA_VERSION_CODE=$(( $(git log -1 --format=%ct) / 60 )) \
#     --output type=local,dest=out .
FROM toolchain-out AS xba-toolchain

FROM android AS xrecomp-apk
ARG XBA_VERSION=dev
ARG XBA_VERSION_CODE=1
# auto: the release key when its secrets are there, else the debug key; release: the release key or an error; debug.
ARG XBA_SIGNING=auto
# 1: debuggable (run-as, for the device check lists; xrecomp-latest). 0: a public release (release.yml).
ARG XBA_DEBUGGABLE=1
ARG JOBS=4
COPY --from=xba-toolchain / /opt/xba-toolchain/
COPY --from=xba-ps1-bios /openbios.bin /opt/xba-ps1-bios/openbios.bin
COPY . /src
WORKDIR /src/android
RUN --mount=type=secret,id=xba_keystore --mount=type=secret,id=xba_keystore_password \
    --mount=type=secret,id=xba_key_alias \
    set -e; \
    case "$XBA_SIGNING" in auto|release|debug) ;; *) echo "XBA_SIGNING=$XBA_SIGNING: auto, release or debug"; exit 1;; esac; \
    if [ -s /run/secrets/xba_keystore ] && [ "$XBA_SIGNING" != debug ]; then \
        export XBA_KEYSTORE=/run/secrets/xba_keystore; \
        XBA_KEYSTORE_PASSWORD=$(cat /run/secrets/xba_keystore_password 2>/dev/null || true); export XBA_KEYSTORE_PASSWORD; \
        if [ -s /run/secrets/xba_key_alias ]; then XBA_KEY_ALIAS=$(cat /run/secrets/xba_key_alias); export XBA_KEY_ALIAS; fi; \
    fi; \
    require=0; [ "$XBA_SIGNING" != release ] || require=1; \
    printf 'sdk.dir=%s\ncmake.dir=/usr\n' "$ANDROID_HOME" > local.properties; \
    gradle --no-daemon --console=plain -PxbaVersion="$XBA_VERSION" -PxbaVersionCode="$XBA_VERSION_CODE" \
        -PxbaToolchainDir=/opt/xba-toolchain -PxbaDebuggable="$XBA_DEBUGGABLE" -PxbaJobs="$JOBS" -PxbaRequireReleaseKey=$require \
        -PxbaPs1Bios=/opt/xba-ps1-bios/openbios.bin \
        assembleRelease; \
    mkdir -p /out; \
    cp app/build/outputs/apk/release/app-release.apk /out/xrecomp.apk; \
    rm -rf /root/.gradle/caches /root/.gradle/daemon app/build app/.cxx build .gradle local.properties
# What is in it, checked: the toolchain, the licences of what it ships (NOTICE « The Android app »), no XBE but our
# synthetic self-test, the version, debuggable, and who signed it (xrecomp-signing.txt: release or DEBUG).
RUN set -e; bt=$(ls -d "$ANDROID_HOME"/build-tools/* | tail -1); apk=/out/xrecomp.apk; \
    unzip -lv "$apk" | grep -E 'lib/|assets/' > /out/xrecomp-contents.txt; \
    for f in lib/arm64-v8a/libmain.so lib/arm64-v8a/libSDL2.so lib/arm64-v8a/libxbacc.so \
             lib/arm64-v8a/libxbapython.so lib/arm64-v8a/libpython3.13.so lib/arm64-v8a/libcapstone.so \
             assets/prepare/python-stdlib.zip assets/prepare/sysroot.zip assets/prepare/toolkit.zip \
             assets/prepare/toolkit.zip.sha256 assets/selftest.xbe \
             assets/licences/LICENSE.txt assets/licences/NOTICE.txt \
             assets/licences/Apache-2.0-WITH-LLVM-exception.txt assets/licences/PSF-2.0.txt \
             assets/licences/BSD-3-Clause-capstone.txt assets/licences/Apache-2.0-apksig.txt \
             assets/licences/MIT-OpenBIOS.txt assets/ps1-bios/openbios.bin; do \
        grep -q " $f\$" /out/xrecomp-contents.txt || { echo "xrecomp.apk: missing $f"; exit 1; }; done; \
    ! unzip -Z1 "$apk" | grep -iE '\.(xbe|iso|xiso)$' | grep -vx 'assets/selftest.xbe' \
        || { echo "xrecomp.apk: an XBE or disc image other than the self-test"; exit 1; }; \
    "$bt/aapt2" dump badging "$apk" | grep -E "^package:|application-debuggable" | tee /out/xrecomp-badging.txt; \
    grep -q "versionCode='$XBA_VERSION_CODE'" /out/xrecomp-badging.txt; \
    if [ "$XBA_DEBUGGABLE" = 1 ]; then grep -q application-debuggable /out/xrecomp-badging.txt; \
    else ! grep -q application-debuggable /out/xrecomp-badging.txt; fi; \
    "$bt/apksigner" verify --print-certs "$apk" | grep -E 'Signer #1 certificate (DN|SHA-256)' > /tmp/certs; \
    if grep -q 'CN=Android Debug' /tmp/certs; then kind=DEBUG; else kind=release; fi; \
    { echo "signed with: $kind key"; cat /tmp/certs; } | tee /out/xrecomp-signing.txt; \
    [ "$XBA_SIGNING" != release ] || [ "$kind" = release ] || { echo "XBA_SIGNING=release but debug-signed"; exit 1; }; \
    [ "$XBA_SIGNING" != debug ] || [ "$kind" = DEBUG ]; \
    ls -la "$apk"

FROM scratch AS xrecomp-apk-out
COPY --from=xrecomp-apk /out/ /
