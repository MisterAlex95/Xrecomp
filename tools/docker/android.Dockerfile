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
