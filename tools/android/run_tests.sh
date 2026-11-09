#!/usr/bin/env bash
# Runs the toolkit's C tests on the Android device connected to adb (XBA-19).
#
#   tools/android/run_tests.sh xba-device-tests.tar.gz [name-filter]
#
# The bundle comes from: docker build -f tools/docker/android.Dockerfile --target device-tests-out \
#                          --output type=local,dest=out .
# It is pushed to /data/local/tmp (no root needed), run there, and the report comes back as
# device-tests-<model>-<date>.md in the current directory, with the logs in device-tests-logs/.
set -euo pipefail

BUNDLE=${1:?usage: run_tests.sh xba-device-tests.tar.gz [name-filter]}
FILTER=${2:-}
DIR=/data/local/tmp

adb get-state >/dev/null || { echo "no device on adb" >&2; exit 2; }
model=$(adb shell getprop ro.product.model | tr -d '\r' | tr ' ' '_')
adb push "$BUNDLE" "$DIR/xba-device-tests.tar.gz" >/dev/null
adb shell "cd $DIR && rm -rf xba-device-tests && tar -xzf xba-device-tests.tar.gz && rm xba-device-tests.tar.gz"
set +e
adb shell "sh $DIR/xba-device-tests/run.sh '$FILTER'"
rc=$?
set -e
out="device-tests-${model}-$(date +%Y%m%d-%H%M).md"
adb pull "$DIR/xba-device-tests/report.md" "$out" >/dev/null
rm -rf device-tests-logs && adb pull "$DIR/xba-device-tests/logs" device-tests-logs >/dev/null
echo "report: $out (logs in device-tests-logs/)"
exit $rc
