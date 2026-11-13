#!/usr/bin/env bash
#
# One-time setup so the conformance suite runs on macOS exactly as it does on
# Windows.
#
# By default it sets up the Python side, which is quick:
#
#   1. SDL2 and libepoxy via Homebrew, which the OpenGL video backend links
#      (found with pkg-config, so sdl2-compat counts as SDL2)
#   2. a virtualenv in .venv with the suite's Python dependencies, made with
#      the newest Python 3.10 or later found (Homebrew's first)
#   3. a `py` shell function, so Windows' `py -3` works here too
#
# That is enough for `py -3 -m pytest tools/`. The conformance suite also needs
# container images, which are large and slow to build, so they are opt-in:
#
#   --test    also build the three container images the conformance suite runs
#             against (~2.2 GB, ~10 min the first time). Needed for
#             `py -3 -m tools.conformance` in any of its phases.
#
set -euo pipefail

with_images=0
while [ $# -gt 0 ]; do
    case "$1" in
        --test)    with_images=1 ;;
        -h|--help) awk 'NR<3 {next} /^#/ {sub(/^# ?/, ""); print; next} {exit}' \
                       "$0"; exit 0 ;;
        *)         echo "unknown option: $1" >&2; exit 2 ;;
    esac
    shift
done

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT_DIR"

GCC_IMAGE=xboxrecomp-gcc-i386     # gcc; assembles and runs the snippets
MSVC_BUILD=xboxrecomp-msvc-amd64    # cl + link, 64-bit tools under Rosetta
MSVC_RUN=xboxrecomp-msvc-wine       # runs the 32-bit PEs the above builds

step() { echo; echo "== $* =="; }

# ---------------------------------------------------------------- prerequisites
step "Checking prerequisites"

# A Python 3.10 or later: macOS's /usr/bin/python3 is older, so Homebrew's
# versioned ones come first, newest first, then whatever is on PATH.
py_ok() { "$1" -c 'import sys; sys.exit(sys.version_info < (3, 10))' >/dev/null 2>&1; }
PYTHON=""
for v in 3.14 3.13 3.12 3.11 3.10; do
    for p in "/opt/homebrew/bin/python$v" "/usr/local/bin/python$v" "$(command -v "python$v" 2>/dev/null || true)"; do
        if [ -n "$p" ] && [ -x "$p" ] && py_ok "$p"; then PYTHON="$p"; break 2; fi
    done
done
if [ -z "$PYTHON" ] && command -v python3 >/dev/null 2>&1 && py_ok python3; then
    PYTHON="$(command -v python3)"
fi
if [ -z "$PYTHON" ]; then
    echo "Python 3.10 or later is required (brew install python@3.12) and re-run." >&2
    exit 1
fi
echo "python: $PYTHON ($("$PYTHON" --version 2>&1))"


# ----------------------------------------------------------------------- venv
step "Native libraries"

# SDL2 and libepoxy are what the OpenGL video backend links against. They are
# not needed for the test suite, so a missing Homebrew is a warning rather than
# a failure -- someone setting up only to run the tests should not be stopped by
# a dependency they will not reach.
#
# Each is checked with pkg-config, which is what CMake asks: Homebrew's SDL2
# today is sdl2-compat (SDL2's API over SDL3), which installs sdl2.pc but
# conflicts with the old sdl2 formula, so `brew list sdl2` would miss it and
# try to install the conflicting one.
if ! command -v brew >/dev/null 2>&1; then
    echo "Homebrew not found; skipping SDL2 and libepoxy."
    echo "The test suite does not need them, but the OpenGL video backend does."
    echo "Install Homebrew from https://brew.sh, or install them another way."
else
    BREW_PREFIX="$(brew --prefix)"
    export PATH="$BREW_PREFIX/bin:$PATH"
    if ! command -v pkg-config >/dev/null 2>&1; then
        echo "Installing: pkgconf (pkg-config)"
        brew install pkgconf
    fi
    missing=""
    # "<pkg-config module> <formula to install when it is missing>"
    for pair in "sdl2 sdl2-compat" "epoxy libepoxy"; do
        set -- $pair
        if pkg-config --exists "$1"; then
            echo "$1: already installed ($(pkg-config --modversion "$1"))."
        else
            missing="$missing $2"
        fi
    done
    if [ -n "$missing" ]; then
        echo "Installing:$missing"
        # shellcheck disable=SC2086
        brew install $missing
    fi
fi

step "Python environment"

VENV="$ROOT_DIR/.venv"
if [ -x "$VENV/bin/python3" ] && ! py_ok "$VENV/bin/python3"; then
    # Made by an earlier version of this script from the system's Python 3.9,
    # or its Python is gone: a venv cannot change its Python, so make it again.
    echo ".venv has $("$VENV/bin/python3" --version 2>&1 || echo "a missing Python"); making it again with $PYTHON ..."
    rm -rf "$VENV"
fi
if [ ! -x "$VENV/bin/python3" ]; then
    echo "Creating virtualenv in .venv ..."
    "$PYTHON" -m venv "$VENV"
else
    echo ".venv already exists ($("$VENV/bin/python3" --version 2>&1))."
fi

# Test that every dependency imports, not that the directory exists and not
# just one of them. A venv can exist and be empty, or have been built against a
# Python that has since gone away, or -- the case that actually bit -- have been
# created by an earlier version of this script that installed a shorter list.
# Checking one package and installing four means the other three are skipped
# forever on any venv that predates them.
DEPS="capstone pytest pefile numpy"
DEP_IMPORTS="import capstone, pytest, pefile, numpy"
if ! "$VENV/bin/python3" -c "$DEP_IMPORTS" >/dev/null 2>&1; then
    echo "Installing dependencies ..."
    "$VENV/bin/python3" -m pip install --quiet --upgrade pip
    # shellcheck disable=SC2086
    "$VENV/bin/python3" -m pip install --quiet $DEPS
else
    echo "Dependencies already installed."
fi

# --------------------------------------------------------------------- images
# Each image is built once and reused. The MSVC ones also download ~1.5 GB of
# toolchain from Microsoft under the licence vsdownload.py accepts on your
# behalf, so say what is about to happen rather than doing it silently.
ensure_image() {
    local name="$1" platform="$2" dockerfile="$3" context="$4" note="$5"
    if docker image inspect "$name" >/dev/null 2>&1; then
        echo "$name: already built."
        return 0
    fi
    echo "$name: building ($note) ..."
    if ! docker build --platform "$platform" -t "$name" \
            -f "$context/$dockerfile" "$context"; then
        echo >&2
        echo "Building $name failed. Without it there is no 32-bit x86 to" >&2
        echo "compare the lifted C against, so nothing could be verified." >&2
        exit 1
    fi
    echo "$name: built."
}

if [ "$with_images" = 1 ]; then
    if ! command -v docker >/dev/null 2>&1 || ! docker info >/dev/null 2>&1; then
        echo "Docker must be installed and running to build the images: it" >&2
        echo "supplies the 32-bit x86 CPU the lifted C is compared against." >&2
        exit 1
    fi

    step "Snippet oracle (1 image)"
    ensure_image "$GCC_IMAGE" linux/386 Dockerfile.gcc . "~280 MB, under a minute"

    step "MSVC toolchain for corpus and XBE (2 images)"
    ensure_image "$MSVC_BUILD" linux/amd64 Dockerfile.amd64 \
        tools/conformance/msvc-wine "~975 MB, ~5 min, downloads MSVC"
    ensure_image "$MSVC_RUN" linux/386 Dockerfile.i386 \
        tools/conformance/msvc-wine "~985 MB, ~4 min, downloads MSVC"
fi

# ------------------------------------------------------------------ py shim
# The docs use Windows' `py -3` launcher throughout. A plain alias cannot stand
# in for it: `py -3 -m tools.conformance` would expand to `python3 -3 -m ...`,
# and -3 is not a valid python3 flag. A function can drop it -- and can also
# prefer a .venv, so the documented commands find the suite's dependencies
# without anyone having to remember to activate anything.
step "The py shim"

MARKER="# >>> xboxrecomp py shim >>>"
RC="${ZDOTDIR:-$HOME}/.zshrc"

if [ -f "$RC" ] && grep -qF "$MARKER" "$RC"; then
    echo "Already present in $RC."
else
    cat >> "$RC" <<'SHIM'

# >>> xboxrecomp py shim >>>
# Windows' `py -3` launcher, for the docs that use it. Not an alias: `py -3 -m
# foo` would expand to `python3 -3 -m foo`, and -3 is not a valid flag. Prefers
# a .venv found by walking up from the current directory, so the documented
# commands pick up project dependencies without an explicit activate.
py() {
    [ "${1:-}" = "-3" ] && shift
    local d="$PWD"
    while [ "$d" != "/" ]; do
        if [ -x "$d/.venv/bin/python3" ]; then
            command "$d/.venv/bin/python3" "$@"
            return $?
        fi
        d="$(dirname "$d")"
    done
    command python3 "$@"
}
# <<< xboxrecomp py shim <<<
SHIM
    echo "Added to $RC."
fi

# ----------------------------------------------------------------------- done
step "Setup complete"

cat <<EOF
Open a new shell (or: source $RC), then run the tests exactly as the README
describes -- the same commands used on Windows:

    py -3 -m pytest tools/
EOF

# Without --test we did not build anything, but the images may well be there
# from an earlier run. Ask, rather than assuming they are missing and telling
# someone to rebuild what they already have.
have_images=0
if [ "$with_images" = 1 ]; then
    have_images=1
elif command -v docker >/dev/null 2>&1 && docker info >/dev/null 2>&1; then
    have_images=1
    for img in "$GCC_IMAGE" "$MSVC_BUILD" "$MSVC_RUN"; do
        docker image inspect "$img" >/dev/null 2>&1 || have_images=0
    done
fi

if [ "$have_images" = 1 ]; then
cat <<EOF
    py -3 -m tools.conformance                  # snippets + corpus
    py -3 -m tools.conformance --only snippets
    py -3 -m tools.conformance --xbe tools/conformance/test.xbe

No title to hand? py -3 tools/conformance/mkxbe.py writes a synthetic one.
EOF
else
cat <<EOF

The conformance suite additionally needs container images, which are not built
yet. Add them with:

    bash tools/macos/setup.sh --test
EOF
fi
