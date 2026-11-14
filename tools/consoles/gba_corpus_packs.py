#!/usr/bin/env python3
"""The GBA homebrew corpus through the whole user chain, headless (XBA-180).

    tools/consoles/gba_corpus_packs.py CORPUS_DIR OUT_DIR --gba-run PATH [--expected FILE] [--update] [--frames N]

For each CORPUS_DIR/<prog>.gba (tools/consoles/build_homebrew.sh gba: our own programs, never game data):

1. `python3 -m tools.prepare <prog>.gba` -- the desktop title pack, as a player's Prepare makes it
   (tools/prepare/gba.py: identify, analyse, lift with gba_tools.recomp, compile with the host cc, libtitle.so)
2. `gba_run <prog>.gba --pack libtitle.so --lockstep --result` -- the pack run headless next to an
   interpreter-only machine (the whole state compared after every frame), until the program's result block says
   PASS or FAIL (consoles/gba/tests/homebrew/README.md « Result protocol »)
3. `gba_run ... --frames <verdict frame + 30> --ppm`: that frame kept as OUT_DIR/<prog>.png (and .ppm), its sha256
   compared with the expected list

A program fails when Prepare fails, the run does not end on PASS, the lockstep diverges, or the screenshot's
sha256 differs from its line in --expected (sha256sum format: "<sha256>  <prog>.ppm"; a program missing from the
list fails too). --update rewrites --expected from this run instead (then check the PNGs before committing it).
Writes OUT_DIR/report.md (one row per program) and OUT_DIR/SHA256SUMS. Exit 0 when every program passed.
"""

import argparse
import hashlib
import os
import re
import struct
import subprocess
import sys
import time
import zlib

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
SETTLE = 30                 # frames past the verdict before the screenshot
DEFAULT_EXPECTED = os.path.join(REPO, "consoles", "gba", "tests", "corpus_screens.sha256")


def read_expected(path):
    out = {}
    if path and os.path.exists(path):
        with open(path, encoding="utf-8") as f:
            for line in f:
                line = line.split("#", 1)[0].strip()
                if line:
                    digest, name = line.split(None, 1)
                    out[name.lstrip("*")] = digest.lower()
    return out


def ppm_to_png(ppm, png):
    """A binary PPM (P6, maxval 255; gba_run --ppm) as an 8-bit RGB PNG, with zlib only."""
    with open(ppm, "rb") as f:
        data = f.read()
    m = re.match(rb"P6\s+(\d+)\s+(\d+)\s+255\s", data)
    if not m:
        raise ValueError("%s: not a P6 PPM" % ppm)
    w, h = int(m.group(1)), int(m.group(2))
    px = data[m.end():m.end() + w * h * 3]
    raw = b"".join(b"\0" + px[y * w * 3:(y + 1) * w * 3] for y in range(h))

    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF)
    with open(png, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def sha256(path):
    with open(path, "rb") as f:
        return hashlib.sha256(f.read()).hexdigest()


def gba(lf, args):
    """Runs gba_run: (output, exit code), logged to lf."""
    try:
        p = subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=300)
        text, rc = p.stdout, p.returncode
    except subprocess.TimeoutExpired as e:
        text = e.stdout.decode(errors="replace") if isinstance(e.stdout, bytes) else (e.stdout or "")
        text, rc = text + "\ntimed out after 300 s\n", -1
    lf.write("\n$ %s\n%s" % (" ".join(args), text))
    return text, rc


def run_one(prog, rom, out, gba_run, frames, expected, update):
    """One program: (row dict, ok)."""
    row = {"prog": prog, "prepare": "-", "run": "-", "lockstep": "-", "native": "-", "screen": "-"}
    work = os.path.join(out, "work", prog)
    log = os.path.join(out, prog + ".log")
    with open(log, "w") as lf:
        t0 = time.time()
        p = subprocess.run([sys.executable, "-m", "tools.prepare", rom, "--work", work,
                            "--pack-dir", os.path.join(out, "packs")],
                           cwd=REPO, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=600)
        lf.write(p.stdout)
        m = re.search(r"^XBA_RESULT ok pack=(.+)$", p.stdout, re.M)
        if p.returncode != 0 or not m:
            row["prepare"] = "FAIL"
            last = [ln for ln in p.stdout.splitlines() if ln.startswith("XBA_RESULT")]
            print("  prepare failed: %s" % (last[-1] if last else "exit %d" % p.returncode))
            return row, False
        lib = os.path.join(m.group(1), "libtitle.so")
        row["prepare"] = "ok %.1f s" % (time.time() - t0)

        # The verdict run, then the screenshot run: SETTLE frames past the verdict frame, so the screen shows what
        # the program draws once done (most draw their verdict backdrop after writing the block).
        ppm = os.path.join(out, prog + ".ppm")
        if os.path.exists(ppm):
            os.remove(ppm)
        text, rc = gba(lf, [gba_run, rom, "--quiet", "--pack", lib, "--lockstep", "--stats", "--result",
                            "--frames", str(frames)])
        verdict = re.search(r"^\S+\s+(PASS|FAIL)\b.*\(after (\d+) frames", text, re.M)
        if verdict:
            shot, src = gba(lf, [gba_run, rom, "--quiet", "--pack", lib, "--lockstep",
                                 "--frames", str(int(verdict.group(2)) + SETTLE), "--ppm", ppm])
            text += shot
            rc = rc or src
    row["run"] = re.sub(r"\s+", " ", verdict.group(0).split(" (after")[0]) if verdict else "no verdict (exit %d)" % rc
    ok = rc == 0 and bool(verdict) and verdict.group(1) == "PASS"
    if "LOCKSTEP DIVERGED" in text or "LOCKSTEP OK" not in text:
        row["lockstep"] = "DIVERGED" if "LOCKSTEP DIVERGED" in text else "?"
        ok = False
    else:
        row["lockstep"] = "ok"
    st = re.search(r"interpreted cartridge (\d+)", text)
    if st:
        row["native"] = "%s interpreted" % st.group(1)   # a Prepare pack has no native counter (GBA_RC_COUNT)
    for ln in text.splitlines():
        if not ln.startswith("  table["):
            print("  " + ln)
    if os.path.exists(ppm):
        digest = sha256(ppm)
        ppm_to_png(ppm, os.path.join(out, prog + ".png"))
        want = expected.get(prog + ".ppm")
        if update:
            row["screen"] = digest[:16] + " (recorded)"
        elif want == digest:
            row["screen"] = digest[:16] + " ok"
        else:
            row["screen"] = "%s CHANGED (expected %s)" % (digest[:16], want[:16] if want else "none")
            print("  screenshot %s.ppm: sha256 %s, expected %s" % (prog, digest, want or "none listed"))
            ok = False
    else:
        row["screen"] = "none"
        ok = False
    return row, ok


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("corpus")
    ap.add_argument("out")
    ap.add_argument("--gba-run", required=True)
    ap.add_argument("--expected", default=DEFAULT_EXPECTED)
    ap.add_argument("--update", action="store_true", help="rewrite --expected from this run")
    ap.add_argument("--frames", type=int, default=900, help="most frames to wait for a verdict (default 900)")
    a = ap.parse_args()

    roms = sorted(f for f in os.listdir(a.corpus) if f.endswith(".gba")) if os.path.isdir(a.corpus) else []
    if not roms:
        print("no .gba in %s: run tools/consoles/build_homebrew.sh gba first" % a.corpus)
        return 1
    os.makedirs(a.out, exist_ok=True)
    out = os.path.abspath(a.out)
    expected = read_expected(a.expected)
    rows, failed = [], []
    for f in roms:
        prog = f[:-4]
        print("== %s" % prog, flush=True)
        row, ok = run_one(prog, os.path.abspath(os.path.join(a.corpus, f)), out, os.path.abspath(a.gba_run),
                          a.frames, expected, a.update)
        rows.append(row)
        if not ok:
            failed.append(prog)
        print("RESULT %s %s" % (prog, "PASS" if ok else "FAIL"), flush=True)

    sums = ["%s  %s.ppm" % (sha256(os.path.join(out, r["prog"] + ".ppm")), r["prog"]) for r in rows
            if os.path.exists(os.path.join(out, r["prog"] + ".ppm"))]
    with open(os.path.join(out, "SHA256SUMS"), "w") as f:
        f.write("\n".join(sums) + "\n")
    if a.update:
        with open(a.expected, "w") as f:
            f.write("# sha256 of the last frame of each GBA homebrew program run as a Prepare pack (XBA-180):\n"
                    "# tools/consoles/gba_corpus_packs.py, gba_run --ppm 30 frames after the verdict. Rewrite with --update.\n")
            f.write("\n".join(sums) + "\n")
        print("wrote %s" % a.expected)
    cols = ("prog", "prepare", "run", "lockstep", "native", "screen")
    with open(os.path.join(out, "report.md"), "w") as f:
        f.write("| Program | Prepare | Result block | Lockstep | Cartridge code | Screenshot sha256 |\n")
        f.write("|---|---|---|---|---|---|\n")
        for r in rows:
            f.write("| " + " | ".join(r[c] for c in cols) + " |\n")
    print()
    with open(os.path.join(out, "report.md")) as f:
        print(f.read())
    print("%d/%d programs passed%s" % (len(rows) - len(failed), len(rows),
                                       (": failed " + ", ".join(failed)) if failed else ""))
    return 1 if failed and not a.update else 0


if __name__ == "__main__":
    sys.exit(main())
