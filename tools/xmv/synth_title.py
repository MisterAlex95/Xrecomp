"""
A synthetic movie-playing title of our own (XBA-199): a fake XDK « XMV
library » with the real API's shape, and title code that plays a movie with
it the way XDK titles do. No game data: every byte comes from here.

    XMV section (the library)
      create(flags, name, &decoder)     ret 0xC   NtCreateFile(name), *decoder = its object
      get_next_frame(dec, surf, &r, &t) ret 0x10  the decoder that cannot run: XMV_NOFRAME for ever
      close(dec)                        ret 4
    .text (the title)
      entry: pixels from MmAllocateContiguousMemory into a D3DSurface
             (LIN_X8R8G8B8, 128x64, pitch 512); create(0, "D:\\intro.xmv", &dec);
             do get_next_frame(dec, &surface, &result, &time) while result != XMV_ENDOFFILE;
             close(dec); DbgPrint("movie done"); spin.
      decoy: a `ret 0x10` function of the title, called once, not in a loop.

Run as lifted, the title polls for ever. With get_next_frame bound to the
runtime's hook (tools.prepare's XMV binding), the movie the title opened plays
into the surface and the title goes on to "movie done".

Variants: section="XMV" or another name (".text2": the library code is not in
a section of its own), strings=False (no movie name in the image),
movie=False (no movie player at all: the negative case), and play=True: the
title hands the movie to the library's own player, play(dec, 0, 0) (ret 0xC,
XMVDecoder_Play's shape), which polls get_next_frame itself; the title never
calls get_next_frame (XBA-309).
"""

import struct

from tools.disasm.synthetic import DATA, Title

NT_CREATE_FILE, MM_ALLOC_CONTIG, DBG_PRINT = 190, 165, 8
IMPORTS = (NT_CREATE_FILE, MM_ALLOC_CONTIG, DBG_PRINT)
S_NTCREATE, S_MMALLOC, S_DBGPRINT = range(3)

DONE = "movie done"
MOVIE = "D:\\intro.xmv"
W, H, PITCH = 128, 64, 512
LIN_X8R8G8B8 = 0x1E


def movie_title(path, section="XMV", strings=True, movie=True, libs=None, title_id=0x58424199, title="Movie",
                play=False):
    """Write the title to path; returns (path, labels {name: va}) for the API and title functions."""
    t = Title()
    x = t.section(".text")
    lib = t.section(section if movie else ".text2")
    r = t.section(".rdata", DATA)
    d = t.section(".data", DATA)

    # ── title code ───────────────────────────────────────────────────────
    x.label("entry").hex("558bec 83ec20")
    x.hex("6a00").hex("6a00").hex("6a00").hex("6a00").call("decoy")      # decoy(0, 0, 0, 0)
    if movie:
        x.hex("6800800000").call_kernel(S_MMALLOC)                       # MmAllocateContiguousMemory(0x8000)
        x.hex("25ffffff7f")                                               # and eax, 0x7FFFFFFF: physical
        x.hex("a3").abs("surface", 4)                                     # surface.Data = eax
        x.push("dec").push("movie_name" if strings else "anon_name").hex("6a00").call("create")
    if movie and play:
        x.hex("6a00 6a00").hex("ff35").abs("dec").call("play")            # play([dec], 0, 0)
        x.hex("ff35").abs("dec").call("close")
        x.push("done").call_kernel(S_DBGPRINT).hex("83c404")              # DbgPrint("movie done")
    elif movie:
        x.label("poll")
        x.hex("8d45f0 50")                                                # lea eax, [ebp-0x10]; push eax (&time)
        x.hex("8d45fc 50")                                                # lea eax, [ebp-4]; push eax (&result)
        x.push("surface")
        x.hex("ff35").abs("dec")                                          # push [dec]
        x.call("get_next_frame")
        x.hex("837dfc02")                                                 # cmp dword [ebp-4], 2
        x.hex("75").rel8("poll")                                          # jne poll
        x.hex("ff35").abs("dec").call("close")
        x.push("done").call_kernel(S_DBGPRINT).hex("83c404")              # DbgPrint("movie done")
    x.hex("ebfe").align(16)                                               # spin
    # decoy: a stdcall title function with four arguments, called once
    x.label("decoy").hex("8b442404 03442408 c21000").align(16)

    # ── the fake XMV library ────────────────────────────────────────────
    if not movie:                       # the negative case: no library, no movie name
        return _write(t, path, libs, title_id, title, ("entry", "decoy"))
    lib.label("create").hex("558bec")
    lib.hex("8b450c").hex("a3").abs("ansi", 4)                            # ansi.Buffer = name
    lib.hex("6a60 6a01 6a01 6880000000 6a00")
    lib.push("iosb").push("objattr").hex("6800001080").push("handle")
    lib.call_kernel(S_NTCREATE)
    lib.hex("8b4510 c700").abs("decoder_obj")                             # *ppDecoder = &decoder_obj
    lib.hex("33c0 5d c20c00").align(16)
    lib.label("get_next_frame")
    lib.hex("8b44240c c70000000000")                                      # *pResult = XMV_NOFRAME
    lib.hex("8b442410 85c0 7408 c70000000000 c7400400000000")             # *pTimeStamp = 0 (if any)
    lib.hex("33c0 c21000").align(16)
    lib.label("close").hex("33c0 c20400").align(16)
    if play:                            # XMVDecoder_Play: polls get_next_frame until XMV_ENDOFFILE
        lib.label("play").hex("558bec 83ec10")
        lib.label("play_poll")
        lib.hex("8d45f8 50")                                              # &time
        lib.hex("8d45fc 50")                                              # &result
        lib.hex("6a00")                                                   # its own surface
        lib.hex("ff7508")                                                 # push [ebp+8]: the decoder
        lib.call("get_next_frame")
        lib.hex("837dfc02")                                               # cmp dword [ebp-4], 2
        lib.hex("75").rel8("play_poll")
        lib.hex("33c0 c9 c20c00").align(16)

    # ── data ─────────────────────────────────────────────────────────────
    name = MOVIE.encode() + b"\0"
    r.label("movie_name").raw(name).align(4)
    r.label("anon_name").raw(b"D:\\intro.bin\0").align(4)
    r.label("done").raw(DONE.encode() + b"\0").align(4)
    d.label("dec").u32(0)
    d.label("handle").u32(0)
    d.label("iosb").u32(0).u32(0)
    d.label("ansi").raw(struct.pack("<HH", len(MOVIE), len(name))).u32(0)
    d.label("objattr").u32(0).abs("ansi").u32(0x40)                      # OBJ_CASE_INSENSITIVE
    d.label("decoder_obj").raw(b"\0" * 64)
    # D3DSurface: Common, Data (physical), Lock, Format, Size
    d.label("surface").u32(0x00040001).u32(0).u32(0).u32(LIN_X8R8G8B8 << 8)
    d.u32((W - 1) | ((H - 1) << 12) | ((PITCH // 64 - 1) << 24)).raw(b"\0" * 12)

    return _write(t, path, libs, title_id, title,
                  ("entry", "create", "get_next_frame", "close", "decoy") + (("play",) if play else ()))


def _write(t, path, libs, title_id, title, names):
    kw = {}
    if libs is not None:
        kw["libs"] = libs
    p = t.write(str(path), "entry", imports=IMPORTS, title_id=title_id, title=title, **kw)
    return p, {k: t.va(k) for k in names}
