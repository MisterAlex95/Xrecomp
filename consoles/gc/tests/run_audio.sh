#!/bin/sh
# GameCube sound checks (XBA-263, XBA-266), called by ctest:
#
#   run_audio.sh unit GC_AUDIO_TEST OUTDIR
#       gc_audio_test (no toolchain, no game data): unit checks, then the AX voices and the disc stream through the
#       machine's registers, their WAVs compared with audio_check.py's references
#   run_audio.sh zelda GC_ZELDA_TEST OUTDIR
#       gc_zelda_test (no toolchain, no game data): the Zelda ucode's protocols and voices, AESND's mails, then
#       zelda.wav against audio_check.py's reference
#   run_audio.sh asnd|aesnd GC_RUN DOL OUTDIR
#       our libasnd / libaesnd homebrew under gc_run, interpreted: its 48 kHz output compared scene by scene (77
#       without it)
#   run_audio.sh play GC_PLAY DOL OUTDIR [asnd|aesnd]
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
zelda)
    test=$2 out=$3
    mkdir -p "$out"
    timeout 300 "$test" "$out" || exit 1
    "$py" "$here/audio_check.py" "$out/zelda.wav" zelda --ref "$out/zelda_ref.wav" || exit 1
    ;;
asnd | aesnd)
    run=$2 dol=$3 out=$4
    [ -f "$dol" ] || { echo "skip: $dol not built (tools/consoles/build_homebrew.sh gc)"; exit 77; }
    mkdir -p "$out"
    timeout 300 "$run" "$dol" --frames 420 --wav "$out/$mode.wav" > "$out/$mode.log" 2>&1
    rc=$?
    cat "$out/$mode.log"
    [ $rc -eq 0 ] || exit 1
    grep -q "ucode $mode" "$out/$mode.log" || { echo "FAIL: the $mode ucode was not recognised"; exit 1; }
    "$py" "$here/audio_check.py" "$out/$mode.wav" "$mode" --ref "$out/${mode}_ref.wav" || exit 1
    ;;
play)
    play=$2 dol=$3 out=$4 spec=${5:-asnd}
    [ -f "$dol" ] || { echo "skip: $dol not built (tools/consoles/build_homebrew.sh gc)"; exit 77; }
    mkdir -p "$out"
    SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy timeout 300 "$play" "$dol" --frames 420 --unpaced \
        --wav "$out/play_$spec.wav" > "$out/play.log" 2>&1
    rc=$?
    cat "$out/play.log"
    [ $rc -eq 0 ] || exit 1
    grep -q "gc: audio: " "$out/play.log" || { echo "FAIL: no audio line"; exit 1; }
    "$py" "$here/audio_check.py" "$out/play_$spec.wav" "$spec" || exit 1
    ;;
*)
    echo "usage: run_audio.sh unit|zelda|asnd|aesnd|play ..." >&2
    exit 2
    ;;
esac
