#!/usr/bin/env bash
# The launcher's five UI sounds (move, select, back, error, done), made with soundcraft-cli (the storytold Craft suite,
# a dev-time asset tool: never needed to build the APK). Our own recipes, generated from synth parameters: no samples.
# Output: android/app/src/main/res/raw/ui_<name>.wav, 16-bit, 22.05 kHz, peak-normalised (UiFeedback plays each one
# through SoundPool at its own, lower volume).
#   tools/android/ui_sounds.sh            # regenerate the five WAVs
# soundcraft-cli is pre-alpha: check `soundcraft-cli describe <command>` if a line fails after an update.
set -euo pipefail
here="$(cd "$(dirname "$0")/../.." && pwd)"
out="$here/android/app/src/main/res/raw"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
mkdir -p "$out"

# synth NAME SECONDS WAVE1 WAVE2 SEMIS2 CUTOFF DECAY_MS LEVEL_DB REVERB_MIX NOTE... (a note: pitch:start:length:velocity,
# in ticks)
synth() {
    local name="$1" secs="$2" w1="$3" w2="$4" semis="$5" cutoff="$6" decay="$7" level="$8" rev="$9"
    shift 9
    {
        echo "track.new {\"kind\":\"instrument\",\"instrument\":\"subtractive_synth\",\"name\":\"UI\"}"
        echo "mix.instrument_param {\"track\":\"UI\",\"param\":\"osc1_wave\",\"value\":$w1}"
        echo "mix.instrument_param {\"track\":\"UI\",\"param\":\"osc2_wave\",\"value\":$w2}"
        echo "mix.instrument_param {\"track\":\"UI\",\"param\":\"osc2_semitones\",\"value\":$semis}"
        echo "mix.instrument_param {\"track\":\"UI\",\"param\":\"cutoff\",\"value\":$cutoff}"
        echo "mix.instrument_param {\"track\":\"UI\",\"param\":\"resonance\",\"value\":8}"
        echo "mix.instrument_param {\"track\":\"UI\",\"param\":\"amp_attack\",\"value\":1}"
        echo "mix.instrument_param {\"track\":\"UI\",\"param\":\"amp_decay\",\"value\":$decay}"
        echo "mix.instrument_param {\"track\":\"UI\",\"param\":\"amp_sustain\",\"value\":0}"
        echo "mix.instrument_param {\"track\":\"UI\",\"param\":\"amp_release\",\"value\":30}"
        echo "mix.instrument_param {\"track\":\"UI\",\"param\":\"level\",\"value\":$level}"
        echo "midi.new_clip {\"track\":\"UI\",\"start\":0,\"length\":24000}"
        local n p s l v
        for n in "$@"; do
            IFS=: read -r p s l v <<<"$n"
            echo "midi.note_add {\"clip\":2,\"pitch\":$p,\"start_ticks\":$s,\"length_ticks\":$l,\"velocity\":$v}"
        done
        if [ "$rev" != 0 ]; then
            echo "mix.insert {\"track\":\"UI\",\"slot\":0,\"plugin\":\"room_reverb\",\"params\":{\"mix\":$rev,\"decay\":0.4,\"size\":20}}"
        fi
    } > "$work/$name.sc"
    soundcraft-cli script "$work/$name.sc" --bounce "$work/$name.wav" --start 0 --end "$secs" >/dev/null
    soundcraft-cli convert "$work/$name.wav" "$out/ui_$name.wav" --bit-depth 16 --rate 22050 --normalize >/dev/null
    echo "ui_$name.wav  $(stat -c%s "$out/ui_$name.wav") bytes"
}

# Waves: 0 sine, 1 triangle, 2 saw, 3 square (subtractive_synth's osc*_wave).
synth move   0.07 1 0 12  3000  25 -4  0 91:0:16:70                                        # a soft high tick
synth select 0.20 3 2 12  2500  60  0 10 79:0:40:90 86:50:60:85                            # two quick notes going up
synth back   0.20 1 0 12  2000  60 -2  8 86:0:40:80 79:50:60:75                            # the same, going down
synth error  0.22 2 3 -12  900  70  0  0 50:0:50:100 50:90:70:100                          # two low buzzes
synth done   0.60 1 3 12  3200 160  0 18 72:0:60:85 76:70:60:85 79:140:60:85 84:210:240:90 # a rising chord
