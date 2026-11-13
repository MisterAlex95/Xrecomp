"""
The progress protocol of `tools.prepare --android` (docs/android/prepare.md)
and of the desktop `tools.prepare --progress` (docs/macos/launcher.md
« Prepare interface »).

One line per event on stdout, which the app's service reads:

    XBA_PROGRESS step=<i>/<n> id=<step id> pct=<0-100> [eta=<s>] msg=<rest of line>
    XBA_RESULT ok pack=<dir>          (Android)
    XBA_RESULT ok exe=<file>          (desktop)
    XBA_RESULT error code=<UPPER_SNAKE> msg=<text>

`msg` is always last and takes the rest of the line (it may hold spaces and
`=`); the other values never hold spaces. A reader ignores keys it does not
know, so keys can be added before `msg` later.
"""

import re
import sys
import threading

STEP_IDS = ("identify", "analyse", "lift", "compile", "pack")
STEPS = len(STEP_IDS)
# The desktop prepare's five steps (tools/prepare/__main__.py).
DESKTOP_STEP_IDS = ("identify", "analyse", "project", "lift", "build")

# Error codes (XBA_RESULT error code=...). The app maps them to messages.
ERROR_CODES = (
    "BAD_ARGS",         # a flag is missing or wrong
    "NO_XBE",           # no default.xbe in the source
    "BAD_XBE",          # xbe_parser could not read it
    "ANALYSE_FAILED",   # disasm, func_id or abi_analysis failed
    "LIFT_FAILED",      # tools.recomp failed
    "COMPILE_FAILED",   # a C file did not compile
    "LINK_FAILED",      # ld.lld failed
    "PACK_FAILED",      # writing or publishing the pack failed
    "NO_SPACE",         # out of storage (ENOSPC)
    "CANCELLED",        # SIGTERM / SIGINT
    "BUSY",             # another prepare holds this work folder
    "INTERNAL",         # a bug: anything else
)

_CODE = re.compile(r"^[A-Z][A-Z0-9_]*$")


def _one_line(text):
    return " ".join(str(text).split())


def progress_line(step, sid, pct, msg, eta=None, steps=STEPS, ids=STEP_IDS):
    """XBA_PROGRESS line for step `step` (1-based) with id `sid`."""
    if sid not in ids:
        raise ValueError("unknown step id %r" % sid)
    pct = max(0, min(100, int(pct)))
    head = "XBA_PROGRESS step=%d/%d id=%s pct=%d" % (step, steps, sid, pct)
    if eta is not None:
        head += " eta=%d" % max(0, int(round(eta)))
    return "%s msg=%s" % (head, _one_line(msg))


def result_ok(pack_dir):
    return "XBA_RESULT ok pack=%s" % pack_dir


def result_ok_exe(exe):
    return "XBA_RESULT ok exe=%s" % exe


def result_ok_project(proj):
    """Desktop --no-build: no executable, the project folder."""
    return "XBA_RESULT ok project=%s" % proj


def result_error(code, msg):
    if not _CODE.match(code):
        raise ValueError("error code %r is not UPPER_SNAKE" % code)
    return "XBA_RESULT error code=%s msg=%s" % (code, _one_line(msg))


def parse_line(line):
    """The reader side (the app does the same in Kotlin): a dict, or None
    for a line that is not ours. {"kind": "progress"|"result", ...}; values
    stay strings except step/steps/pct/eta (int) and ok (bool)."""
    line = line.rstrip("\r\n")
    if line.startswith("XBA_PROGRESS "):
        out = {"kind": "progress"}
        rest = line[len("XBA_PROGRESS "):]
    elif line.startswith("XBA_RESULT "):
        out = {"kind": "result"}
        rest = line[len("XBA_RESULT "):]
        word, _, rest = rest.partition(" ")
        if word not in ("ok", "error"):
            return None
        out["ok"] = word == "ok"
    else:
        return None
    while rest:
        if rest.startswith("msg="):
            out["msg"] = rest[4:]
            break
        tok, _, rest = rest.partition(" ")
        key, eq, val = tok.partition("=")
        if eq:
            out[key] = val
    if out["kind"] == "progress":
        try:
            i, n = out["step"].split("/")
            out["step"], out["steps"] = int(i), int(n)
            out["pct"] = int(out["pct"])
            if "eta" in out:
                out["eta"] = int(out["eta"])
        except (KeyError, ValueError):
            return None
    return out


class Reporter:
    """Writes the protocol lines, one at a time (worker threads report too),
    and drops a progress line identical to the previous one."""

    def __init__(self, stream=None, ids=STEP_IDS):
        self.stream = stream or sys.stdout
        self.ids = ids
        self.lock = threading.Lock()
        self.last = None

    def _write(self, line):
        with self.lock:
            if line == self.last:
                return
            self.last = line
            self.stream.write(line + "\n")
            self.stream.flush()

    def progress(self, step, pct, msg, eta=None):
        self._write(progress_line(step, self.ids[step - 1], pct, msg, eta,
                                  steps=len(self.ids), ids=self.ids))

    def ok(self, pack_dir):
        self._write(result_ok(pack_dir))

    def ok_exe(self, exe):
        self._write(result_ok_exe(exe))

    def ok_project(self, proj):
        self._write(result_ok_project(proj))

    def error(self, code, msg):
        self._write(result_error(code, msg))
