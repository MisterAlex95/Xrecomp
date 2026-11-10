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

# The app shell APK (XBA-18), debug-signed. Out of the image with:
#   docker build -f tools/docker/android.Dockerfile --target apk-out --output type=local,dest=out .
FROM android AS apk
ARG XBA_VERSION=dev
COPY . /src
WORKDIR /src/android
RUN printf 'sdk.dir=%s\ncmake.dir=/usr\n' "$ANDROID_HOME" > local.properties \
    && gradle --no-daemon --console=plain -PxbaVersion="$XBA_VERSION" assembleDebug \
    && mkdir -p /out \
    && cp app/build/outputs/apk/debug/app-debug.apk /out/xboxrecomp-shell.apk \
    && unzip -l /out/xboxrecomp-shell.apk | grep -E 'lib/|classes' | tee /out/apk-contents.txt \
    && rm -rf /root/.gradle/caches /root/.gradle/daemon app/build app/.cxx build .gradle

FROM scratch AS apk-out
COPY --from=apk /out/ /

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
         -PxbaApplicationId=fr.adserver.xboxrecomp.stub -PxbaLabel="xboxrecomp stub" assembleDebug \
    && mkdir -p /out \
    && cp app/build/outputs/apk/debug/app-debug.apk /out/xboxrecomp-stub.apk \
    && cp /work/stub/game/default.xbe /out/stub-default.xbe \
    && unzip -l /out/xboxrecomp-stub.apk | grep -E 'lib/|classes' | tee /out/stub-apk-contents.txt \
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
