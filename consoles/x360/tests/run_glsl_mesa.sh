#!/bin/sh
# run_glsl_mesa.sh (XBA-270): test_xenos_glsl against a real GLES 3.0 implementation -- Mesa's llvmpipe on EGL's
# surfaceless platform, in a throwaway Debian container (no display, no GPU): builds xenos_glsl_run.c there and runs
# pytest on test_xenos_glsl.py with $XENOS_GLSL_RUN set (vertex shaders through transform feedback, pixel shaders
# into a float target, against the test's reference interpreter). Sources go in with `docker cp` (CI runs docker
# without bind mounts); the run has no network, the image build needs it for its packages. About 30 s once the
# image exists. Exits non-zero when a case differs or the harness cannot run.
#
#   consoles/x360/tests/run_glsl_mesa.sh
set -eu
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../../.." && pwd)
image=xba-x360-gles
command -v docker >/dev/null || { echo "docker is required" >&2; exit 1; }
echo "== image $image (Debian bookworm, Mesa llvmpipe)"
timeout 1800 docker build -q -t "$image" - <<'EOF' >/dev/null
FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends gcc libc6-dev libegl-dev libgles-dev libegl-mesa0 \
        libgl1-mesa-dri python3 python3-pytest && rm -rf /var/lib/apt/lists/*
EOF
cid=
cleanup() { [ -z "$cid" ] || docker rm -f "$cid" >/dev/null 2>&1 || true; }
trap cleanup EXIT
cid=$(docker create --network none -e PYTHONDONTWRITEBYTECODE=1 -e EGL_PLATFORM=surfaceless \
    -e LIBGL_ALWAYS_SOFTWARE=1 "$image" sh -c '
    cd /usr/src && gcc -O1 -o /tmp/xenos_glsl_run consoles/x360/tests/xenos_glsl_run.c -lEGL -lGLESv2 || exit 1
    cd /usr/src/consoles/x360/tools && XENOS_GLSL_RUN=/tmp/xenos_glsl_run \
        python3 -m pytest -q -s -p no:cacheprovider -rs x360_tools/test_xenos_glsl.py')
tar -C "$repo" --exclude=__pycache__ -cf - consoles/x360/tests/xenos_glsl_run.c consoles/x360/tools tools/__init__.py \
    tools/ppc tools/xex | docker cp - "$cid:/usr/src"
timeout 1800 docker start -a "$cid"
