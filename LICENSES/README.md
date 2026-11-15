# Third-party licence texts

`LGPL-2.1.txt` is the verbatim GNU Lesser General Public License v2.1, from

    https://www.gnu.org/licenses/old-licenses/lgpl-2.1.txt

It applies to the files listed under "LGPL-2.1-or-later" in the top-level
NOTICE — the MCPX APU sources and `src/nv2a/nv2a_regs.h`, extracted from xemu
and remaining the copyright of espes, Jannik Vogel and Matt Borgerson.

Shipping the verbatim text alongside those files is an LGPL requirement, not
a courtesy.

## Shipped by the Android app

The Android APK (XRecomp) also carries the components below, listed with their versions under « The Android app »
in NOTICE. Their binary distribution requires the licence text to travel with them: the app build copies this folder,
`LICENSE` and `NOTICE` into its assets, and the app shows them (About › Licences).

| File | Applies to |
|---|---|
| `Apache-2.0-WITH-LLVM-exception.txt` | LLVM: clang, lld, compiler-rt builtins and libunwind of the on-device toolchain (`libxbacc.so`, the sysroot's `lib/clang/`), verbatim from the LLVM project's `LICENSE.TXT` (its third-party and legacy parts left out: none of that code is built in) |
| `PSF-2.0.txt` | CPython (`libpython3.13.so`, `libxbapython.so`, the standard library zip), verbatim `LICENSE` of the CPython sources |
| `Apache-2.0-apksig.txt` | apksig (in `classes.dex`: signs the game apps of Manage › Create an app), verbatim `LICENSE` of the `com.android.tools.build:apksig` jar (its copyright line, then the Apache License 2.0) |
| `BSD-3-Clause-capstone.txt` | Capstone (`libcapstone.so` and its Python bindings), verbatim from the Capstone distribution |
| `MIT-OpenBIOS.txt` | OpenBIOS, the open-source PlayStation BIOS of PCSX-Redux's nugget tree (`assets/ps1-bios/openbios.bin`, booted by the PS1 runtime), verbatim `LICENSE` of the nugget sources at the pinned commit |
| `MIT-paraLLEl-RDP.txt` | paraLLEl-RDP, Granite and volk (`libmain.so`: the N64 Vulkan RDP, `third_party/parallel-rdp`), their three MIT licences verbatim |
| `Apache-2.0-Vulkan-Headers.txt` | the Khronos Vulkan headers paraLLEl-RDP is built with (`third_party/parallel-rdp/vulkan-headers`), verbatim Apache License 2.0 |
| `BSD-2-Clause-libadrenotools.txt` | libadrenotools and linkernsbypass (`libxba_vkdriver.so` and the four hook libraries: loading a user-supplied Vulkan driver for the N64 RDP, `third_party/adrenotools`), verbatim `LICENSE` of libadrenotools (linkernsbypass carries the same text) |
| `MIT-lsfg-vk.txt` | framegen (lsfg-vk 1.0, GameNative/lsfg-vk-android d33caceb) and its volk (`libmain.so`: frame generation's Vulkan compute chain, fetched at pinned commits by `src/framegen/lsfg_sources.cmake`), their two MIT licences verbatim |
| `Zlib-DXVK.txt` | DXVK v2.6.2's DXBC compiler (`libxba_import.so`: translates the player's Lossless.dll shaders), and `src/framegen/dxvk_shim/` (a DXVK derivative), verbatim `LICENSE` of DXVK |
| `MIT-Khronos-SPIRV-Headers.txt` | SPIRV-Headers (DXVK's `include/spirv`, in the DXBC translator of `libxba_import.so`), verbatim `LICENSE` |
