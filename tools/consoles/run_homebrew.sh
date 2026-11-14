#!/usr/bin/env bash
# Runs the homebrew test corpus (XBA-149) headless in external emulators used as test oracles, and checks each
# program's 512-byte result block (consoles/<console>/tests/homebrew/README.md, « Result protocol »).
#
#   tools/consoles/run_homebrew.sh [ps1|n64|all] [CORPUS_DIR]       (default: all build/homebrew)
#
# CORPUS_DIR is what tools/consoles/build_homebrew.sh wrote (CORPUS_DIR/<console>/...). For each console the script
# builds the oracle image tools/docker/<console>-oracle.Dockerfile (tag xba-<console>-oracle; the layer cache makes a
# rebuild instant), then runs every program in a throwaway container of it, without network:
#   ps1  PCSX-Redux -no-ui with OpenBIOS (both built from source, no Sony BIOS), interpreter CPU; a Lua script
#        (oracle/ps1_watch.lua) reads the block at each VSync. Each program runs twice: the PS-X EXE side-loaded
#        (<prog>.exe) and the disc booted by OpenBIOS through SYSTEM.CNF (<prog>.cue, « <prog>/disc »).
#   n64  ares (N64 core only) under Xvfb, paraLLEl-RDP on lavapipe; oracle/gdb_poll.py reads the block through
#        ares's GDB server while the game runs.
# oracle/decode_block.py turns the block into PASS / FAIL / TIMEOUT / NOBLOCK. The emulators are oracles only:
# nothing of them is copied into or linked with our code, and their images are never published.
# Prints one RESULT line per run and a summary; exits 1 if any run did not PASS.
#
# Env: XBA_ORACLE_FRAMES (PS1 VSync budget per run, default 1200), XBA_ORACLE_SECONDS (N64 wall-clock budget per
# run, default 90), XBA_HOMEBREW_TIMEOUT (seconds per docker build, default 5400), JOBS (image builds, default 4),
# DOCKER_BUILD_ARGS (extra `docker build` flags).
set -euo pipefail

what=${1:-all}
corpus=${2:-build/homebrew}
frames=${XBA_ORACLE_FRAMES:-1200}
seconds=${XBA_ORACLE_SECONDS:-90}
tmo=${XBA_HOMEBREW_TIMEOUT:-5400}
jobs=${JOBS:-4}

repo=$(cd "$(dirname "$0")/../.." && pwd)
case "$what" in
ps1 | n64) consoles=("$what") ;;
all) consoles=(ps1 n64) ;;
*)
    echo "usage: $0 [ps1|n64|all] [CORPUS_DIR]" >&2
    exit 2
    ;;
esac
command -v docker >/dev/null || {
    echo "docker is required" >&2
    exit 1
}

cid=
cleanup() { [ -z "$cid" ] || docker rm -f "$cid" >/dev/null 2>&1 || true; }
trap cleanup EXIT

# Inside the containers: /run holds the program files and tools/consoles/oracle/*.
ps1_cmd='timeout $((LIMIT / 30 + 60)) pcsx-redux -no-ui -safe -no-gui-log -run -interpreter -fastboot \
      -bios /opt/pcsx-redux/share/pcsx-redux/resources/openbios.bin $LOAD \
      -dofile /run/ps1_watch.lua -exec "xba_watch($LIMIT)" >/run/out.txt 2>&1 || true
  python3 /run/decode_block.py --endian little --name "$NAME" --test-id "$TESTID" </run/out.txt \
    || { echo "  --- last lines of the PCSX-Redux log"; grep -v "^XBA-BLOCK" /run/out.txt | tail -n 15; exit 1; }'
n64_cmd='Xvfb :99 -screen 0 640x480x24 -nolisten tcp >/run/xvfb.txt 2>&1 &
  export DISPLAY=:99 HOME=/tmp
  sleep 1
  stdbuf -oL -eL ares --system "Nintendo 64" --no-file-prompt --settings-file /tmp/settings.bml \
    --setting Video/Driver=None --setting Developer/DebugServerEnabled=true \
    --setting Developer/DebugServerPort=9123 --setting Developer/DebugServerUseIPv4=true \
    "/run/$PROG" >/run/ares.txt 2>&1 &
  ares=$!
  timeout $((LIMIT + 30)) python3 /run/gdb_poll.py --port 9123 --addr 0xA0300000 --endian big \
    --timeout "$LIMIT" >/run/out.txt 2>&1 || true
  kill "$ares" 2>/dev/null || true
  python3 /run/decode_block.py --endian big --name "$NAME" --test-id "$TESTID" </run/out.txt \
    || { echo "  --- poller / ares output"; cat /run/out.txt /run/ares.txt | grep -v "^XBA-BLOCK" | tail -n 15; exit 1; }'

declare -A testid=([hello]=HELO [cpu]="CPU " [timer_irq]=TIRQ [gfx]="GFX " [overlay]="OVL " [iwram_copy]=IWRM [sound]="SND ")
results=()
failed=0

run_one() { # console name testid limit load-args-or-rom files...
    local c=$1 name=$2 tid=$3 limit=$4 load=$5
    shift 5
    local cmd_var="${c}_cmd" out rc
    cid=$(docker create --network none -e NAME="$name" -e TESTID="$tid" -e LIMIT="$limit" -e LOAD="$load" \
        -e PROG="$load" "xba-$c-oracle" bash -c "${!cmd_var}")
    for f in "$@" "$repo"/tools/consoles/oracle/*; do docker cp "$f" "$cid:/run/" >/dev/null; done
    rc=0
    out=$(timeout $((limit + 300)) docker start -a "$cid" 2>&1) || rc=$?
    docker rm -f "$cid" >/dev/null 2>&1 || true
    cid=
    printf '%s\n' "$out"
    local line
    line=$(printf '%s\n' "$out" | grep -m1 '^RESULT ' || echo "RESULT $name NOBLOCK (container exit $rc)")
    results+=("$c  ${line#RESULT }")
    case "$line" in "RESULT $name PASS"*) ;; *) failed=1 ;; esac
}

for c in "${consoles[@]}"; do
    dir="$corpus/$c"
    [ -d "$dir" ] || {
        echo "$dir is missing: run tools/consoles/build_homebrew.sh $c $corpus first" >&2
        exit 1
    }
    image="xba-$c-oracle"
    echo "== $c: oracle image $image"
    start=$(date +%s)
    # The image carries the sha256 of its Dockerfile (label xba.dockerfile): an image built from the same file is
    # reused as is, so a pruned build cache never forces a cold rebuild (16 min for PS1). XBA_ORACLE_REBUILD=1 forces it.
    dsum=$(sha256sum "$repo/tools/docker/$c-oracle.Dockerfile" | cut -d' ' -f1)
    have=$(docker image inspect -f '{{ index .Config.Labels "xba.dockerfile" }}' "$image" 2>/dev/null || true)
    if [ "$have" = "$dsum" ] && [ -z "${XBA_ORACLE_REBUILD:-}" ]; then
        echo "== $c: $image is up to date with its Dockerfile, not rebuilt"
    else
        # shellcheck disable=SC2086
        timeout "$tmo" docker build ${DOCKER_BUILD_ARGS:-} --build-arg JOBS="$jobs" --label "xba.dockerfile=$dsum" \
            -t "$image" - <"$repo/tools/docker/$c-oracle.Dockerfile"
    fi
    echo "== $c: image ready in $(($(date +%s) - start)) s"
    docker run --rm "$image" sh -c 'cat /opt/*/VERSION'

    case "$c" in
    ps1)
        for exe in "$dir"/*.exe; do
            p=$(basename "$exe" .exe)
            # overlay reads its overlays from the disc, so its EXE run gets the disc in the drive too
            load="-exe /run/$p.exe"
            [ "$p" != overlay ] || load="-iso /run/$p.cue $load"
            run_one ps1 "$p" "${testid[$p]:-}" "$frames" "$load" "$exe" "$dir/$p.cue" "$dir/$p.bin"
            run_one ps1 "$p/disc" "${testid[$p]:-}" "$frames" "-iso /run/$p.cue" "$dir/$p.cue" "$dir/$p.bin"
        done
        ;;
    n64)
        for rom in "$dir"/*.z64; do
            p=$(basename "$rom" .z64)
            run_one n64 "$p" "${testid[$p]:-}" "$seconds" "$p.z64" "$rom"
        done
        ;;
    esac
done

echo
echo "== summary"
printf '%s\n' "${results[@]}"
exit "$failed"
