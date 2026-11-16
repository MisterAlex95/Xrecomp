/**
 * YOUR_GAME_NAME - Recompiled Game Entry Point
 *
 * This is the Windows executable that hosts the recompiled game code.
 * It performs the following initialization sequence:
 *
 * 1. Load the original XBE file from disk
 * 2. Initialize the Xbox memory layout (map data sections to original VAs)
 * 3. Initialize the Xbox kernel replacement layer
 * 4. Initialize the kernel bridge (thunk table in Xbox memory)
 * 5. Set up game file paths for I/O redirection
 * 6. Initialize the stack pointer
 * 7. Install VEH crash handler for diagnostics
 * 8. Call the game's original entry point (recompiled)
 *
 * Customize this file for your game:
 *   - Set YOUR_GAME_ENTRY_POINT to the XBE entry point address
 *   - Set YOUR_GAME_XBE_PATH to where the XBE file lives
 *   - Set YOUR_GAME_DIR to the game data directory
 *     (on Android both come from the app's storage instead, see below)
 *   - Add any CRT global pre-initialization your game needs
 *   - Customize the VEH handler for game-specific crash diagnosis
 *
 * XBE Details (fill in from xbe_parser output):
 *   Title:       YOUR_GAME_NAME
 *   Title ID:    0x00000000
 *   Base addr:   0x00010000
 *   Entry point: 0x00000000
 *   Code size:   ~??? KB (.text)
 *   Sections:    ?? (list them)
 *   Kernel imports: ??
 */

#ifdef _WIN32
#include <windows.h>
#include <dbghelp.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>

/* xboxrecomp runtime headers */
#include <xbox/xboxrecomp.h>

/* The profile layers: profiles/ in the toolkit (engine, title; optional),
 * copied into src/title/ by tools.prepare with a generated xba_layers.c that
 * defines these two hooks and calls each layer's, lowest first
 * (docs/technical/profiles.md; CMakeLists defines XBA_TITLE_PROFILE when that
 * folder has sources). configure runs before the memory layout -- switches, map size --
 * and start right before the guest's entry point, with the kernel up; it gets
 * the game and save directories (save_dir NULL = the kernel's default). */
extern int recomp_xdk_defaults(void);
/* The XDK D3D service (src/kernel/recomp_d3d_service.h): D3D's device found at
 * run time, its GPU semaphore and software methods, the DMA walk. For every
 * title; one without D3D finds no device and it stays idle. */
extern int recomp_d3d_configure(void);
extern void recomp_d3d_start(const char *game_dir, const char *save_dir);
#ifdef XBA_TITLE_PROFILE
extern void xba_title_configure(void);
extern void xba_title_start(const char *game_dir, const char *save_dir);
#endif

/* Android (XBOXRECOMP_SHELL_TITLE, set when android/app builds this project
 * with XBA_TITLE_DIR): the app shell owns SDL_main. It does the start-up
 * (logcat, recomp.cfg, storage, fast cores, fault handler) and calls
 * recomp_title_main() below on the guest thread of its window, with the
 * directories it resolved: game_dir (D:\), save_dir, and the XBE as found
 * on disk. See docs/android/app-shell.md. */
#ifdef XBOXRECOMP_SHELL_TITLE
#include "platform/recomp_storage.h"
#endif

/* Options the title forces (title.json "env_force"): tools.prepare writes
 * them to src/title/env_force.h and CMakeLists defines XBA_TITLE_ENV_FORCE.
 * main() sets them over the environment before anything reads it -- the
 * frame generation pacer and the window start before xba_title_configure.
 * The Android app gets them from pack.json instead (recomp_pack.c). */
#if defined(XBA_TITLE_ENV_FORCE) && !defined(XBOXRECOMP_SHELL_TITLE)
#include "title/env_force.h"
#include "platform/recomp_cfg.h"
#endif

/*
 * If xboxrecomp.h is not an umbrella header in your setup, include
 * the individual headers directly:
 *
 * #include "kernel.h"
 * #include "xbox_memory_layout.h"
 * #include "d3d8_xbox.h"
 * #include "dsound_xbox.h"
 * #include "xinput_xbox.h"
 */

/* ── Global register state (defined in xbox_memory_layout.c) ── */

/* The guest registers (g_eax, g_esp, g_xmm0, ...) are fields of one
 * per-thread struct, g_cpu, and xbox_memory_layout.h (through xboxrecomp.h)
 * names each of them with a macro. Do not declare them here: a line such as
 * `extern RECOMP_TLS uint32_t g_esp;` no longer compiles. */
extern ptrdiff_t g_xbox_mem_offset;

/* ── XBE Constants ─────────────────────────────────────────── */

/*
 * TODO: Set these from your xbe_parser output.
 * Run: py -3 -m tools.xbe_parser game/default.xbe
 */
#define YOUR_GAME_ENTRY_POINT   0x00000000  /* XBE entry point VA */
#define YOUR_GAME_XBE_PATH      "game\\Your Game Title\\default.xbe"
#define YOUR_GAME_DIR            "game\\Your Game Title"

/* What the boot below uses: the constants above, or on Android the paths
 * recomp_title_main() was handed. save_dir NULL = the kernel's default. */
static const char *s_xbe_path = YOUR_GAME_XBE_PATH;
static const char *s_game_dir = YOUR_GAME_DIR;
static const char *s_save_dir = NULL;

/* ── Forward declarations ──────────────────────────────────── */

static BOOL load_xbe(const char *path, void **out_data, size_t *out_size);

/* Recompiled entry point (generated by recomp pipeline) */
extern void xbe_entry_point(void);

/* Declared in recomp_types.h, which this file does not include. MSVC accepts
 * the implicit declaration; clang and GCC 14 reject it. */
extern int recomp_dispatch_init(void);

/* ── VEH crash handler ─────────────────────────────────────── */

/*
 * Vectored Exception Handler for crash diagnostics.
 *
 * When the recompiled game hits an access violation, this handler prints
 * the faulting address, all Xbox register values, and a native stack trace.
 * This is your primary debugging tool during bring-up.
 *
 * Customize this for your game:
 *   - Add game-specific address checks (GPU register probes, etc.)
 *   - Add dumps of game-specific globals (heap handles, state flags)
 *   - Add SEH simulation if your game uses __try/__except
 */
/* Name the guest function a fault happened in, and recover the call chain.
 *
 * Recompiled code faults as ordinary native code, so the exception record
 * carries a host RIP and nothing else -- there is no guest program counter to
 * report, and the host address changes every build. Two things recover the
 * guest view:
 *
 *   - every generated function is a real symbol in the image (sub_005A03C0
 *     and so on), so the linker's PDB already maps host address back to guest
 *     function. dbghelp turns an anonymous RIP into that name.
 *
 *   - every lifted call pushes its guest return address onto the guest stack
 *     before jumping, so the stack still holds the chain. Scanning up from esp
 *     for values inside the code sections recovers it.
 *
 * ponytail: the stack scan is a scan, not a frame walk -- these are FPO frames
 * with no reliable ebp chain, so there is nothing to walk. It over-reports,
 * since addresses from returned-from calls linger below esp, but naming the
 * guest function is the whole question at a fault.
 *
 * Requires linking dbghelp and keeping the .pdb beside the .exe.
 */
static void print_guest_context(void *rip)
{
    /* SYMBOL_INFO is variable-length: the name is written past the struct, so
     * it must be over-allocated with MaxNameLen set to the slack. */
#ifdef _WIN32
    char buf[sizeof(SYMBOL_INFO) + 256];
    SYMBOL_INFO *sym = (SYMBOL_INFO *)buf;
    DWORD64 disp = 0;

    memset(buf, 0, sizeof(buf));
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 255;
    if (SymFromAddr(GetCurrentProcess(), (DWORD64)(uintptr_t)rip, &disp, sym))
        fprintf(stderr, "  in %s+0x%llX\n",
                sym->Name, (unsigned long long)disp);
#else
    (void)rip;   /* no dbghelp: the guest stack scan below still names the chain */
#endif

    if (g_xbox_mem_offset && g_esp) {
        const uint32_t *sp =
            (const uint32_t *)((uintptr_t)g_xbox_mem_offset + g_esp);
        int shown = 0, i;
        fprintf(stderr, "  guest stack (return addresses, innermost first):\n");
        for (i = 0; i < 256 && shown < 24; i++) {
            uint32_t v = sp[i];
            if (v > g_xbox_code_lo && v < g_xbox_code_hi) {
                fprintf(stderr, "    [esp+%-4d] 0x%08X\n", i * 4, v);
                shown++;
            }
        }
        /* When the frame is gone, say so, and fall back on what is left.
         *
         * A fault inside a block copy leaves no return address in range: the
         * copy is a host memmove and its caller's frame is further up than
         * this reaches. Worse, a copy that ran past its buffer overwrites the
         * stack itself, so the list comes out empty exactly when it is most
         * needed -- and an empty list under a promising heading reads like
         * "nothing to see" rather than "the evidence is destroyed". */
        if (!shown)
            fprintf(stderr, "    (no return addresses in range)\n");

        /* Everything below only pays for itself when the scan came back
         * empty. A crash where the chain was recovered already says what
         * happened, and burying that under another twenty lines makes the
         * common report worse to read. */
        if (!shown) {
            uint32_t k;

            /* The indirect-call history, which the stack cannot overwrite.
             * recomp_manual.c prints this too, but only from
             * recomp_icall_fail_log -- a page fault inside a block copy
             * never reaches that path, and this is the same information by
             * the route the fault actually took.
             *
             * Declared here rather than included: main.c does not pull in
             * recomp_types.h, and recomp_manual.c spells it the same way. */
            extern volatile uint32_t g_icall_trace[16];
            extern volatile uint32_t g_icall_trace_idx;

            fprintf(stderr, "  recent ICALL targets:");
            for (k = 0; k < 16; k++)
                fprintf(stderr, " %08X",
                        g_icall_trace[(g_icall_trace_idx + k) & 15]);
            fprintf(stderr, "\n");

            /* The frame unfiltered. The pointers and lengths handed to
             * whatever faulted live here and look nothing like code, so the
             * filter above drops exactly what is wanted. On the crash this
             * was written for, one of these words was the end of the mapped
             * RAM mirror -- which is what identified the fault as a copy
             * running off its buffer rather than a stray pointer. */
            for (i = 0; i < 20; i++)
                fprintf(stderr, "    raw[esp+%-4d] 0x%08X\n", i * 4, sp[i]);
        }
    }
}

/* The emulated APU.
 *
 * Two entry points that had no callers anywhere in the tree. apu_mmio_hook.c
 * documents apu_hook_handle_mmio as "called from VEH in main.c" and no main.c
 * called it; apu_decode_and_handle's first line is `if (!g_apu_state) return
 * false` and nothing assigned g_apu_state. So with RECOMP_AC97_READY set the
 * APU's registers were unmapped to be trapped, and every trap was then
 * declined and surfaced as an access violation on the first register
 * DirectSound touched. The VEH below now reaches apu_hook_handle_mmio
 * through the toolkit's xbox_VehMmioFault.
 *
 * Declared rather than included so this does not depend on src/apu being on
 * the include path. */
typedef struct MCPXAPUState MCPXAPUState;
extern MCPXAPUState *mcpx_apu_init_standalone(uint8_t *ram_ptr);
extern MCPXAPUState *g_apu_state;

#ifdef _WIN32
static LONG CALLBACK veh_handler(PEXCEPTION_POINTERS ep)
{
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        uintptr_t fault_addr = ep->ExceptionRecord->ExceptionInformation[1];

        /*
         * GPU register probe at 0xFD000000 range.
         * Some games probe NV2A registers directly. On real hardware this
         * returns GPU state; here we just skip the instruction.
         * TODO: Implement mini x86-64 decoder for instruction skipping,
         * or connect to the xbox_nv2a library for proper handling.
         */
        if (fault_addr >= 0xFD000000 && fault_addr < 0xFE000000) {
            return EXCEPTION_CONTINUE_SEARCH;
        }

        /* Trapped hardware registers (APU, USB OHCI) -> their models.
         *
         * The other half of RECOMP_AC97_READY: reporting the codec ready is
         * what lets DirectSoundCreate past its first gate, and from there it
         * drives the APU directly, so the registers have to fault to be seen.
         * "Handled" means the access was decoded and the instruction stepped
         * over, so execution resumes instead of unwinding. */
        if (xbox_VehMmioFault(ep))
            return EXCEPTION_CONTINUE_EXECUTION;

        fprintf(stderr, "[CRASH] Access violation at RIP=0x%llX, fault addr=0x%llX (%s)\n",
            (unsigned long long)ep->ContextRecord->Rip,
            (unsigned long long)fault_addr,
            ep->ExceptionRecord->ExceptionInformation[0] ? "write" : "read");
        fprintf(stderr, "  Xbox regs: eax=0x%08X ecx=0x%08X edx=0x%08X esp=0x%08X\n",
            g_eax, g_ecx, g_edx, g_esp);
        fprintf(stderr, "  Xbox regs: ebx=0x%08X esi=0x%08X edi=0x%08X\n",
            g_ebx, g_esi, g_edi);
        fprintf(stderr, "  Xbox VA of fault: 0x%08X\n",
            (uint32_t)(fault_addr - (uintptr_t)g_xbox_mem_offset));
        print_guest_context((void *)ep->ContextRecord->Rip);

        /*
         * TODO: Add game-specific diagnostics here. Examples:
         *
         * Dump CRT heap handle:
         *   uint32_t heap = *(uint32_t *)((uint8_t *)g_xbox_mem_offset + HEAP_HANDLE_VA);
         *   fprintf(stderr, "  CRT heap handle: 0x%08X\n", heap);
         *
         * Dump game state:
         *   uint32_t state = *(uint32_t *)((uint8_t *)g_xbox_mem_offset + GAME_STATE_VA);
         *   fprintf(stderr, "  Game state: %u\n", state);
         */

        /* Print native stack return addresses for debugging */
        {
            uintptr_t *sp = (uintptr_t *)ep->ContextRecord->Rsp;
            fprintf(stderr, "  Native stack (first 8 return addrs):\n");
            for (int i = 0; i < 64 && sp[i]; i++) {
                if (sp[i] >= 0x140000000ULL && sp[i] < 0x150000000ULL) {
                    fprintf(stderr, "    [%d] 0x%llX\n", i, (unsigned long long)sp[i]);
                }
            }
        }
        fflush(stderr);
    }

    return EXCEPTION_CONTINUE_SEARCH;
}
#endif /* _WIN32 */

/* ── WinMain ───────────────────────────────────────────────── */

#ifdef _WIN32
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                   LPSTR lpCmdLine, int nCmdShow)
{
#else
static int game_main(void *hInstance, void *hPrevInstance, char *lpCmdLine,
                     int nCmdShow)
{
#endif
    void *xbe_data = NULL;
    size_t xbe_size = 0;

    (void)hInstance;
    (void)hPrevInstance;
    (void)lpCmdLine;
    (void)nCmdShow;

    /* Unbuffered output for immediate visibility during debugging */
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    printf("=== YOUR_GAME_NAME - Static Recompilation ===\n");
    printf("Loading XBE...\n");

    /* Install VEH handler (first handler in chain) */
    /* Load symbols up front rather than from inside the handler: at fault
     * time the process is already in a bad way, and SymInitialize
     * allocates. Failure is not fatal -- the handler prints no name. */
#ifdef _WIN32
    SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME);
    SymInitialize(GetCurrentProcess(), NULL, TRUE);
    AddVectoredExceptionHandler(1, veh_handler);
#else
    /* Linux, macOS, Android: the toolkit's SIGSEGV handler emulates trapped
     * MMIO (APU, AC'97, USB OHCI) and hands every other fault to the previous
     * action, so a real crash still dumps core / stops the debugger. */
    if (xbox_PosixInstallMmioFaultHandler() != 0)
        fprintf(stderr, "[BOOT] could not install the MMIO fault handler\n");
#endif

    /* Step 1: Load XBE */
    if (!load_xbe(s_xbe_path, &xbe_data, &xbe_size)) {
        MessageBoxA(NULL, "Failed to load default.xbe.\n"
                    "Place the game files in the 'game' subdirectory.",
                    "Recomp", MB_ICONERROR);
        return 1;
    }
    printf("XBE loaded: %zu bytes\n", xbe_size);

#ifdef XBA_TITLE_PROFILE
    xba_title_configure();
#endif
    /* The XDK switches (DISPATCH gate, paced vblanks, the USB pad, AC'97
     * ready): after the layers, so a title layer's and the environment's win
     * (src/kernel/recomp_xdk_defaults.h; RECOMP_XDK_DEFAULTS=0: none). */
    recomp_xdk_defaults();
    /* After the layers and the defaults, so a title's RECOMP_PB_WALK wins over
     * the service's DMA-walk default (RECOMP_D3D_SERVICE=0: no service). */
    recomp_d3d_configure();

    /* Step 2: Initialize Xbox memory layout */
    printf("Initializing Xbox memory layout...\n");
    if (!xbox_MemoryLayoutInit(xbe_data, xbe_size)) {
        MessageBoxA(NULL, "Failed to initialize Xbox memory layout.\n"
                    "The required virtual address range may be unavailable.",
                    "Recomp", MB_ICONERROR);
        free(xbe_data);
        return 1;
    }

    g_xbox_mem_offset = xbox_GetMemoryOffset();

    /* The pad. XAPI is linked into the title and drives the MCPX USB host
     * controllers itself, so a controller has to be there for it to find:
     * xbox_OhciInit models them once guest memory is mapped. It does nothing
     * unless RECOMP_USB=1 (and RECOMP_KEYBOARD=1 lets the keyboard stand in
     * for the pad: Enter = Start, Z/X/C/V = A/B/X/Y, arrows, WASD). Nothing
     * called it before, so a title built from this template saw no pad even
     * with RECOMP_USB set (Fable: The Lost Chapters sat on "Press START"). */
    {
        extern void xbox_OhciInit(void);
        xbox_OhciInit();
    }
    printf("Xbox memory mapped. Offset: 0x%llX\n", (unsigned long long)g_xbox_mem_offset);

    /* Bring up the emulated APU.
     *
     * Gated on the same variable that unmaps its registers, because the two
     * halves are useless apart: a trap with no model declines every access,
     * and a model nothing traps into never sees a register. The APU walks
     * Xbox physical RAM to find voice buffers, so it gets the guest RAM
     * base. */
    if (getenv("RECOMP_AC97_READY") && *getenv("RECOMP_AC97_READY")
            && *getenv("RECOMP_AC97_READY") != '0') {   /* "0": off (XBA-106) */
        g_apu_state = mcpx_apu_init_standalone((uint8_t *)xbox_GetMemoryBase());
        fprintf(stderr, "[BOOT] emulated APU %s\n",
                g_apu_state ? "up" : "FAILED to initialise");
    }

    /* Step 3: Initialize Xbox kernel */
    printf("Initializing Xbox kernel replacement...\n");
    xbox_kernel_init();

    /* Step 4: Set game directory for file I/O path translation */
    {
        extern void xbox_path_init(const char *game_dir, const char *save_dir);
        xbox_path_init(s_game_dir, s_save_dir);
    }

    /* Step 5: Initialize kernel bridge (thunk table in Xbox memory) */
    printf("Initializing kernel bridge...\n");
    xbox_kernel_bridge_init();

    /* Step 6: Initialize stack */
    g_esp = XBOX_STACK_TOP;

    /*
     * TODO: Pre-initialize CRT globals if needed.
     *
     * Many Xbox games use the MSVC CRT. The CRT's __heap_init sets up a
     * heap descriptor at a game-specific address. You may need to
     * pre-initialize __active_heap to avoid small-block heap issues:
     *
     *   uint32_t *active_heap = (uint32_t *)((uint8_t *)g_xbox_mem_offset + ACTIVE_HEAP_VA);
     *   *active_heap = 1;  // 1 = system heap (HeapAlloc), avoids SBH init
     *
     * Find ACTIVE_HEAP_VA by searching the disassembly for __heap_init
     * or by looking for the CRT's __active_heap global in the data section.
     */

    printf("\n=== Initialization complete ===\n");
    printf("Entry point: 0x%08X\n", YOUR_GAME_ENTRY_POINT);
    printf("ESP: 0x%08X\n", g_esp);

    /* Build the flat dispatch table before the guest runs.
     *
     * Without this recomp_lookup falls back to a binary search over the
     * whole function table -- roughly log2(n) branches on *every*
     * indirect call, which for a 45,000-function C++ title is about 16
     * every time the game goes through a vtable. Half-Life 2 spent most
     * of its static initialisation inside recomp_lookup for exactly this
     * reason, and it read as a hang.
     *
     * Optional by design: if the allocation fails the search still works,
     * so a failure is worth one line and not a fatal error. */
    if (!recomp_dispatch_init())
        fprintf(stderr, "[BOOT] flat dispatch unavailable; "
                        "indirect calls will use the binary search\n");

    /* Arm the hang watchdog. Does nothing unless RECOMP_WATCHDOG_SECS is set,
     * and must be called from this thread -- the guest registers it samples are
     * thread-local, so it has to be handed the copies belonging to the thread
     * that runs guest code.
     *
     * Not optional boilerplate: without this call RECOMP_WATCHDOG_SECS is
     * silently inert, and the one diagnostic that tells a hang from slowness
     * does nothing while appearing to be set. */
    xbox_WatchdogStart();

    recomp_d3d_start(s_game_dir, s_save_dir);
#ifdef XBA_TITLE_PROFILE
    xba_title_start(s_game_dir, s_save_dir);
#endif

    /* Code written at run time (XBA-118): the loader is done, so the lifted
     * code pages go read-only and the first write to each is named
     * (src/kernel/recomp_smc.h; RECOMP_SMC=0: off). */
    {
        extern int xbox_SmcArm(void);
        (void)xbox_SmcArm();
    }

    /* Step 7: Call the recompiled entry point */
    printf("\nStarting game...\n");
    fflush(stdout);

#ifndef _WIN32
    /* « Resume where I left off » (XBA-250): with RECOMP_STATE_RESUME on and an automatic slot that can be
     * resumed, the saved threads are made again and re-enter their call chains instead of the entry point
     * running; this thread waits for the resumed main thread. */
    {
        extern int xbox_StateResumeAtBoot(void);
        extern int xbox_StateResumeWait(void);
        if (xbox_StateResumeAtBoot())
            xbox_StateResumeWait();
        else
            xbe_entry_point();
    }
#else
    xbe_entry_point();
#endif

    printf("\nGame returned. Cleaning up...\n");

    /* Cleanup */
    xbox_kernel_shutdown();
    xbox_MemoryLayoutShutdown();
    free(xbe_data);

    return 0;
}

/* ── XBE Loading ───────────────────────────────────────────── */

static BOOL load_xbe(const char *path, void **out_data, size_t *out_size)
{
    FILE *f = fopen(path, "rb");
#ifndef _WIN32
    /* The game directory may be a disc image read in place (XBA-126):
     * "game" a link to Game.iso / .cso / a split part, or the shell's
     * "fd:N". The XBE then comes out of the image, which stays mounted
     * for D:\ (src/kernel/kernel_disc.h). */
    if (!f) {
        extern int xbox_disc_slurp(const char *host_path, void **data, size_t *size);
        if (xbox_disc_slurp(path, out_data, out_size) == 0)
            return TRUE;
    }
#endif
    if (!f) {
        fprintf(stderr, "Cannot open XBE: %s\n", path);
        return FALSE;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0) {
        fclose(f);
        return FALSE;
    }

    void *data = malloc((size_t)size);
    if (!data) {
        fclose(f);
        return FALSE;
    }

    if (fread(data, 1, (size_t)size, f) != (size_t)size) {
        free(data);
        fclose(f);
        return FALSE;
    }

    fclose(f);
    *out_data = data;
    *out_size = (size_t)size;
    return TRUE;
}

#ifdef XBOXRECOMP_SHELL_TITLE
/* Android: the app shell's hook (src/platform/recomp_app.h). Called once
 * default.xbe is found, on the guest thread (64 MB stack) of the window.
 * When the guest returns, the window stays on its last frame until Back is
 * pressed twice. */
int recomp_title_main(const recomp_storage *store)
{
    s_xbe_path = store->xbe;
    s_game_dir = store->game_dir;
    s_save_dir = store->save_dir;
    return game_main(NULL, NULL, NULL, 0);
}
#else
#ifndef _WIN32
#include <limits.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
/* The game paths are relative ("game/default.xbe"): started from Finder or
 * another directory, move to the binary's own folder first. */
static void chdir_to_executable(void)
{
    char path[PATH_MAX];
    char *slash;
#ifdef __APPLE__
    uint32_t size = sizeof path;
    if (_NSGetExecutablePath(path, &size) != 0)
        return;
#else
    ssize_t n = readlink("/proc/self/exe", path, sizeof path - 1);
    if (n <= 0)
        return;
    path[n] = 0;
#endif
    slash = strrchr(path, '/');
    if (!slash)
        return;
    *slash = 0;
    if (chdir(path) != 0)
        fprintf(stderr, "[BOOT] chdir %s failed\n", path);
}

extern int xbox_HostWindowMain(int (*guest_main)(void));
static int guest_main_posix(void) { return game_main(NULL, NULL, NULL, 0); }
#endif

/* Console entry point (for debugging -- lets you see printf output) */
int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
#ifdef XBA_TITLE_ENV_FORCE
    for (size_t i = 0; i < sizeof xba_title_env_force / sizeof xba_title_env_force[0]; i++)
        recomp_cfg_force(XBA_TITLE_ENV_FORCE_TID, xba_title_env_force[i][0], xba_title_env_force[i][1]);
#endif
#ifdef XBA_IMAGE_MAP
    /* The disc's other images and their executables (tools.prepare, XBA-82). */
    if (!getenv("RECOMP_IMAGE_MAP")) {
#ifdef _WIN32
        _putenv_s("RECOMP_IMAGE_MAP", XBA_IMAGE_MAP);
#else
        setenv("RECOMP_IMAGE_MAP", XBA_IMAGE_MAP, 0);
#endif
    }
#endif
#ifdef XBA_IMAGE_NAME
    /* This executable is that image of the disc, not default.xbe: a launch
     * back to default.xbe is then a hand-over, not a restart of this one. */
#ifdef _WIN32
    _putenv_s("RECOMP_IMAGE", XBA_IMAGE_NAME);
#else
    setenv("RECOMP_IMAGE", XBA_IMAGE_NAME, 1);
#endif
#endif
#ifdef XBA_DISCOVERY_FILE
    /* The discovery file in the prepare's work folder (CMake XBA_DISCOVERY_FILE,
     * set by tools.prepare; src/kernel/recomp_discovery.h). RECOMP_DISCOVERY
     * set by hand wins; =0 turns it off. */
    if (!getenv("RECOMP_DISCOVERY")) {
#ifdef _WIN32
        _putenv_s("RECOMP_DISCOVERY", XBA_DISCOVERY_FILE);
#else
        setenv("RECOMP_DISCOVERY", XBA_DISCOVERY_FILE, 0);
#endif
    }
#endif
#ifdef _WIN32
    return WinMain(GetModuleHandle(NULL), NULL, GetCommandLineA(), SW_SHOW);
#else
    chdir_to_executable();
    /* The Android shell's defaults, on the desktop too: the frame-buffer
     * window and the pushbuffer executor that draws into it (a title that
     * links the XDK's D3D shows nothing without them). Either can still be
     * set to 0. The window lives on the process main thread (macOS requires
     * it) and the guest on a thread of its own. */
    setenv("RECOMP_FB_WINDOW", "1", 0);
    setenv("RECOMP_PB_EXEC", "1", 0);
    return xbox_HostWindowMain(guest_main_posix);
#endif
}
#endif /* XBOXRECOMP_SHELL_TITLE */
