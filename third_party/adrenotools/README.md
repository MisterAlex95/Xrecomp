# libadrenotools (vendored)

Loads a Vulkan driver the player supplied (Mesa Turnip, for example) in place of the device's own, without root, by
Billy Laws. Upstream's README is `README.upstream.md`. The app uses it for the N64 Vulkan RDP only
(`consoles/n64/runtime/n64_vk_driver.c`, `android/app/src/main/cpp/vkdriver.c`, `consoles/README.md`).

## Source

- <https://github.com/bylaws/libadrenotools> at `8fae8ce254dfc1344527e05301e43f37dea2df80` (2024-09-10).
- Its submodule `lib/linkernsbypass`: <https://github.com/bylaws/liblinkernsbypass> at
  `aa3975893d83ef1bc84c321ec60c65fbf1287887`, copied in (no submodule here).

## Licence

BSD 2-Clause, Copyright (c) 2021, Billy Laws, for both (`LICENSE`, `lib/linkernsbypass/LICENSE`). The APK carries
the text as `LICENSES/BSD-2-Clause-libadrenotools.txt`.

## Trimmed

Removed from upstream's tree: `tools/` (driver packaging notes and shims for other projects), `build_asm.sh`,
`adrenotools.pc.in`, `.gitmodules`. The sources and both `CMakeLists.txt` are unchanged.

## How the app builds and uses it

- Android arm64 only (upstream's `CMakeLists.txt` stops elsewhere). `android/app/src/main/cpp/CMakeLists.txt` adds
  this directory, sets C++20 on its targets, and links the static `adrenotools` into **`libxba_vkdriver.so`**
  (`vkdriver.c`, one export: `xba_vkdriver_open`). The four hook libraries (`libhook_impl.so`, `libmain_hook.so`,
  `libfile_redirect_hook.so`, `libgsl_alloc_hook.so`) are separate `.so` in the APK: adrenotools loads them by name
  from the app's native library directory at run time (the app uses legacy packaging, so they are extracted there).
- `libmain.so` does not link any of it. The N64 runtime `dlopen`s `libxba_vkdriver.so` only when a custom driver is
  selected (`RECOMP_VK_DRIVER_DIR`, `docs/runtime/environment-variables.md`): linkernsbypass's static constructor
  (it patches the page of `dlopen` to find private linker symbols) then never runs for a player who did not pick one.
- Only `ADRENOTOOLS_DRIVER_CUSTOM` is used; no file redirect, no GPU mapping import, no BCn patch.
- If the custom library cannot be loaded, adrenotools' hook silently loads the device's own driver instead (logcat tag
  `hook_impl`, "falling back!"). The runtime therefore checks the `driverID` of the device it gets.

## Updating

Copy the new trees over these files (same trimming), update the two commits here and in `NOTICE`, rebuild the APK and
run a game with a custom driver on a device.
