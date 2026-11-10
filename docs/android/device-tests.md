# The toolkit's tests on a device

XBA-19. The C test projects under `tests/` built as Android executables, packed into a bundle that runs through adb,
without root or Termux.

```bash
docker build -f tools/docker/android.Dockerfile --target device-tests-out --output type=local,dest=out .
tools/android/run_tests.sh out/xba-device-tests.tar.gz            # everything (a few minutes)
tools/android/run_tests.sh out/xba-device-tests.tar.gz memory     # only the tests whose name has "memory"
```

`run_tests.sh` pushes the bundle to `/data/local/tmp`, runs `run.sh` there, and brings back
`device-tests-<model>-<date>.md` (a pass / skip / fail table) and `device-tests-logs/`.

## What is in the bundle

- 102 tests from 68 projects (2026-11-09). Not built: the Windows-only projects, and `input_keyboard_sdl`, which needs
  an SDL window (graphics tests run from the app, A3). `build-report.md` in the bundle lists them with the reason.
- `tools/android/bundle_tests.py` reads each project's tests from ctest (`--show-only=json-v1`): command line,
  working directory, environment, `SKIP_RETURN_CODE`, `WILL_FAIL`. It copies every file the tests point to (the
  synthetic `tools/conformance/test.xbe`, which is ours) and rewrites the paths relative to the bundle.
- `run.sh` is POSIX sh for Android's toybox: `timeout` of 300 s per test, `LD_LIBRARY_PATH` for `libSDL2.so`,
  `TMPDIR` inside the bundle, and the SDL dummy drivers.
- CI builds the bundle once a week (job `device-tests`, 12 minutes, because every project rebuilds the toolkit) and
  fails when a project that is not in `tools/ci/android-unbuilt.txt` stops building (XBA-53). It does not publish the
  bundle: build it when there is a device run to do.

## Reading the result

Compare with [baseline-arm64.md](baseline-arm64.md). A test that passes on Linux arm64 but not on the device gets a
ticket under A2 / A3 / A4. Expected on the device: `mmio_decode_a64` is the test qemu could not judge (XBA-45), and
the memory tests check XBA-21. `recomp_selftest` runs the checks of the app shell's self-test (XBA-51) from the
shell domain; the same checks in the app process are `RECOMP_SHELL_SELFTEST=1`
([app-shell.md](app-shell.md#self-test-in-the-app-process)), and a check that passes here but fails there is an app
difference (untrusted_app, ART, libsigchain).
