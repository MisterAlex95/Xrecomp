#!/usr/bin/env bash
# A title's own APK (docs/android/app-shell.md « A title's own APK »): the app with the game's name, its own
# package and the game's title image as its icon, locked to that disc (WRONG_GAME for another default.xbe).
#
#   tools/android/title_apk.sh SOURCE [--title-dir DIR] [--task TASK] [--work DIR] [--dry-run] [-- GRADLE_ARGS...]
#
# SOURCE      the game as tools.prepare takes it: a disc image, a folder holding default.xbe, or default.xbe.
# --title-dir the title's project (templates/new-game with its generated C) to link into libmain.so; without it the
#             APK runs the title from the pack its on-device Prepare builds (pass -PxbaToolchainDir=... after --).
# --task      the Gradle task (default assembleRelease).
# --work      where the extracted default.xbe and the icon resources go (default ~/.cache/xrecomp/title-apk/TITLEID):
#             they come from your game, so never inside the repository.
# --dry-run   print the Gradle command instead of running it.
#
# Everything after -- goes to Gradle (-PxbaVersionCode=..., -PxbaToolchainDir=..., -PxbaLsfg=1, ...). Runs Gradle
# from android/ with the environment Gradle needs (SDL2_SRC, the SDK: tools/docker/android.Dockerfile has both).
# Needs python3 (no extra module) and sha256sum. The APK is built from your own disc: keep it for yourself.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
PY=${PYTHON:-python3}
GRADLE=${GRADLE:-gradle}
SOURCE=
TITLE_DIR=
TASK=assembleRelease
WORK=
DRY=0
while [ $# -gt 0 ]; do
    case "$1" in
        --title-dir) TITLE_DIR=${2:?--title-dir DIR}; shift 2 ;;
        --task) TASK=${2:?--task TASK}; shift 2 ;;
        --work) WORK=${2:?--work DIR}; shift 2 ;;
        --dry-run) DRY=1; shift ;;
        --) shift; break ;;
        -h|--help) sed -n '2,19p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        -*) echo "title_apk.sh: unknown option $1" >&2; exit 2 ;;
        *) [ -z "$SOURCE" ] || { echo "title_apk.sh: one SOURCE only" >&2; exit 2; }; SOURCE=$1; shift ;;
    esac
done
[ -n "$SOURCE" ] || { echo "usage: title_apk.sh SOURCE [--title-dir DIR] [--task TASK] [-- GRADLE_ARGS...]" >&2; exit 2; }
if [ -n "$TITLE_DIR" ]; then
    [ -f "$TITLE_DIR/CMakeLists.txt" ] || { echo "title_apk.sh: $TITLE_DIR has no CMakeLists.txt" >&2; exit 2; }
    TITLE_DIR=$(cd "$TITLE_DIR" && pwd)
fi

# The title ID and name from the certificate; default.xbe extracted (a disc image) or copied, to hash it.
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
if ! (cd "$ROOT" && "$PY" -m tools.prepare.identify "$SOURCE" --json --xbe-out "$TMP/default.xbe") > "$TMP/id.json"; then
    echo "title_apk.sh: $SOURCE: $(cat "$TMP/id.json")" >&2
    exit 1
fi
field() { "$PY" -c 'import json,sys; print(json.load(open(sys.argv[1]))[sys.argv[2]])' "$1" "$2"; }
TID=$(field "$TMP/id.json" title_id)
NAME=$(field "$TMP/id.json" name)
WORK=${WORK:-${XDG_CACHE_HOME:-$HOME/.cache}/xrecomp/title-apk/$TID}
case "$(cd "$(dirname "$WORK")" 2>/dev/null && pwd)/" in
    "$ROOT"/*) echo "title_apk.sh: --work $WORK is inside the repository; the icon comes from your game" >&2; exit 2 ;;
esac
mkdir -p "$WORK"
WORK=$(cd "$WORK" && pwd)
mv "$TMP/default.xbe" "$WORK/default.xbe"
SHA=$(sha256sum "$WORK/default.xbe" | cut -d' ' -f1)
rm -rf "$WORK/icon-res"
(cd "$ROOT" && "$PY" -m tools.title_art "$WORK/default.xbe" --android-res "$WORK/icon-res" --json) > "$TMP/art.json"
ICON=$(field "$TMP/art.json" icon)
# A package segment starts with a letter: t + the title ID in lower case.
APP_ID=fr.adserver.xboxrecomp.t$(echo "$TID" | tr 'A-F' 'a-f')

echo "title $TID « $NAME »: package $APP_ID, icon from the $( [ "$ICON" = image ] && echo "title image" || echo "title colour (no title image)")"
CMD=("$GRADLE" "$TASK" "-PxbaApplicationId=$APP_ID" "-PxbaLabel=$NAME" "-PxbaXbeSha256=$SHA"
     "-PxbaIconRes=$WORK/icon-res")
[ -n "$TITLE_DIR" ] && CMD+=("-PxbaTitleDir=$TITLE_DIR")
CMD+=("$@")
if [ "$DRY" = 1 ]; then
    printf '(cd %q &&' "$ROOT/android"; printf ' %q' "${CMD[@]}"; printf ')\n'
    exit 0
fi
cd "$ROOT/android"
"${CMD[@]}"
echo "APK: $(ls -t "$ROOT"/android/app/build/outputs/apk/*/*.apk 2>/dev/null | head -1)"
