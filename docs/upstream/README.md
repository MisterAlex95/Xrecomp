# xboxrecomp (upstream README)

> The README of [sp00nznet/xboxrecomp](https://github.com/sp00nznet/xboxrecomp) as it stood when this fork's README
> was rewritten (2026-11-14), kept for its background, library table, FAQ, community ports and test notes. Paths are
> relative to the repository root. The changelog moved to [CHANGELOG.md](../../CHANGELOG.md).

```
 #   #  ####    ###   #   #         #####   ###    ###   #       ###
 #   #  #   #  #   #  #   #           #    #   #  #   #  #      #
  # #   ####   #   #   # #            #    #   #  #   #  #       ##
  # #   #   #  #   #   # #            #    #   #  #   #  #         #
 #   #  #   #  #   #  #   #           #    #   #  #   #  #         #
 #   #  ####    ###   #   #           #     ###    ###   #####   ###

 Static Recompilation Toolkit for Original Xbox Games
```

> Turn any Xbox game binary into a native executable for Windows, Linux or macOS. No emulation. No interpreter. Just raw, recompiled C.

**[Join the sp00nznet recomp Discord](https://discord.gg/CRpzGWZFcu)** — the
community hub for sp00nznet's recomp projects, where ps3recomp development
happens in the open. Good place to ask questions, show a port you are working
on, or find out what people are stuck on before you duplicate the effort.

**Title-agnostic.** The runtime, kernel layer, D3D8 abstraction, NV2A translator, and the Python pipeline (parser → disasm → func_id → abi_analysis → recomp) all derive per-title layout and behavior from the XBE itself. *Burnout 3: Takedown* was the reference title the toolkit was built against, so many docs use its metrics as examples — see `docs/technical/candidate-games.md` for ports in progress.

### Recent Changes

**Current version: v0.17.0 — _"Empty Promises"_ (November 2026).**
See the [Changelog](../../CHANGELOG.md) for what landed and when.

---

## What Is This?

This is a complete toolkit for **statically recompiling original Xbox (2001-2005) games** from their retail XBE executables into native programs: Windows first, with Linux and macOS (x86-64 and arm64) hosts too.

Static recompilation takes the raw x86 machine code from an Xbox binary and translates every function — every `mov`, every `jmp`, every `call` — into equivalent C source code. That C code compiles with MSVC into a native x86-64 `.exe` that runs on modern Windows; the runtime libraries also build on Linux and macOS, x86-64 or arm64, with SDL2 and OpenGL standing in for Win32 and D3D11. The game's original logic executes directly on your CPU, not through an interpreter or JIT compiler.

**This is the first *public* static recompilation toolkit for the original Xbox.**
Microsoft got here first: their internal Ficl/Fission recompiler shipped Xbox
back-compat on the 360. We have since studied it — see
[Microsoft's Own Recompiler](../../docs/technical/ms-fusion-recompiler.md).

The technique has been proven on other platforms — [N64Recomp](https://github.com/N64Recomp/N64Recomp) showed MIPS-to-C was viable, [XenonRecomp](https://github.com/hedge-dev/XenonRecomp) brought it to Xbox 360's PowerPC — but nobody had tackled the OG Xbox until now. Its x86 architecture makes it both easier (same instruction set family as the host) and harder (variable-length instructions, complex addressing modes, x87 FPU stack) than MIPS or PPC targets.

### Why Not Just Use an Emulator?

Emulators are great. Cxbx-Reloaded and xemu do incredible work. But static recomp offers some unique advantages:

- **Native performance** — recompiled code runs at full speed, no interpretation overhead
- **Moddability** — the output is human-readable C code; you can patch, extend, and improve the game
- **Portability** — the C output can target any platform with a C compiler (ARM, RISC-V, WebAssembly...)
- **Preservation** — a self-contained native binary is the ultimate form of game preservation
- **Understanding** — the process forces you to deeply understand the game at the machine code level

## The Pipeline

```
         YOUR XBOX DISC
              |
              v
    +-------------------+
    |  1. Extract XBE   |     Extract default.xbe from the disc image
    +-------------------+
              |
              v
    +-------------------+
    |  2. Parse XBE     |     Read headers, sections, kernel imports
    +-------------------+     tools/xbe_parser/
              |
              v
    +-------------------+
    |  3. Disassemble   |     Find functions, build control flow graphs
    +-------------------+     tools/disasm/
              |
              v
    +-------------------+
    |  4. Identify      |     Classify: CRT, RenderWare, D3D, game code
    +-------------------+     tools/func_id/
              |
              v
    +-------------------+
    |  5. Lift to C     |     Translate x86 instructions to C statements
    +-------------------+     tools/recomp/
              |
              v
    +-------------------+
    |  6. Build Runtime  |    Kernel shim, D3D translation, memory layout
    +-------------------+     templates/runtime/
              |
              v
    +-------------------+
    |  7. Compile & Run  |    MSVC builds native .exe — game runs!
    +-------------------+
```

## Runtime Libraries

Following the [RexGlueSDK](https://github.com/rexglue/rexglue-sdk) pattern (which does the same for Xbox 360 via Xenia), xboxrecomp provides link-time libraries extracted from [xemu](https://github.com/xemu-project/xemu) and purpose-built compatibility layers. Your recompiled game links against these — no emulator needed at runtime.

| Library | Source | What It Does |
|---------|--------|-------------|
| **xbox_kernel** | Custom | Xbox kernel → Win32 (all 371 ordinals routed: 347 `bridge_*` functions + 24 data exports — memory, file I/O, threading, sync, crypto, HAL, EEPROM, SMBus), plus the NV2A pushbuffer executor for titles that statically link the XDK's D3D |
| **xbox_d3d8** | Custom | D3D8 → D3D11 graphics: **4-stage multi-texture** FFP pipeline, **NV2A register combiner** pixel shaders, **programmable vertex shaders** (NV2A microcode → HLSL), **hardware T&L lighting** (8 lights), **vertex fog**, DrawPrimitiveUP ring buffer, texture unswizzling, 20+ format conversions. On Linux/macOS a separate OpenGL 3.3 back end (SDL2 + libepoxy, `d3d8_gl.c`) |
| **xbox_dsound** | Custom | DirectSound → software mixer (IDirectSound8/IDirectSoundBuffer8) |
| **xbox_apu** | xemu *(LGPL-2.1+)* | MCPX APU audio (256-voice processor, ADPCM/PCM, envelopes, HRTF). Output through XAudio2 (waveOut fallback) on Windows, SDL2 elsewhere. The GP DSP doorbell ack is derived by default (`RECOMP_APU_DSP_ACK=auto`; an explicit address, or `0`/`off`, still overrides) |
| **xbox_nv2a** | xemu *(regs, LGPL-2.1+)* + Custom | NV2A GPU (register handlers, MMIO interception, push buffer parsing, PGRAPH → D3D11 translation) |
| **xbox_input** | Custom | Xbox gamepad → XInput, plus the keyboard as the port-0 pad (`RECOMP_KEYBOARD=1`) with one key map on every host |
| **xbox_usb** | Custom | MCPX OHCI host controllers, so a title's statically linked XAPI finds a pad |
| **xbox_video** | Custom | FMV playback: Media Foundation decode onto a D3D8 texture, plus a window on the guest framebuffer. For titles whose video is a container Windows already decodes, the emulated decoder does not have to work for the video to be watchable — and the title still decides when it plays. Linux/macOS get the framebuffer window (SDL2) but no decoder |

### Building the Libraries

```bash
cd xboxrecomp
cmake -S . -B build
cmake --build build --config Release
```

This produces the static libraries in the table above, plus the `platform` layer they share, in `build/src/*/Release/` (`build/src/*/` with a single-config generator). Link your game project against `xboxrecomp` (umbrella target) or individual libraries.

On POSIX, a project that uses the NV2A pushbuffer executor directly can omit
the bundled OpenGL D3D8 layer and its libepoxy dependency:

```bash
cmake -S . -B build -DXBOXRECOMP_BUILD_D3D8=OFF
```

When embedding xboxrecomp with `add_subdirectory`, a parent may instead define
its own `xbox_d3d8` target before adding the toolkit. The default remains the
bundled D3D11 implementation on Windows and OpenGL implementation on POSIX.

**This repo builds libraries only — there is no game `.exe` here, and building it will never produce one.** The executable is built by *your* game project, which lives in its own directory and links these libraries. Start it by copying [`templates/new-game/`](../../templates/new-game/): it has the `CMakeLists.txt` that produces the `.exe` and the `main.c` that boots the guest. See [Getting Started, Step 6](../../docs/GETTING_STARTED.md#step-6-create-your-game-project).

### Integration Pattern

Your recompiled game provides two callback functions that the kernel bridge calls to resolve function addresses:

```c
typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(uint32_t xbox_va);        // Auto-generated dispatch table
recomp_func_t recomp_lookup_manual(uint32_t xbox_va);  // Hand-written overrides
```

The recompiler output (`tools/recomp`) generates these automatically. The xboxrecomp libraries handle everything else — memory layout, kernel calls, graphics, audio, and input.

### Architecture

```
┌─────────────────────────────────────────────────┐
│              Your Game (.exe)                     │
│  ┌──────────┐ ┌──────────┐ ┌──────────────────┐ │
│  │ recomp/  │ │ manual   │ │ game-specific    │ │
│  │ gen/*.c  │ │ overrides│ │ loaders/formats  │ │
│  └────┬─────┘ └────┬─────┘ └────────┬─────────┘ │
│       │             │                │            │
│       └──────┬──────┘────────────────┘            │
│              │ recomp_lookup() / ICALL dispatch    │
├──────────────┼────────────────────────────────────┤
│              │   xboxrecomp libraries             │
│  ┌───────────┴──────────┐                         │
│  │    xbox_kernel        │  Memory layout, file    │
│  │    (kernel_bridge.c)  │  I/O, threading, sync   │
│  └───────────┬──────────┘                         │
│              │                                     │
│  ┌───────┐ ┌┴──────┐ ┌────────┐ ┌──────┐ ┌─────┐│
│  │xbox_  │ │xbox_  │ │xbox_   │ │xbox_ │ │xbox_││
│  │d3d8   │ │dsound │ │apu     │ │nv2a  │ │input││
│  │D3D8→  │ │DSound→│ │MCPX APU│ │NV2A  │ │XPP→ ││
│  │D3D11  │ │mixer  │ │(xemu)  │ │(xemu)│ │XInput│
│  └───────┘ └───────┘ └────────┘ └──────┘ └─────┘│
├──────────────────────────────────────────────────┤
│  Windows: D3D11/XInput/XAudio2; POSIX: SDL2/GL   │
└──────────────────────────────────────────────────┘
```

## Quick Start

### Prerequisites

- **Windows 11/10** (D3D11 backend) — or **Linux** / **macOS**, x86-64 or arm64 (OpenGL + SDL2 backend)
- **Linux**: `tools/linux/install_deps.sh` installs the build dependencies
- **macOS**: `tools/macos/setup.sh` (Homebrew SDL2 + libepoxy; `--test` adds the Docker images the conformance suite needs)
- **Python 3.10+** with `capstone` (`pip install capstone`)
- **Visual Studio 2022** (MSVC compiler) on Windows; Clang or GCC elsewhere
- **CMake 3.20+**
- An original Xbox game disc image (you must own the game)

`py -3` below is the Windows Python Launcher — on Linux and macOS use
`python3`, and on a Microsoft Store install that has no `py`, use `python`.

### Step-by-Step

The condensed version. [docs/GETTING_STARTED.md](../../docs/GETTING_STARTED.md) is
the long one, and the one to read if a step here does not go as written — it
explains *why* each flag is there, which is what you need when your title
behaves differently from the example.

```bash
# 1. Clone this repo
git clone https://github.com/sp00nznet/xboxrecomp.git
cd xboxrecomp

# 2. Extract default.xbe from your Xbox disc image
#    (Use xdvdfs, extract-xiso, or similar tool)
mkdir game_files
# copy default.xbe and game data into game_files/

# 3. Parse the XBE — learn what you're working with
#    --json is NOT optional: step 4 reads the section layout back out of it.
#    The name matters too. Step 4 looks for <xbe stem>_analysis.json beside the
#    XBE, so keep it there and keep the suffix.
py -3 -m tools.xbe_parser game_files/default.xbe --json game_files/default_analysis.json
#    Output: section map, kernel imports, entry point, XDK version

# 4. Disassemble — find all functions
py -3 -m tools.disasm game_files/default.xbe --text-only
#    Output: tools/disasm/output/ (functions.json, xrefs.json, strings.json)
#    --text-only does what it says: only .text. A title with code in its XDK
#    library sections (D3D, DSOUND, XPP...) needs them named explicitly, e.g.
#    --extra-sections XIPS,DOLBY. Drop --text-only to take every code section.

# 5. Identify library functions
py -3 -m tools.func_id game_files/default.xbe -v
#    Output: tools/func_id/output/ (CRT, RenderWare, vtables classified)

# 6. Recover calling conventions and parameter counts
py -3 -m tools.abi_analysis game_files/default.xbe -v
#    Output: tools/abi_analysis/output/abi_functions.json
#    Skipping this still "works", but every function falls back to
#    cdecl / 0 params / int-or-void, so the generated signatures are guesses.

# 6b. Optional: real names instead of sub_XXXXXXXX, if you have Ghidra.
#     FidDb recognises the statically linked CRT/XDK helpers and names a few
#     hundred of them. Do it BEFORE step 8: the recompiler emits whatever name
#     is on the functions.json entry, so the names reach the generated C,
#     crash traces and ABI reports. See docs/GETTING_STARTED.md step 4.5.
XBE=game_files/default.xbe tools/ghidra_naming/run_ghidra.sh
py -3 tools/ghidra_naming/merge_names.py --apply

# 7. Create your game project — this is what becomes the .exe
#    The toolkit is a library; the executable lives in your own project.
cp -r templates/new-game ../mygame        # Windows cmd: xcopy /E /I templates\new-game ..\mygame
#    Then edit:
#      ../mygame/CMakeLists.txt  -> project name, XBOXRECOMP_DIR path
#      ../mygame/src/main.c      -> YOUR_GAME_ENTRY_POINT / XBE path from step 3

# 8. Lift to C — the big one
#    --gen-dir writes the generated code into your game project, where the
#    template's CMakeLists globs src/recomp/gen/*.c. Without it the output
#    lands in this repo (src/game/recomp/gen/) and nothing compiles it.
py -3 -m tools.recomp game_files/default.xbe --all --split 250 --gen-dir ../mygame/src/recomp/gen
#    Output: recomp_0000.c ... recomp_dispatch.c, recomp_funcs.h (millions of
#    lines of C), plus recomp_types.h — the runtime register model the
#    generated code includes. You do not supply that one; if the build says
#    "Cannot open include file: 'recomp_types.h'", this step did not finish.

# 9. Build and run — from the game project, not from xboxrecomp
cd ../mygame
cmake -S . -B build
cmake --build build --config Release
build\Release\your_game_recomp.exe          # named after project() in your CMakeLists
```

### An optional front end

[xboxrecomp-cli](https://github.com/apexJCL/xboxrecomp-cli) (`xbr`, by
[@apexJCL](https://github.com/apexJCL)) wraps this pipeline for game repos.
A game repo describes itself in one `game.toml`, and the CLI provides pinned
toolchains, cross-builds, packaging and a player build wizard. It is
maintained outside this repo and tracks its tags. `regen.sh` and the steps
above stay the documented route (#223).

### What To Expect

The first time you run a recompiled game, **it will crash**. That's normal. The process is iterative:

1. **Boot** — get past the entry point (usually straightforward)
2. **Stub** — identify and stub out functions that touch hardware you haven't implemented yet
3. **Fix ICALLs** — indirect calls (vtable dispatches, function pointers) are the hardest 10%
4. **Add runtime** — implement kernel functions, D3D calls, and input as the game needs them
5. **Debug** — use the ICALL trace ring buffer, memory access logging, and your debugger
6. **Iterate** — each crash teaches you something about the game. Fix it and move on.

With Burnout 3 (the first game recompiled with this toolkit), the process from "empty repo" to "game boots and renders textured 3D tracks" took about two weeks of iterative development.

## Repository Structure

```
xboxrecomp/
├── README.md                    # You are here
├── CMakeLists.txt               # Top-level build (builds all runtime libs)
├── tools/                       # The recompilation toolchain (Python)
│   ├── xbe_parser/              # XBE file format parser
│   ├── disasm/                  # x86 disassembler + function detector
│   ├── func_id/                 # Library function identifier
│   ├── abi_analysis/            # Calling convention / param recovery
│   ├── recomp/                  # x86 -> C static recompiler
│   ├── debug_symbols/           # Debug-build symbol recovery
│   ├── symbols/ ghidra_naming/  # Optional symbol-name recovery (Ghidra)
│   ├── ida_naming/              # ... or the same thing through IDA
│   ├── xiso/ xmv/               # Disc image and video container tools
│   ├── conformance/             # Differential tests: lifted C vs the real CPU
│   ├── kernel_audit/            # Kernel import coverage per title
│   ├── linux/ macos/            # Host setup scripts
│   └── fusion/                  # MS Ficl/Fission study tooling
├── src/                         # Runtime libraries (C, link-time)
│   ├── platform/                # Win32 vocabulary + its POSIX implementation
│   ├── kernel/                  # xbox_kernel - Xbox kernel → Win32
│   ├── d3d/                     # xbox_d3d8   - D3D8 → D3D11 graphics
│   ├── audio/                   # xbox_dsound - DirectSound compat
│   ├── apu/                     # xbox_apu    - MCPX APU emulation (xemu)
│   ├── nv2a/                    # xbox_nv2a   - NV2A GPU emulation (xemu)
│   ├── input/                   # xbox_input  - Gamepad → XInput
│   ├── usb/                     # xbox_usb    - MCPX OHCI controllers
│   └── video/                   # xbox_video  - FMV playback + framebuffer window
├── include/xbox/                # Public umbrella header (xboxrecomp.h)
├── tests/                       # Runtime C tests, one CMake project each
├── templates/                   # Starter templates for new projects
│   ├── new-game/                # ** Copy this to start a game project **
│   │   ├── CMakeLists.txt       # Builds the game .exe, links xboxrecomp
│   │   └── src/main.c           # Host entry point: loads XBE, boots guest
│   └── runtime/                 # Runtime shim templates
│       ├── recomp_types.h       # Register model + ICALL macros
│       ├── xbox_memory.h        # Memory layout helpers
│       └── kernel_stubs.h       # Kernel function stub templates
└── docs/                        # Documentation
    ├── pipeline/                # Step-by-step pipeline guides
    ├── technical/               # Deep technical documentation
    ├── formats/                 # Xbox file format references
    └── runtime/                 # Runtime implementation guides
```

## Documentation

### Start Here
- **[Getting Started Guide](../../docs/GETTING_STARTED.md)** — End-to-end walkthrough from XBE to running game
- **[Documentation Index](../../docs/INDEX.md)** — Every document in one place, and the same pages again by symptom
- **[Decompilation Guide](../../docs/DECOMP.md)** — Using this as a function splitter instead: one byte-exact `.s` per function, with signatures and the call graph. You never run the recompiler
- **[Tools Reference](../../tools/README.md)** — Detailed usage for every pipeline tool
- **[Runtime Libraries](../../src/README.md)** — Architecture, build instructions, integration guide

### Per-Module API Reference
- [xbox_kernel](../../src/kernel/README.md) — Memory layout, file I/O, threading, sync, crypto, EEPROM, SMBus
- [xbox_d3d8](../../src/d3d/README.md) — D3D8 interface, register combiners, vertex shaders, texture unswizzle
- [xbox_dsound](../../src/audio/README.md) — DirectSound buffers, 3D audio, mixbins
- [xbox_apu](../../src/apu/README.md) — MCPX APU voice processor, mixer, MMIO
- [xbox_nv2a](../../src/nv2a/README.md) — NV2A GPU registers, push buffer, PGRAPH→D3D11
- [xbox_input](../../src/input/README.md) — Gamepad state, vibration, button mapping

### Pipeline Guides
- [Extracting and Parsing XBE Files](../../docs/pipeline/01-xbe-parsing.md)
- [Disassembly and Function Detection](../../docs/pipeline/02-disassembly.md)
- [Function Identification](../../docs/pipeline/03-function-id.md)
- [x86 to C Lifting](../../docs/pipeline/04-lifting.md)
- [Building the Runtime](../../docs/pipeline/05-runtime.md)
- [Iterative Debugging](../../docs/pipeline/06-debugging.md)

### Technical Deep Dives
- [The Register Model](../../docs/technical/register-model.md) — Why global registers work and how the stack is simulated
- [Memory Layout Reproduction](../../docs/technical/memory-layout.md) — CreateFileMapping, mirror views, and address space tricks
- [Indirect Call Dispatch](../../docs/technical/indirect-calls.md) — The RECOMP_ICALL problem and how to solve it
- [D3D8 to D3D11 Translation](../../docs/technical/d3d-translation.md) — Bridging Xbox's graphics API to modern DirectX
- [NV2A Shader Translation](../../docs/technical/nv2a-shaders.md) — Register combiners and vertex microcode to HLSL
- [D3D8LTCG Device Context](../../docs/technical/d3d8ltcg-device-context.md) — Device field map, PB ring management, stub calling conventions
- [Xbox Kernel Replacement](../../docs/technical/kernel-replacement.md) — Mapping Xbox kernel ordinals to Win32
- [SEH and Exception Handling](../../docs/technical/seh-handling.md) — Structured exception handling in recompiled code
- [Lessons Learned](../../docs/technical/lessons-learned.md) — What worked, what didn't, mistakes to avoid
- [Gap Analysis vs xemu](../../docs/technical/gap-analysis.md) — What's implemented, what's missing, prioritized roadmap
- [Pushbuffer Executor](../../docs/technical/pushbuffer-executor.md) — Executing a statically linked XDK D3D title's pushbuffer: DMA-engine walk, answering the title, fixed-function transform and lighting, anti-aliased surfaces, render back ends
- [Microsoft's Own Recompiler](../../docs/technical/ms-fusion-recompiler.md) — White-room analysis of Ficl/Fission: pipeline, address map, HLE boundary
- [Ficl/Fission Codegen Teardown](../../docs/technical/ms-fusion-codegen-teardown.md) — IDA/Hex-Rays teardown of both their translators, and how it reframes our roadmap
- [SVOD Extraction](../../docs/technical/svod-extraction.md) — reading the BC package container to get the donor title's guest XBE out, and the validation gate that catches a plausible-looking bad extraction
- [Burnout 3 Reunification](../../docs/technical/burnout3-reunification.md) — bringing the origin title back onto the extracted toolkit: what's done, and the threading gate that makes the runtime a merge not a swap

### Xbox Formats
- [XBE File Format](../../docs/formats/xbe.md) — Xbox executable format reference
- [Xbox Kernel Exports](../../docs/formats/kernel-exports.md) — All 366 kernel functions documented

## How It Works

The interesting parts each have their own document rather than a summary here,
so there is one place to keep correct:

- **[The Register Model](../../docs/technical/register-model.md)** — why the guest
  registers are globals (and thread-local), how the guest stack is simulated,
  and why every recompiled function is `void f(void)`.
- **[Memory Layout](../../docs/technical/memory-layout.md)** — reproducing the Xbox
  address space with `CreateFileMapping` + 28 mirror views, and why
  `VirtualAlloc` cannot do it (mirrors must alias the same physical pages, not
  copy them).
- **[Indirect Call Dispatch](../../docs/technical/indirect-calls.md)** — `call [eax+0x10]`
  with no compile-time target. The hardest part of any bring-up.
- **[NV2A Shader Translation](../../docs/technical/nv2a-shaders.md)** — register
  combiners and vertex microcode to HLSL, both translated at runtime and cached.
- **[SEH and Exception Handling](../../docs/technical/seh-handling.md)** — how
  `__SEH_prolog`/`__SEH_epilog` are detected per title and bridged.

## Games That Work Well As Targets

Based on our experience with Burnout 3, the best candidates for Xbox static recomp share these traits:

| Factor | Easier | Harder |
|--------|--------|--------|
| **Engine** | RenderWare (shared patterns) | Custom engine (unique quirks) |
| **Threading** | Single-threaded | Multi-threaded with sync |
| **GPU usage** | Standard D3D8 calls | NV2A push buffer microcode |
| **Code size** | Small .text section | Large with LTCG |
| **Online** | Offline only | Xbox Live dependent |
| **PC port** | No PC version (worth the effort!) | Good PC port exists |

See [docs/technical/candidate-games.md](../../docs/technical/candidate-games.md) for a detailed list of promising targets.

## Projects Using This Toolkit

Ports built on xboxrecomp, by sp00nznet and the community. The version column is
the xboxrecomp release each project's toolkit copy is based on; "fork" means the
project carries its own patches on top. Status comes from each project's own
README (October 2026): **Playable** means you can play the game; **Booting** means
the game's own code runs but does not reach gameplay yet; **Early** means the code
is lifted to C but does not run yet. Follow the link for the full picture.

Missing a project, or is a row out of date? Open a pull request that edits this table.

| Game | Creator | Repository | xboxrecomp version | Platforms | Status |
|------|---------|------------|--------------------|-----------|--------|
| 007: Nightfire | [@limeylicious](https://github.com/limeylicious) | [limeylicious/Nightfire-Windows-Port](https://github.com/limeylicious/Nightfire-Windows-Port) | v0.8 | Windows | Playable |
| Amped 2 | [@bertibertig](https://github.com/bertibertig) | [bertibertig/Amped2-Recomp](https://github.com/bertibertig/Amped2-Recomp) | v0.14.0 (fork) | Linux; Windows experimental | Playable |
| BLiNX 2: Masters of Time & Space | [@apexJCL](https://github.com/apexJCL) | [apexJCL/blinx2-recomp](https://github.com/apexJCL/blinx2-recomp) | v0.14.0 (fork) | macOS, Linux / Steam Deck (Proton), Windows (untested) | Playable |
| Blood Wake | [@sp00nznet](https://github.com/sp00nznet) | [sp00nznet/bloodwake](https://github.com/sp00nznet/bloodwake) | main | Windows | Early |
| Burnout 3: Takedown | [@sp00nznet](https://github.com/sp00nznet) | [sp00nznet/burnout3](https://github.com/sp00nznet/burnout3) | v0.12.0 | Windows | Playable |
| Dead or Alive Xtreme Beach Volleyball | [@NoRain211](https://github.com/NoRain211) | [NoRain211/doaxvb-re](https://github.com/NoRain211/doaxvb-re) | v0.5 (fork) | Windows, macOS, Linux | Playable |
| Def Jam: Fight for NY | [@vyanhursky](https://github.com/vyanhursky) | [vyanhursky/DefJamFFNY-recomp](https://github.com/vyanhursky/DefJamFFNY-recomp) | v0.16.0 (fork) | Windows, macOS, Linux / Steam Deck | Playable |
| Jet Set Radio Future | [@andeecollard](https://github.com/andeecollard) | [andeecollard/jsrf-recomp](https://github.com/andeecollard/jsrf-recomp) | v0.8.0 (fork) | macOS (Apple Silicon); Windows experimental | Playable |
| Kung Fu Chaos | [@maxiumpro](https://github.com/maxiumpro) | [maxiumpro/kungfuchaos-recomp](https://github.com/maxiumpro/kungfuchaos-recomp) | fork | Windows | Playable |
| Mortal Kombat: Deadly Alliance | [@FOTLabs](https://github.com/FOTLabs) | [FOTLabs/mkda-nx](https://github.com/FOTLabs/mkda-nx) | v0.12.0 (fork) | Nintendo Switch | Playable |
| SSX 3 | [@koenhendriks](https://github.com/koenhendriks) | [koenhendriks/SSX3-recomp](https://github.com/koenhendriks/SSX3-recomp) | v0.13.1 (fork) | Windows, Linux | Playable |
| SSX 3 | [@bertibertig](https://github.com/bertibertig) | [bertibertig/SSX3-Recomp](https://github.com/bertibertig/SSX3-Recomp) | v0.14.0 (fork) | Linux; Windows experimental | Playable |
| SSX Tricky | [@MatiasRiveraC](https://github.com/MatiasRiveraC) | [MatiasRiveraC/SSX-Tricky-PC](https://github.com/MatiasRiveraC/SSX-Tricky-PC) | v0.5 (fork) | Windows | Playable |
| StarCraft: Ghost | [@aknavj](https://github.com/aknavj) | [aknavj/scghost-recomp](https://github.com/aknavj/scghost-recomp) | v0.12.0 (fork) | Windows | Playable |
| Wreckless: The Yakuza Missions | [@sp00nznet](https://github.com/sp00nznet) | [sp00nznet/wreckless](https://github.com/sp00nznet/wreckless) | main | Windows | Booting |
| Xbox Dashboard (build 3944) | [@sp00nznet](https://github.com/sp00nznet) | [sp00nznet/xboxdashboard](https://github.com/sp00nznet/xboxdashboard) | main | Windows | Booting |

## How You Can Help

This is an emerging field. Here's how you can contribute:

1. **Try it on a new game** — Pick an Xbox exclusive, follow the pipeline, and see how far you get. Even partial results teach us about the toolchain's gaps.
2. **Improve the lifter** — Coverage is good but unquantified; the honest signal is that an unhandled instruction lifts to a bare `/* mnemonic */` comment, so grepping generated output for those finds the gaps. Segment prefixes and the rarer x87/SSE forms are where they cluster.
3. **Document Xbox formats** — Every game has its own asset formats. Document what you discover.
4. **Build runtime components** — Better D3D8 emulation, audio, networking — the runtime layer is where most per-game work happens.
5. **Share your findings** — Write up what you learn. The Xbox modding/preservation community benefits from every discovery.

Not sure where to start, or want to sanity-check an idea first? Ask in the
[Discord](https://discord.gg/CRpzGWZFcu) — several of the people working on
ports and on the lifter are there.

## Dependencies

The toolchain is intentionally lightweight:

```
Python 3.10+
capstone        # x86 disassembly  (pip install capstone)
pytest          # test suite only  (pip install pytest)
```

That's it for the core pipeline — no IDA, no Ghidra, no proprietary tools. Just the standard library + Capstone. (Optional `tools/ghidra_naming` and `tools/ida_naming` helpers use headless Ghidra or IDA purely to recover symbol names; neither is ever required to produce a working build.)

### Running the tests

```
py -3 -m pytest tools/       # unit tests
py -3 -m tools.conformance   # differential: lifted C vs the real CPU
```

Several unit tests compile the lifter's own output and sweep it against x86's
definitions — they are the real proof for the shift, flag and x87 work, and
each is paired with a negative control that feeds the harness the pre-fix
expression and requires it to fail. They need a C compiler on `PATH`, and
**skip rather than fail without one**, so check the skip count: a clean run is
988 passed / 1 skipped (a POSIX-only test). If clang is installed but not on `PATH`:

```bash
export PATH="/c/Program Files/LLVM/bin:$PATH"   # Git Bash
```

The unit tests are fast and need no game files. The conformance suite goes
further: it assembles each snippet, lifts the resulting bytes, then runs the
lifted C *and the original instructions* over the same inputs and requires them
to agree. The CPU executing those instructions is the oracle — no model to be
wrong. See [Conformance Testing](../../docs/technical/conformance-testing.md).

### Running the tests (Linux and macOS)

That oracle has to be 32-bit x86. On Windows a 32-bit MSVC supplies one.
Everywhere else Docker containers stand in for the toolchain while the lifting stays on
the host. Run the setup once:

```bash
bash tools/macos/setup.sh --test        # adds the images (~2.2 GB, ~10 min)
```

After that **the suite runs exactly as it does on Windows** — the commands below
are the commands, no platform-specific runner:

```bash
py -3 -m tools.conformance                  # snippets + corpus
py -3 -m tools.conformance --only snippets
py -3 -m tools.conformance --xbe tools/conformance/test.xbe
```

| image | phase | cost |
|---|---|---|
| `xboxrecomp-gcc-i386` | snippets | ~280 MB, under a minute |
| `xboxrecomp-msvc-amd64` | corpus, XBE — compiles and links | ~975 MB, ~5 min |
| `xboxrecomp-msvc-wine` | corpus, XBE — runs the 32-bit harnesses | ~985 MB, ~4 min |

The snippet image is GCC and builds in seconds. The other two carry MSVC under
Wine and each download ~1.5 GB from Microsoft, so setup says what it is about to
do before building them. See
[tools/conformance/msvc-wine/](../../tools/conformance/msvc-wine/) for why corpus and
XBE need the real MSVC where snippets do not, and why it takes two images.

If you fix a lift, add the case.

The runtime's own C tests live in `tests/`, one CMake project each. On a Linux
host, [tests/proton_run.sh](../../tests/proton_run.sh) runs the mingw-built Windows
ones under Proton as ctest's emulator; its header has the commands.

The runtime libraries (C) use:
- MSVC (Visual Studio 2022) or MinGW-w64 on Windows; Clang or GCC on Linux/macOS
- Windows SDK (D3D11, DXGI, XInput, XAudio2, waveOut, Media Foundation)
- Linux/macOS: SDL2, libepoxy, pkg-config
- CMake 3.20+
- No other external dependencies — all hardware emulation code is self-contained

## FAQ

**Q: Is this legal?**
A: This project provides tools and documentation. You must own a legitimate copy of any game you recompile. No copyrighted game code or assets are included in this repository.

**Q: How is this different from an emulator?**
A: Emulators interpret or JIT-compile code at runtime. Static recompilation translates the entire binary ahead of time into native C code that compiles to a regular `.exe`. There's no CPU emulation at runtime — the recompiled functions execute directly.

**Q: Can I use this on Xbox 360 games?**
A: No. Xbox 360 uses PowerPC (big-endian, different ISA). See [XenonRecomp](https://github.com/hedge-dev/XenonRecomp) for Xbox 360 static recompilation. This toolkit is specifically for the original Xbox's x86 code.

**Q: How long does it take to get a game running?**
A: It depends on the game's complexity. Burnout 3 went from zero to "boots and renders 3D tracks" in about two weeks. Simple games might be faster; complex ones with custom engines could take longer. The toolchain handles the mechanical translation — the real work is building the runtime shims and debugging indirect calls.

**Q: Why C output instead of direct x86-64 binary translation?**
A: C is portable, debuggable, and the compiler optimizes it for you. You can read the output, set breakpoints in it, and modify individual functions. Direct binary translation would be faster to run but impossible to debug or modify.

## License

**MIT** — see [LICENSE](../../LICENSE). Third-party components keep their original
licence:

| Component | Licence | Copyright |
|---|---|---|
| the MCPX APU sources in `src/apu/` | LGPL-2.1-or-later | espes; Jannik Vogel; Matt Borgerson |
| `src/nv2a/nv2a_regs.h` | LGPL-2.1-or-later | espes; Jannik Vogel |
| everything else | MIT | sp00nz and contributors |

The APU and the NV2A register definitions were extracted from
[xemu](https://github.com/xemu-project/xemu) and are that project's work, not
ours. LGPL-2.1 expressly permits linking them from MIT or proprietary code, so
a recompiled game is unaffected; what it asks is that the notices stay, the
source stays available, and users can relink against a modified library.
[LICENSES/LGPL-2.1.txt](../../LICENSES/LGPL-2.1.txt) is the verbatim licence text —
shipping it alongside those files is a requirement, not a courtesy.

Not every file under `src/apu/` and `src/nv2a/` is xemu-derived. See
[NOTICE](../../NOTICE) for the exact list, each with the copyright it actually
carries — including algorithms we implemented ourselves but learned from xemu,
credited there even where no licence obligation attaches.

## Contributors

xboxrecomp is built by more than one person. See
**[CONTRIBUTORS.md](../../CONTRIBUTORS.md)** for who did what — including the people
who never sent a patch and still moved the project further than a patch would
have, by finding the wall everyone else was about to hit.

Thank you, all of you.

## Credits

Built with [Claude Code](https://claude.ai) (Anthropic) — proving that AI-assisted systems programming can tackle problems previously considered impractical.

Human contributors are credited in [CONTRIBUTORS.md](../../CONTRIBUTORS.md); the
third-party code we build on is credited in [NOTICE](../../NOTICE).

## References

- [XBE File Format](https://xboxdevwiki.net/Xbe) — Xbox Dev Wiki
- [Xbox Kernel Exports](https://xboxdevwiki.net/Kernel) — Xbox Dev Wiki
- [NV2A GPU](https://xboxdevwiki.net/NV2A) — Xbox GPU documentation
- [Xbox Architecture](https://www.copetti.org/writings/consoles/xbox/) — Copetti's deep dive
- [N64Recomp](https://github.com/N64Recomp/N64Recomp) — Static recomp for N64 (MIPS→C)
- [XenonRecomp](https://github.com/hedge-dev/XenonRecomp) — Static recomp for Xbox 360 (PPC→C)
- [RexGlueSDK](https://github.com/rexglue/rexglue-sdk) — Xbox 360 recomp runtime (Xenia as link-time library)
- [Cxbx-Reloaded](https://github.com/Cxbx-Reloaded/Cxbx-Reloaded) — Xbox emulator (dynamic recomp)
- [xemu](https://github.com/xemu-project/xemu) — Xbox emulator (LLE)
