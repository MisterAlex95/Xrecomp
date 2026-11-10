#!/usr/bin/env bash
# xboxrecomp -- prepare an Xbox game you own, on a Mac (double-click it, or run it from a terminal).
#
#   scripts/xboxrecomp.command [GAME]
#
# GAME is the disc image (.iso), the folder with the extracted disc, or its default.xbe; without it the script
# asks (drag the file or folder into the window). The first run installs what it needs: Homebrew packages (sdl2,
# libepoxy, cmake, ninja, python@3.12) and a Python environment with capstone in ~/.xboxrecomp. Then it runs
# tools.prepare: analysis, C lift, build, and the game data next to the result. Nothing is uploaded anywhere.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
HOME_DIR="${XBOXRECOMP_HOME:-$HOME/.xboxrecomp}"
VENV="$HOME_DIR/venv"

say() { printf '\n== %s ==\n' "$*"; }

say "xboxrecomp prepare"
if ! command -v brew >/dev/null 2>&1; then
    echo "Homebrew is needed (https://brew.sh). Install it, then run this again."
    exit 1
fi
missing=()
for f in sdl2 libepoxy cmake ninja python@3.12; do
    brew list --versions "$f" >/dev/null 2>&1 || missing+=("$f")
done
if [ ${#missing[@]} -gt 0 ]; then
    echo "To install with Homebrew: ${missing[*]}"
    read -r -p "Install now? [Y/n] " ok
    case "${ok:-y}" in [Nn]*) exit 1 ;; esac
    brew install "${missing[@]}"
fi
PY312="$(brew --prefix python@3.12)/bin/python3.12"
if [ ! -x "$VENV/bin/python" ]; then
    say "Python environment ($VENV)"
    mkdir -p "$HOME_DIR"
    "$PY312" -m venv "$VENV"
fi
"$VENV/bin/python" -c "import capstone" 2>/dev/null || "$VENV/bin/pip" install --quiet capstone

GAME="${1:-}"
if [ -z "$GAME" ]; then
    echo
    echo "Drag your game here (the .iso, or the folder of the extracted disc), then press Enter:"
    read -r GAME
    GAME="${GAME%\"}"; GAME="${GAME#\"}"; GAME="${GAME%\'}"; GAME="${GAME#\'}"
    GAME="$(printf '%s' "$GAME" | sed -e 's/[[:space:]]*$//' -e 's/\\ / /g')"
fi
if [ ! -e "$GAME" ]; then
    echo "Not found: $GAME"
    exit 1
fi

say "Preparing $(basename "$GAME")"
cd "$ROOT"
"$VENV/bin/python" -m tools.prepare "$GAME"
