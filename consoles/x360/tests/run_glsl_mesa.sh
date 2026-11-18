#!/bin/sh
# run_glsl_mesa.sh (XBA-270): test_xenos_glsl against a real GLES 3.0 implementation -- Mesa's llvmpipe on EGL's
# surfaceless platform, in a throwaway Debian container (no display, no GPU): builds xenos_glsl_run.c there and runs
# pytest on test_xenos_glsl.py with $XENOS_GLSL_RUN set. Needs docker and the network for the image's packages.
#
#   consoles/x360/tests/run_glsl_mesa.sh
set -eu
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../../.." && pwd)
image=xba-x360-gles
command -v docker >/dev/null || { echo "docker is required" >&2; exit 1; }
timeout 1800 docker build -q -t "$image" - <<'EOF' >/dev/null
FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends gcc libc6-dev libegl-dev libgles-dev libegl-mesa0 \
        libgl1-mesa-dri python3 python3-pytest && rm -rf /var/lib/apt/lists/*
EOF
# the tree read-only; the harness and pytest's files in the container's /tmp
timeout 1800 docker run --rm --network none -v "$repo:/src:ro" -e PYTHONDONTWRITEBYTECODE=1 \
    -e EGL_PLATFORM=surfaceless -e LIBGL_ALWAYS_SOFTWARE=1 -w /src "$image" sh -c '
    gcc -O1 -o /tmp/xenos_glsl_run consoles/x360/tests/xenos_glsl_run.c -lEGL -lGLESv2 || exit 1
    cd /src/consoles/x360/tools && XENOS_GLSL_RUN=/tmp/xenos_glsl_run PYTHONPATH=/src:/src/consoles/x360/tools \
        python3 -m pytest -q -p no:cacheprovider -rs x360_tools/test_xenos_glsl.py'
