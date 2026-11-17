#!/bin/sh
# GameCube sound checks (XBA-263), called by ctest:
#
#   run_audio.sh unit GC_AUDIO_TEST OUTDIR
#       gc_audio_test (no toolchain, no game data): unit checks, then the AX voices and the disc stream through the
#       machine's registers, their WAVs compared with audio_check.py's references
#   run_audio.sh asnd GC_RUN ASND.dol OUTDIR
#       our libasnd homebrew under gc_run, interpreted: its 48 kHz output compared scene by scene (77 without it)
#   run_audio.sh play GC_PLAY ASND.dol OUTDIR
#       the same through gc_runtime (headless, SDL's dummy audio driver): what it queued for the host compared
set -u
mode=$1
here=$(cd "$(dirname "$0")" && pwd)
py=${PYTHON:-python3}

case "$mode" in
unit)
    test=$2 out=$3
    mkdir -p "$out"
    timeout 300 "$test" "$out" || exit 1
    "$py" "$here/audio_check.py" "$out/ax.wav" ax --ref "$out/ax_ref.wav" || exit 1
    "$py" "$here/audio_check.py" "$out/dtk.wav" dtk --ref "$out/dtk_ref.wav" || exit 1
    ;;
asnd)
    run=$2 dol=$3 out=$4
    [ -f "$dol" ] || { echo "skip: $dol not built (tools/consoles/build_homebrew.sh gc)"; exit 77; }
    mkdir -p "$out"
    timeout 300 "$run" "$dol" --frames 420 --wav "$out/asnd.wav" > "$out/asnd.log" 2>&1
    rc=$?
    cat "$out/asnd.log"
    [ $rc -eq 0 ] || exit 1
    grep -q "ucode asnd" "$out/asnd.log" || { echo "FAIL: the ASND ucode was not recognised"; exit 1; }
    "$py" "$here/audio_check.py" "$out/asnd.wav" asnd --ref "$out/asnd_ref.wav" || exit 1
    ;;
play)
    play=$2 dol=$3 out=$4
    [ -f "$dol" ] || { echo "skip: $dol not built (tools/consoles/build_homebrew.sh gc)"; exit 77; }
    mkdir -p "$out"
    SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy timeout 300 "$play" "$dol" --frames 420 --unpaced \
        --wav "$out/play_asnd.wav" > "$out/play.log" 2>&1
    rc=$?
    cat "$out/play.log"
    [ $rc -eq 0 ] || exit 1
    grep -q "gc: audio: " "$out/play.log" || { echo "FAIL: no audio line"; exit 1; }
    "$py" "$here/audio_check.py" "$out/play_asnd.wav" asnd || exit 1
    ;;
*)
    echo "usage: run_audio.sh unit|asnd|play ..." >&2
    exit 2
    ;;
esac
