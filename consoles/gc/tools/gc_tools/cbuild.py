"""Builds the GameCube runtime's programs from source with a plain C compiler, for the Python tests (XBA-225, XBA-261):
gc_run and gc_disc_info as the CMake build makes them (consoles/gc/runtime, consoles/gc/tests), with the codecs the
disc readers use compiled from third_party/libchdr (miniz, zstd, the LZMA SDK decoder; their warnings are not ours).
Objects are compiled in parallel and kept in the output directory."""

import os
import subprocess

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, "..", "..", "..", ".."))
RUNTIME = os.path.join(REPO, "consoles", "gc", "runtime")
CHDR = os.path.join(REPO, "third_party", "libchdr")
PLATFORM = os.path.join(REPO, "src", "platform")
CORE = ["gc_core.c", "gc_cpu.c", "gc_bus.c", "gc_exi.c", "gc_si.c", "gc_mmu.c", "gc_recomp.c", "gc_rel.c", "gc_state.c", "gc_pack.c", "gc_extract.c", "gc_disc.c", "gc_disc_wia.c", "gc_bzip2.c", "gc_di.c",
        "gc_boot.c", "gx/gx_fifo.c", "gx/gx_vtx.c", "gx/gx_tex.c", "gx/gx_shader.c", "gx/gx_gl.c", "gx/gx_egl.c", "gx/gx_progcache.c",
        "gc_dsp.c", "gc_ai.c", "gc_ax.c", "gc_asnd.c", "gc_zelda.c", "gc_aesnd.c", "gc_audio_util.c"]
SHARED = [os.path.join(PLATFORM, "console_pack.c"), os.path.join(PLATFORM, "recomp_sha256.c")]  # gc_pack.c's shared part
CODECS = [os.path.join(CHDR, "deps", "miniz-3.1.2", "miniz.c"), os.path.join(CHDR, "deps", "zstd-1.5.7", "zstddeclib.c"),
          os.path.join(CHDR, "deps", "lzma-26.02", "src", "LzmaDec.c")]
DEFINES = ["-DMINIZ_NO_ARCHIVE_APIS", "-DMINIZ_NO_DEFLATE_APIS", "-DMINIZ_NO_STDIO", "-DMINIZ_NO_TIME"]
INCLUDES = ["-I", RUNTIME, "-I", PLATFORM, "-I", os.path.join(CHDR, "deps", "miniz-3.1.2"), "-I", os.path.join(CHDR, "deps", "zstd-1.5.7"),
            "-I", os.path.join(CHDR, "deps", "lzma-26.02", "include")]


def _compile(jobs):
    procs = [(subprocess.Popen(cmd, stderr=subprocess.PIPE), cmd) for cmd in jobs]
    for p, cmd in procs:
        _, err = p.communicate()
        if p.returncode:
            raise RuntimeError("%s failed:\n%s" % (" ".join(cmd), err.decode(errors="replace")))


def core_objects(outdir, cc="cc", cflags=()):
    """The core's and the codecs' objects in outdir (compiled once)."""
    os.makedirs(outdir, exist_ok=True)
    objs, jobs = [], []
    for src in [os.path.join(RUNTIME, f) for f in CORE] + SHARED + CODECS:
        third = src in CODECS
        obj = os.path.join(outdir, os.path.basename(src) + ".o")
        objs.append(obj)
        if not os.path.exists(obj):
            flags = ["-O1", "-w"] if third else list(cflags)
            jobs.append([cc, "-c", "-fPIC"] + flags + DEFINES + INCLUDES + ["-o", obj, src])
    _compile(jobs)
    return objs


def gc_run(outdir, cc="cc", cflags=()):
    exe = os.path.join(outdir, "gc_run")
    objs = core_objects(outdir, cc, cflags)
    platform = os.path.join(REPO, "src", "platform")  # --rewind / --cheats (XBA-264)
    subprocess.run([cc] + list(cflags) + ["-rdynamic", "-DGC_RUN_PLATFORM=1", "-I", platform] + INCLUDES
                   + ["-o", exe, os.path.join(RUNTIME, "tools", "gc_run.c"), os.path.join(platform, "recomp_rewind.c"),
                      os.path.join(platform, "recomp_cheats.c")] + objs + ["-ldl", "-lm", "-lpthread"], check=True)
    return exe


def gc_disc_info(outdir, cc="cc", cflags=()):
    exe = os.path.join(outdir, "gc_disc_info")
    objs = core_objects(outdir, cc, cflags)
    platform = os.path.join(REPO, "src", "platform")
    subprocess.run([cc] + list(cflags) + INCLUDES + ["-I", platform, "-o", exe,
                    os.path.join(REPO, "consoles", "gc", "tests", "gc_disc_info.c"),
                    os.path.join(RUNTIME, "gc_identify.c"), os.path.join(platform, "recomp_pack.c"),
                    os.path.join(platform, "recomp_cfg.c")] + objs + ["-ldl", "-lm", "-lpthread"], check=True)
    return exe
