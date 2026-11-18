#!/bin/sh
# run_hle_table.sh X360_HLE_TEST WORKDIR -- hle_coverage reads the runtime's sources; the test binary asks the real
# dispatch table (x360_hle_lookup, the variable exports): both lists must be the same (XBA-296).
set -e
bin=$1
work=$2
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../../.." && pwd)
command -v python3 >/dev/null || { echo "skipped: no python3"; exit 77; }
mkdir -p "$work"
${XBR_TEST_EMULATOR:+$(echo "$XBR_TEST_EMULATOR" | tr , ' ')} "$bin" --list-hle | LC_ALL=C sort >"$work/real.txt"
PYTHONPATH="$repo:$repo/consoles/x360/tools" python3 -m x360_tools.hle_coverage --implemented | LC_ALL=C sort >"$work/read.txt"
diff "$work/real.txt" "$work/read.txt"
echo "hle_coverage matches the dispatch table: $(wc -l <"$work/real.txt") exports"
