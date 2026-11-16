#!/usr/bin/env bash
# Profiles the running XRecomp game on the Android device connected to adb (XBA-36): simpleperf from the NDK, a flame
# graph with the lifted functions named after their guest addresses.
#
#   tools/android/profile.sh SECONDS [--serial S] [--package P] [--out DIR] [--freq HZ] [--dwarf] [--ndk DIR]
#                            [--lib DIR]... [--no-report]
#
# Needs adb, python3 and an NDK (ANDROID_NDK_HOME, or --ndk); the app must be debuggable (xrecomp-latest, a debug
# build) or the device rooted. Everything is in tools/android/profile_device.py; docs/android/profiling.md.
set -euo pipefail
exec "${PYTHON:-python3}" "$(cd "$(dirname "$0")" && pwd)/profile_device.py" "$@"
