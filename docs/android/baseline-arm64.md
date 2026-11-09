# Linux arm64 baseline

XBA-14, 2026-11-09, upstream v0.17.0 + our `main` at the time of writing. A later Android failure is compared to this.

## How it was made

There is no arm64 Linux host yet. The arm64 side is **cross-compiled** (Debian trixie multiarch, gcc 14
`aarch64-linux-gnu`) and **run under qemu-user** (`qemu-aarch64`, no binfmt on the host):

```bash
docker build -f tools/docker/linux-arm64.Dockerfile --target test .   # toolchain: cmake/toolchains/linux-aarch64-cross.cmake
docker build -f tools/docker/linux.Dockerfile --target test .         # the x86-64 side (CI)
```

qemu-user is not a real arm64 host. It runs the code on a real arm64 instruction set, but it manages the guest's
address space and signal frames itself. A pass is good evidence. A failure is a lead to confirm on hardware.

## Summary

| | x86-64 (native) | arm64 (cross + qemu-user) |
|---|---|---|
| Toolkit build | ok | ok, ELF aarch64 |
| Compiler warnings (toolkit, unique) | 94 | 94, the same list (no new warning) |
| C test projects | 80, 11 Windows-only | 80, 11 Windows-only |
| Tests | 99 pass, 1 skip, 0 fail | 90 pass, 3 skip, **7 fail** (3 projects) |
| `pytest tools/` | 981 pass, 8 skip | n/a (host independent) |

## The arm64 failures

| Project | What fails | Reading | Next |
|---|---|---|---|
| `mmio_decode_a64` (1) | Trapped `ldr s/d/q`: the SIMD register written in the signal frame is not the one the code reads after `sigreturn` (got 0x2F2F2F2F, want 0x3FC00000). Output: 30 traps, 1 unknown, 4 failures, all four on `ldr s`, `ldr d` and `ldr q`. | Probably qemu-user. Its `fpsimd_context` write-back is the suspect. It is exactly what XBA-22 depends on, though. | Confirm on hardware (XBA-45). |
| `kernel_memory_bridge_posix` (2), `memory_regressions` (4 of 5) | SIGSEGV after `xbox_MemoryLayoutInit` cannot map the contiguous window at base + 0x80000000 (ENOMEM) nor the kernel VA 0x80010000. | Probably qemu-user. It places guest mappings itself, high in the address space, and the base view landed where base + 2 GB was not free. | Confirm on hardware (XBA-45). It feeds XBA-21. |

## Skips that are expected

- `mmio_decode_a64` on x86-64: arm64-only.
- `mmx_helpers`, `mmx_integer`, `xmm_helpers` on arm64: they compare the scalar fallback with the x86-64 intrinsics,
  so there is nothing to compare on arm64. They exit 77. `mmx_helpers` and `xmm_helpers` did not declare
  `SKIP_RETURN_CODE 77` and showed up as failures: fixed in this fork. The scalar fallback that arm64 uses is still
  checked, against the intrinsics, on x86-64.

## Per project

Windows-only projects are left out: apu_vp, fb_present, idex_channel, input_xinput_imports, kernel_dispatch,
kernel_irql_abi, kernel_physical_address, kernel_regressions, mmio_decode, nv2a_pvideo, xaudio2.
"build only" means the project registers no test (`d3d8_smoke`).

| Project | x86-64 (native) | arm64 (cross, qemu-user) |
|---|---|---|
| adpcm_decode | 1 pass | 1 pass |
| apu_dsp_ack | 1 pass | 1 pass |
| apu_irq | 1 pass | 1 pass |
| apu_lifecycle | 1 pass | 1 pass |
| apu_mixdown | 2 pass | 2 pass |
| apu_monitor | 3 pass | 3 pass |
| apu_phys | 1 pass | 1 pass |
| apu_wav | 8 pass | 8 pass |
| cmake_d3d8_consumer | build only | build only |
| d3d8_smoke | build only | build only |
| dpc_order | 1 pass | 1 pass |
| fist | 1 pass | 1 pass |
| flip_hold | 1 pass | 1 pass |
| fp_precision | 1 pass | 1 pass |
| guest_cpu | 1 pass | 1 pass |
| input_keyboard | 1 pass | 1 pass |
| input_keyboard_map | 1 pass | 1 pass |
| input_keyboard_sdl | 1 pass | 1 pass |
| input_map | 2 pass | 2 pass |
| input_sdl_hotplug | 1 pass | 1 pass |
| irq_progress | 1 pass | 1 pass |
| irq_safe_points | 1 pass | 1 pass |
| kernel_audio_setting | 1 pass | 1 pass |
| kernel_bare_cdrom0 | 1 pass | 1 pass |
| kernel_bridge | 1 pass | 1 pass |
| kernel_directory | 1 pass | 1 pass |
| kernel_directory_posix | 1 pass | 1 pass |
| kernel_dvd_scsi | 1 pass | 1 pass |
| kernel_eeprom_settings | 1 pass | 1 pass |
| kernel_events | 2 pass | 2 pass |
| kernel_fatx_overwrite | 1 pass | 1 pass |
| kernel_fd_limit | 1 pass | 1 pass |
| kernel_file_delete_log | 1 pass | 1 pass |
| kernel_file_relative | 1 pass | 1 pass |
| kernel_file_status | 1 pass | 1 pass |
| kernel_guest_cpu | 1 pass | 1 pass |
| kernel_io_apc | 1 pass | 1 pass |
| kernel_memory_bridge_posix | 2 pass | **2 FAIL** |
| kernel_memory_posix | 1 pass | 1 pass |
| kernel_missing_report | 1 pass | 1 pass |
| kernel_object_paths | 1 pass | 1 pass |
| kernel_open_directory | 1 pass | 1 pass |
| kernel_path | 1 pass | 1 pass |
| kernel_wait_log | 2 pass | 2 pass |
| memory_layout_posix | 1 pass | 1 pass |
| memory_regressions | 5 pass | 1 pass, **4 FAIL** |
| mmio_decode_a64 | 1 skip | **1 FAIL** |
| mmx_helpers | 1 pass | 1 skip |
| mmx_integer | 1 pass | 1 skip |
| mmx_pmaddwd | 1 pass | 1 pass |
| mmx_rounding | 1 pass | 1 pass |
| nv2a_combiner | 1 pass | 1 pass |
| nv2a_ffp_backend | 3 pass | 3 pass |
| nv2a_pb_constants | 1 pass | 1 pass |
| nv2a_pb_walk | 1 pass | 1 pass |
| nv2a_raster_batch | 1 pass | 1 pass |
| nv2a_tex_filter | 12 pass | 12 pass |
| nv2a_vsh | 2 pass | 2 pass |
| recomp_diff | 1 pass | 1 pass |
| stack_ops | 1 pass | 1 pass |
| usb_ohci_posix | 1 pass | 1 pass |
| vblank_ack | 1 pass | 1 pass |
| win32_compat_posix | 1 pass | 1 pass |
| win32_handles_posix | 1 pass | 1 pass |
| win32_thread_posix | 1 pass | 1 pass |
| win32_waitable_timer_posix | 1 pass | 1 pass |
| wma_decoder | 1 pass | 1 pass |
| x87_trig | 2 pass | 2 pass |
| xmm_helpers | 1 pass | 1 skip |
