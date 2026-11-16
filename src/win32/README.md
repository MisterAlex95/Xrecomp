# Win32 runtime (spike)

XBA-233, epic XBA-231 (Windows PC games). It runs a 32-bit Windows console program recompiled by the x86
pipeline, on Linux x86-64 and arm64 (and other POSIX hosts). Only the console subset works today: no windows, no
D3D, no threads, no SEH.

```sh
sh tools/win32/build_hello.sh out/win32                  # our own mingw hello worlds (Docker if no cross gcc)
python3 -m tools.win32 out/win32/hello_crt.exe --work out/w --run one two
# Hello, world! (argc=3, through the CRT)
```

`tools.win32` runs `tools.pe_parser` (the PE front end: sections, entry point, imports by name or ordinal, base
relocations, TLS), then `tools.disasm`, `tools.func_id`, `tools.abi_analysis` and `tools.recomp` as for an XBE.
It then compiles the lifted C with these sources and a `main` that calls `w32_main`. The executable reads the `.exe`
at run time (its first argument), as an Xbox title reads its XBE.

- `win32_runtime.c`: the guest memory (the 4 GiB guest space reserved, pages committed as needed; the same model as
  the Xbox runtime, guest address + `g_xbox_mem_offset`). The PE loader maps the image at its preferred base, because
  the lifted code has its absolute addresses built in. It resolves imports into synthetic addresses
  `W32_THUNK_BASE + 4 * n`, which `recomp_lookup_kernel` turns into HLE calls. It also sets up the TEB/PEB (`fs:`)
  and TLS, and defines the symbols the lifted code links against.
- `win32_hle.c`: the kernel32 and msvcrt functions the hello worlds import. Each HLE function reads its arguments
  off the guest stack and pops them if it is stdcall. An import that has no HLE is reported when it is called, and
  the run stops with exit status 127.

It is separate from the Xbox kernel (`src/kernel`) and links none of it. The two share only the lifter's ABI
(`templates/runtime/recomp_types.h`). Set `XBR_WIN32_TRACE=1` to log one line per import call.

Tests: `tools/pe_parser/test_pe_parser.py` (synthetic PEs) and `tools/win32/test_e2e.py` (`XBA_E2E=1`: the
synthetic hello world built, run and checked; the mingw ones too when `i686-w64-mingw32-gcc` or
`XBA_WIN32_HELLO_DIR` is available).
