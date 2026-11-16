# xbox_apu — MCPX APU Audio Emulation

Hardware emulation of the Xbox's MCPX APU (Audio Processing Unit), extracted from [xemu](https://github.com/xemu-project/xemu). The APU is a custom chip with three processors:

- **VP (Voice Processor)** — 256 hardware voices, ADPCM/PCM decode, pitch shifting, envelopes, HRTF 3D audio
- **GP (Global Processor)** — DSP for global effects (reverb, chorus). Not emulated: the mixbins are folded to
  stereo by role (`apu_mix.c`), with an optional native I3DL2 reverb (`RECOMP_APU_REVERB`), and what the title asks
  of the GP is recorded (`apu_dsp_watch.c`). [docs/technical/apu-dsp.md](../../docs/technical/apu-dsp.md).
- **EP (Encode Processor)** — DSP for AC3/DTS encoding. Not emulated: the output is PCM stereo whatever EP image the
  title loads; its canary is kept so DirectSound sees it alive.

Audio output uses Windows `waveOut` at 48kHz stereo 16-bit.

## Files

| File | LOC | Purpose |
|------|-----|---------|
| `apu.h` | 70 | **Public API** — init, shutdown, MMIO, mixer voices |
| `apu_state.h` | 525 | APU state struct (VP, GP, EP, DSP core, 256 voices) |
| `apu_regs.h` | 366 | MCPX APU register definitions and field masks |
| `apu_core.c` | 762 | MMIO handlers, frame thread, waveOut output, test tone |
| `apu_vp.c` | 1,247 | Voice Processor — decode, resample, mix, envelopes, HRTF |
| `apu_dsp.c` | 420 | GP/EP stand-in: DSP doorbell ack, EP canary, calls the mixdown and the watch |
| `apu_mix.c` | 380 | Mixbin downmix by role, native I3DL2 reverb (this project's code, MIT) |
| `apu_dsp_watch.c` | 310 | What the title asks of the GP/EP: commands, images, mixbins, to the log and the status file (MIT) |
| `apu_mmio_hook.c` | 253 | VEH x86-64 instruction decoder for 0xFE800000+ MMIO |
| `apu_shim.h` | 431 | QEMU compatibility (threads, atomics, clocks → Win32) |
| `apu_debug.h` | 104 | Debug logging macros |
| `fpconv.h` | 70 | IEEE 754 float/int conversion utilities |

## Quick Start

```c
#include "apu.h"

// Initialize APU with pointer to Xbox RAM
// (ram_ptr = native pointer to Xbox VA 0x00000000)
MCPXAPUState *apu = mcpx_apu_init_standalone(ram_ptr);

// Test: play a 440Hz sine tone
mcpx_apu_play_test_tone(apu);

// --- OR use the software mixer for direct PCM playback ---

// Allocate a mixer voice
int voice = apu_mixer_alloc_voice();
APUMixerVoice *v = apu_mixer_get_voice(voice);

// Configure it
v->pcm_data = my_pcm_buffer;     // 16-bit signed PCM
v->pcm_bytes = buffer_size;
v->num_channels = 2;              // 1=mono, 2=stereo
v->sample_rate = 44100;           // Source sample rate
v->volume = 0.8f;                 // 0.0 - 1.0

// Play
apu_mixer_play(voice, 1);  // 1 = looping

// Stop and free
apu_mixer_stop(voice);
apu_mixer_free_voice(voice);

// Shutdown
mcpx_apu_shutdown(apu);
```

## Architecture

```
┌─────────────────────────────────────────────────┐
│                    Xbox Game                     │
│  DirectSound API calls (IDirectSound8)           │
└────────────────────┬────────────────────────────┘
                     │ writes to MMIO registers
                     ▼
┌─────────────────────────────────────────────────┐
│              APU Register Space                  │
│  0xFE800000 - 0xFE87FFFF (512 KB)               │
│  Intercepted via VEH (apu_mmio_hook.c)           │
└────────┬───────────┬───────────┬────────────────┘
         │           │           │
         ▼           ▼           ▼
   ┌──────────┐ ┌─────────┐ ┌─────────┐
   │    VP    │ │   GP    │ │   EP    │
   │256 voices│ │  (stub) │ │  (stub) │
   │ ADPCM/  │ │  effects│ │  encode │
   │  PCM    │ │  bypass │ │  bypass │
   └────┬─────┘ └────┬────┘ └────┬────┘
        │             │           │
        └──────┬──────┘───────────┘
               │ mixed samples
               ▼
   ┌──────────────────────────────┐
   │    waveOut (48kHz stereo)    │
   │  4 × 2048-sample buffers    │
   └──────────────────────────────┘
```

## Two Audio Paths

### 1. Hardware APU Emulation (MMIO)

For games that program the APU directly. The game writes to MMIO registers at 0xFE800000+, which are intercepted by the VEH hook and routed to the APU emulation:

```c
// Game writes: voice 5, set buffer address
*(uint32_t*)(0xFE802000 + 5 * voice_stride + VOICE_BUF_OFFSET) = buffer_addr;

// VEH intercepts → mcpx_apu_mmio_write() → VP processes voice
```

The VP runs in a separate thread, mixing active voices at the hardware frame rate.

### The frame thread

`mcpx_apu_frame_thread` (`apu_core.c`) renders one 32-sample frame per pass
(0.67 ms of audio) and hands the device a 256-sample EP period every eight.
It holds the APU lock (`d->lock`) for its whole loop and drops it in
`throttle()`'s wait and once at the end of every frame:

- **Lock hand-over only for waiters.** Every other thread that takes
  `d->lock` (the software mixer's play, the test tone, init, shutdown) goes
  through `mcpx_apu_lock_guest()`, which counts waiters. At the end of a frame
  the thread yields the lock only while that count is non-zero, and then until
  a waiter has had it (at most 2 ms). An unconditional `SwitchToThread()` on
  every frame cost a whole scheduler quantum per frame once the host's cores
  were busy, and the 43 ms device queue ran dry (Fable, 12 busy processes:
  13 underruns in 20 s). The hand-over itself is still needed: a critical
  section is not fair, and Burnout 3 froze on its vehicle select while its
  thread starved on the lock. The MMIO entry points do not take `d->lock`
  (registers are atomics, VOICE_LOCK is a per-voice spin lock); a new site
  that takes it must use `mcpx_apu_lock_guest()`. Test:
  `tests/apu_lock_contention`.
- **Audio priority.** Linux and Android: `setpriority(-16)` on the thread
  (Android's `THREAD_PRIORITY_AUDIO`; refused without `CAP_SYS_NICE` on
  desktop Linux, which is ignored). macOS: QoS user-interactive. The first
  line of the thread logs the outcome (`[APU] frame thread nice -16: ...`);
  `RECOMP_APU_PRIORITY=0` keeps the default priority.
- **Audio health, always on.** Each period that reaches the device reads the
  queue first (so an underrun that began partway through a period, during a
  trap, is counted), then the clock; trap spells read the clock when they
  begin and end; the VP counts persistent stream voices with nothing to play.
  The `[APU] health` line comes out every 10 s when something went wrong,
  else once a minute ([debugging](../../docs/pipeline/06-debugging.md#audio-diagnostics)).
  Test: `tests/apu_health`.

### 2. Software Mixer (Direct API)

For games where you've decoded the audio format yourself and just want to play PCM. The mixer provides 64 voice slots with:

- 16-bit signed PCM input (mono or stereo)
- 16.16 fixed-point resampling (any rate → 48kHz)
- Per-voice volume control
- Looping support

```c
int apu_mixer_alloc_voice(void);                    // Returns slot 0-63, or -1
void apu_mixer_free_voice(int slot);
APUMixerVoice *apu_mixer_get_voice(int slot);       // Configure voice params
void apu_mixer_play(int slot, int looping);          // Start playback
void apu_mixer_stop(int slot);                       // Stop playback
```

## MMIO Hook

The APU MMIO hook (`apu_mmio_hook.c`) uses Windows Vectored Exception Handling to intercept memory accesses to the APU register range:

```
Access to 0xFE800000+
  → Page fault (no physical memory there)
  → VEH handler catches EXCEPTION_ACCESS_VIOLATION
  → Decodes the faulting x86-64 instruction (MOV, ADD, XOR, etc.)
  → Routes to mcpx_apu_mmio_read() or mcpx_apu_mmio_write()
  → Advances RIP past the instruction
  → Resumes execution
```

This allows recompiled code to access APU registers using normal memory operations, exactly as the original Xbox code did.

## Voice Processor Details

The VP processes 256 hardware voices per frame:

- **ADPCM decode**: Xbox ADPCM (4-bit, 64-sample blocks)
- **PCM formats**: 8-bit unsigned, 16-bit signed, 24-bit
- **Pitch**: Log2 fixed-point (4.12 format). Pitch 0 = 48kHz native rate.
- **Envelopes**: Multi-segment amplitude envelopes (attack, decay, sustain, release)
- **HRTF**: Head-Related Transfer Function for 3D positional audio
- **Mixbins**: 32 output channels, voices route to any combination
- **Filters**: Per-voice low-pass/high-pass (IIR biquad)

## Register Map (Key Registers)

```
Offset      Name                   Description
──────────────────────────────────────────────────
0x1000      NV_PAPU_ISTS           Interrupt status
0x1100      NV_PAPU_FECTL          Frontend control
0x2000      NV_PAPU_SECTL          Synth engine control
0x202C      NV_PAPU_VPVADDR        VP voice address base
0x3000+     NV_PAPU_GP*            Global Processor registers
0x4000+     NV_PAPU_EP*            Encode Processor registers
```

Full register definitions in `apu_regs.h`.

## Dependencies

- `qemu_shim.h` (from `../nv2a/`) — QEMU type abstraction layer
- `kernel32.lib` — Win32 threading
- `winmm.lib` — waveOut audio output
