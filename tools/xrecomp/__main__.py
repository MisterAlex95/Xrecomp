"""
xrecomp: prepare and play your own games from a terminal (Linux, macOS, Windows). docs/desktop/cli.md.

    xrecomp prepare SOURCE [--force] [--no-title-layer | --title-layer] [--no-build] [-- PREPARE ARGS]
    xrecomp play GAME [--print [--json]] [-- PLAYER ARGS]
    xrecomp list [FOLDER...] [--json]
    xrecomp identify SOURCE... [--json]
    xrecomp settings [GAME] [KEY=VALUE | KEY= ...] [--unset KEY] [--reset]
    xrecomp folders [add FOLDER | remove FOLDER]
    xrecomp where [GAME]

SOURCE is what `python -m tools.prepare` takes: an Xbox disc image, extracted disc folder or default.xbe, a GBA ROM,
a PS1 disc (.cue / .chd / .bin / .iso) or PS-X EXE, an N64 ROM (.z64 / .v64 / .n64). It is read where it is, never
copied. GAME is a source path, a title ID (12345678, gba-AXVE-1A2B3C4D) or a unique part of a title.

Files go to the platform's user data folder ($XRECOMP_HOME moves it; `xrecomp where` prints it).

Windows (XBA-89, experimental): Xbox games only. The GBA, PS1 and N64 runtimes are POSIX-only (src/host); prepare and
play refuse them there and say so.
"""

import argparse
import json
import os
import shlex
import subprocess
import sys
import time

if __package__ in (None, ""):        # run as a file: the toolkit root on the path
    sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..")))

from tools.xrecomp import config, library                    # noqa: E402
from tools.xrecomp.home import ROOT, Home, iso_now           # noqa: E402

PLAYERS = {"gba": "gba_play", "ps1": "ps1_play", "n64": "n64_play", "gc": "gc_play"}
RUN_LOGS_KEPT = 10


def err(msg):
    print("xrecomp: " + msg, file=sys.stderr, flush=True)


def console_unsupported(console, platform=None):
    """Why this host cannot prepare or play a console's games, or None. Windows: Xbox games only (XBA-89)."""
    platform = platform or sys.platform
    if console and console != "xbox" and platform.startswith("win"):
        return ("%s games are not supported on Windows: the console runtimes (GBA, PS1, N64, GameCube) are POSIX-only. "
                "Use Linux, macOS or WSL2 for them; Xbox games work here (docs/desktop/windows.md)"
                % console.upper())
    return None


# ── Python and the console players ──────────────────────────────

def python_for(home, env=None):
    """The Python for the pipeline: $XBA_PYTHON, settings.json python_path, the toolkit's .venv, else this one."""
    env = os.environ if env is None else env
    if env.get("XBA_PYTHON"):
        return env["XBA_PYTHON"]
    p = home.settings().get("python_path")
    if isinstance(p, str) and p:
        return os.path.expanduser(p)
    for rel in ((".venv", "bin", "python3"), (".venv", "Scripts", "python.exe")):
        v = os.path.join(ROOT, *rel)
        if os.path.isfile(v):
            return v
    return sys.executable


def find_player(console, env=None, root=ROOT):
    """gba_play / ps1_play / n64_play / gc_play: $XRECOMP_<CONSOLE>_PLAY, the PATH, a build folder of the toolkit,
    else a release archive's (library.find_exe). None when not built."""
    return library.find_exe(PLAYERS[console], console, env, root)


# ── prepare ─────────────────────────────────────────────────────

class ProgressPrinter:
    """Prints a prepare's output for a person: its own lines as they are, the protocol lines (XBA_PROGRESS /
    XBA_RESULT, tools/prepare/progress.py) as « [2/5] analyse 40 % ... » once per step and every 10 %."""

    def __init__(self, out=None):
        self.out, self.last = out or sys.stdout, None
        self.result = None

    def __call__(self, line):
        from tools.prepare.progress import parse_line
        p = parse_line(line)
        if p is None:
            self.out.write(line if line.endswith("\n") else line + "\n")
        elif p["kind"] == "result":
            self.result = p
        else:
            key = (p["step"], p["pct"] // 10)
            if key != self.last or p["pct"] == 100:
                self.last = key
                self.out.write("[%d/%d] %-8s %3d %%  %s\n" % (p["step"], p["steps"], p["id"], p["pct"],
                                                              p.get("msg", "")))
        self.out.flush()


def prepare_command(home, e, a, py):
    """The tools.prepare command line for an Entry (its public entry: either compile route, CMake or XBA-86's)."""
    t = home.title(e.id)
    args = [py, "-u", "-m", "tools.prepare", e.source, "--work", t.work]
    if e.console != "xbox":
        args += ["--pack-dir", home.packs]
    if a.force:
        args.append("--force")
    if a.no_build:
        args.append("--no-build")
    return args + list(a.extra)


def cmd_prepare(home, a):
    if os.path.exists(os.path.expanduser(a.source)):
        e = library.identify(a.source)
    else:                   # a game of the library, by its ID or title: prepare it again
        try:
            e = library.resolve(home, a.source)
        except (LookupError, library.Ambiguous) as x:
            err(str(x))
            return 1
    if e.error:
        err("%s: %s" % (a.source, e.error))
        return 1
    why = console_unsupported(e.console)
    if why:
        err(why)
        return 1
    t = home.title(e.id)
    st = t.state()
    if a.title_layer is not None:
        st["enhancements"] = a.title_layer
    enhancements = st.get("enhancements") is not False
    py = python_for(home)
    args = prepare_command(home, e, a, py)
    if not enhancements:
        args.append("--no-title-layer")
    env = dict(os.environ)
    env.pop("XBA_NO_TITLE_LAYER", None)
    print("%s  %s (%s)\n  source: %s\n  work:   %s" % (e.id, e.title, e.console, e.source, t.work), flush=True)
    if a.print:
        print(" ".join(shlex.quote(x) for x in args))
        return 0
    os.makedirs(t.work, exist_ok=True)
    t.update_state(source_path=e.source, title=e.title, console=e.console,
                   enhancements=st.get("enhancements"))
    printer = ProgressPrinter()
    p = subprocess.Popen(args, cwd=ROOT, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                         universal_newlines=True, errors="replace", bufsize=1)
    try:
        for line in p.stdout:
            printer(line)
        rc = p.wait()
    except KeyboardInterrupt:
        p.terminate()       # tools.prepare stops its children and answers CANCELLED
        try:
            rc = p.wait(timeout=10)
        except subprocess.TimeoutExpired:
            p.kill()
            rc = p.wait()
        rc = rc or 130
    res = printer.result or {}
    ok = rc == 0
    rec = {"ok": ok, "at": iso_now()}
    if not ok:
        rec["code"] = res.get("code") or ("CANCELLED" if rc == 130 else "FAILED")
        rec["message"] = res.get("msg") or "tools.prepare exit %d (see %s)" % (rc, os.path.join(t.work, "prepare.log"))
    t.update_state(last_prepare=rec)
    if ok and a.no_build:
        print("Lifted, not built: %s" % (res.get("project") or os.path.join(t.work, "project")))
        return 0
    if ok:
        library.state(e, home)
        where = e.exe or e.pack_lib or res.get("project") or res.get("pack") or t.work
        print("Prepared: %s %s\n  %s\n  play it: xrecomp play %s" % (e.id, e.title, where, e.id))
        return 0
    err("prepare failed: %s: %s" % (rec["code"], rec["message"]))
    return rc if rc else 1


# ── play ────────────────────────────────────────────────────────

def play_request(home, e, extra=(), inherited=None):
    """(argv, cwd, env) for a game, or raises LookupError with what is missing."""
    inherited = dict(os.environ if inherited is None else inherited)
    t = home.title(e.id)
    defaults = config.RecompConfig.load(home.defaults_dir)
    game = config.RecompConfig.load(t.cfg_dir)
    if e.state is None:
        library.state(e, home)
    if e.console == "xbox":
        if not e.exe:
            raise LookupError("%s %s is not prepared: xrecomp prepare %s" % (e.id, e.title, shlex.quote(e.source)))
        env = config.game_env(inherited, defaults, game, t.save)
        return [e.exe] + list(extra), os.path.dirname(e.exe), env
    why = console_unsupported(e.console)
    if why:
        raise LookupError(why)
    player = find_player(e.console, inherited)
    if not player:
        raise LookupError("%s is not built (a release archive has it in runtime/<host>/bin): cmake -S . -B build "
                          "&& cmake --build build --target %s (or XRECOMP_%s_PLAY=/path/to/%s)" % (PLAYERS[e.console], PLAYERS[e.console],
                                                                e.console.upper(), PLAYERS[e.console]))
    env = config.game_env(inherited, defaults, game, t.save)
    argv = [player, e.source]
    if e.pack_lib:          # prepared (an older toolkit's pack still loads): native, and what it misses is kept
        os.makedirs(t.work, exist_ok=True)
        argv += ["--pack", e.pack_lib, "--missed", t.discovered]
    # The runtimes look for build/ps1-bios/openbios.bin from their working directory: the toolkit's.
    return argv + list(extra), ROOT, env


def env_changes(inherited, env):
    """What a game's environment changes: [(key, value or None for unset)], sorted."""
    keys = set(inherited) | set(env)
    return sorted((k, env.get(k)) for k in keys if inherited.get(k) != env.get(k))


def new_run_log(t, now=None):
    os.makedirs(t.logs, exist_ok=True)
    logs = sorted(n for n in os.listdir(t.logs) if n.startswith("run-") and n.endswith(".log"))
    for n in logs[:max(0, len(logs) - (RUN_LOGS_KEPT - 1))]:
        try:
            os.remove(os.path.join(t.logs, n))
        except OSError:
            pass
    stamp = time.strftime("%Y%m%d-%H%M%S", time.localtime(now))
    path, n = os.path.join(t.logs, "run-%s.log" % stamp), 1
    while os.path.exists(path):
        n += 1
        path = os.path.join(t.logs, "run-%s-%d.log" % (stamp, n))
    return path


def cmd_play(home, a):
    try:
        e = library.resolve(home, a.game)
        library.state(e, home)
        argv, cwd, env = play_request(home, e, a.extra)
    except LookupError as x:
        err(str(x))
        return 1
    except library.Ambiguous as x:
        err(str(x))
        return 2
    changes = env_changes(os.environ, env)
    if a.json:              # the macOS launcher's way in (XBA-278): it starts the player itself
        print(json.dumps({"id": e.id, "console": e.console, "title": e.title, "argv": argv, "cwd": cwd,
                          "env": env, "changes": dict(changes)}, indent=2))
        return 0
    if a.print:
        print("cd %s" % shlex.quote(cwd))
        for k, v in changes:
            print("unset %s" % k if v is None else "export %s=%s" % (k, shlex.quote(v)))
        print(" ".join(shlex.quote(x) for x in argv))
        return 0
    t = home.title(e.id)
    if e.state == library.STALE:
        print("note: %s" % e.note, flush=True)
    if e.console == "xbox":
        os.makedirs(t.save, exist_ok=True)
    t.update_state(source_path=e.source, title=e.title, console=e.console)
    log = new_run_log(t)
    print("%s  %s (%s)\n  log: %s" % (e.id, e.title, e.label, log), flush=True)
    t0 = time.time()
    with open(log, "w") as f:
        f.write("# %s\n" % " ".join(shlex.quote(x) for x in argv))
        for k, v in changes:
            f.write("# %s\n" % ("unset " + k if v is None else "%s=%s" % (k, v)))
        f.flush()
        p = subprocess.Popen(argv, cwd=cwd, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                             universal_newlines=True, errors="replace", bufsize=1)
        try:
            for line in p.stdout:
                sys.stdout.write(line)
                sys.stdout.flush()
                f.write(line)
            rc = p.wait()
        except KeyboardInterrupt:
            p.terminate()           # by PID, then SIGKILL after 5 s (the Mac app's Stop)
            try:
                rc = p.wait(timeout=5)
            except subprocess.TimeoutExpired:
                p.kill()
                rc = p.wait()
        secs = time.time() - t0
        f.write("# ended: exit %s after %d s\n" % (rc, secs))
    st = t.state()
    t.update_state(play_seconds=float(st.get("play_seconds") or 0) + secs, runs=int(st.get("runs") or 0) + 1,
                   last_played=iso_now())
    if rc != 0:
        err("%s stopped unexpectedly (exit %s); log: %s" % (e.title, rc, log))
    library.state(e, home)
    if "discovered" in e.reasons or (e.console == "xbox" and e.state == library.STALE):
        print("note: %s" % e.note)
    return 0 if rc == 0 else 1


# ── list ────────────────────────────────────────────────────────

def cmd_list(home, a):
    entries = library.library(home, a.folders)
    if a.json:
        print(json.dumps([e.as_dict() for e in entries], indent=2))
        return 0
    if not entries:
        print("No games yet. xrecomp prepare SOURCE, or xrecomp folders add FOLDER (then xrecomp list).")
        return 0
    rows = [(e.id or "?", e.console or "?", e.label, e.title or os.path.basename(e.source),
             e.note or e.error or "") for e in entries]
    w = [max(len(r[i]) for r in rows) for i in range(4)]
    for r in rows:
        print("%-*s  %-*s  %-*s  %-*s  %s" % (w[0], r[0], w[1], r[1], w[2], r[2], w[3], r[3], r[4]))
    return 0


# ── identify ────────────────────────────────────────────────────

def identify_dict(home, src):
    """What a source is and how prepared (Entry.as_dict, after state()), with its key and, for a console game, the
    player that runs it (null: not built) and the folder its saves go to."""
    e = library.identify(src)
    library.state(e, home)
    d = e.as_dict()
    d["key"] = e.key
    if e.console and e.console != "xbox" and not e.error:
        d["player"] = find_player(e.console)
        d["unsupported"] = console_unsupported(e.console)
        d["save_dir"] = home.title(e.id).save
        d["pack_dir"] = home.title(e.id).pack
    return d


def cmd_identify(home, a):
    """Each SOURCE (the macOS launcher's console games, XBA-278: it asks here rather than reading ROM headers and
    pack.json itself, so the two never disagree)."""
    out = [identify_dict(home, src) for src in a.sources]
    if a.json:
        print(json.dumps(out, indent=2))
        return 0
    for d in out:
        print("%-24s %-5s %-24s %s" % (d["id"] or "?", d["console"] or "?", d["label"] or d["error"] or "",
                                       d["title"] or d["source"]))
    return 0 if all(not d["error"] for d in out) else 1


# ── settings ────────────────────────────────────────────────────

def cmd_settings(home, a):
    items = list(a.items)
    game = None
    if items and "=" not in items[0]:
        ref = items.pop(0)
        try:
            game = library.resolve(home, ref)
        except (LookupError, library.Ambiguous) as x:
            err(str(x))
            return 1
    try:
        assigns = [config.parse_assignment(x) for x in items]
        unsets = [config.parse_assignment(k + "=")[0] for k in a.unset]
    except ValueError as x:
        err(str(x))
        return 2
    defaults = config.RecompConfig.load(home.defaults_dir)
    target = config.RecompConfig.load(home.title(game.id).cfg_dir) if game else defaults
    changed = bool(assigns or unsets or a.reset)
    if a.reset:
        target.clear()
    for k in unsets:
        target.set(k, None)
    for k, v in assigns:
        # On Default settings an empty value is the runtime default: no line. On a game it is a line of its own.
        target.set(k, None if (game is None and v == "") else v)
    if changed:
        target.save()
        if game is None:        # the Mac app keeps a copy in settings.json
            s = home.settings()
            s["global_defaults"] = dict(target.pairs())
            if os.path.exists(home.settings_file) or s["global_defaults"]:
                home.save_settings(s)
        elif not os.path.exists(home.title(game.id).state_file):
            home.title(game.id).update_state(source_path=game.source, title=game.title, console=game.console)
    if game is None:
        print("Default settings (%s):" % target.path)
        for k, v in target.pairs():
            print("  %s=%s" % (k, v))
        if not target.pairs():
            print("  (none: every option at the runtime's default)")
        return 0
    forced = {}
    if game.console == "xbox":
        library.state(game, home)
        forced = game.env_force
    print("%s  %s: %s" % (game.id, game.title, target.path))
    keys = [k for k, _ in target.pairs()] + [k for k, _ in defaults.pairs() if target.get(k) is None]
    keys += [k for k in sorted(forced) if k not in keys]
    if not keys:
        print("  (as in Default settings, which set nothing)")
    for k in keys:
        if k in forced:
            print("  %s=%s  (set by the title)" % (k, forced[k]))
            continue
        v, where = config.effective(target, defaults, k)
        label = {"game": "this game", "defaults": "as in Default settings",
                 "runtime": "runtime default, this game"}[where]
        print("  %s=%s  (%s)" % (k, "" if v is None else v, label))
    return 0


# ── folders, where ──────────────────────────────────────────────

def cmd_folders(home, a):
    s = home.settings()
    folders = list(home.library_folders())
    if a.action:
        if not a.folder:
            err("folders %s needs a FOLDER" % a.action)
            return 2
        f = os.path.abspath(os.path.expanduser(a.folder))
        if a.action == "add" and f not in folders:
            folders.append(f)
        elif a.action == "remove":
            folders = [x for x in folders if x != f]
        s["library_folders"] = folders
        home.save_settings(s)
    for f in folders:
        print(f)
    if not folders:
        print("(no library folder: xrecomp folders add FOLDER)")
    return 0


def cmd_where(home, a):
    if not a.game:
        print(home.root)
        return 0
    try:
        e = library.resolve(home, a.game)
    except (LookupError, library.Ambiguous) as x:
        err(str(x))
        return 1
    t = home.title(e.id)
    for k, v in (("title", t.dir), ("work", t.work), ("settings", os.path.join(t.cfg_dir, "recomp.cfg")),
                 ("saves", t.save), ("logs", t.logs), ("pack", t.pack), ("discovered", t.discovered)):
        if v:
            print("%-10s %s" % (k, v))
    return 0


# ── main ────────────────────────────────────────────────────────

def parser():
    ap = argparse.ArgumentParser(prog="xrecomp", description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--home", help="the data folder (default: $XRECOMP_HOME, else the platform's)")
    sub = ap.add_subparsers(dest="cmd", metavar="COMMAND")
    sub.required = True

    p = sub.add_parser("prepare", help="prepare a game (tools.prepare)")
    p.add_argument("source", help="a disc image, disc folder, default.xbe or ROM; or a game of the library")
    p.add_argument("--force", action="store_true", help="redo every step")
    p.add_argument("--no-build", action="store_true", help="Xbox: stop after the C lift")
    g = p.add_mutually_exclusive_group()
    g.add_argument("--no-title-layer", dest="title_layer", action="store_false", default=None,
                   help="leave the title's optional enhancements out (profiles/titles/<id>), remembered")
    g.add_argument("--title-layer", dest="title_layer", action="store_true", help="put them back in")
    p.add_argument("--print", action="store_true", help="print the tools.prepare command, do not run it")
    p.add_argument("extra", nargs="*", help="after --: more tools.prepare arguments")

    p = sub.add_parser("play", help="play a game")
    p.add_argument("game")
    p.add_argument("--print", action="store_true", help="print the command line and environment, do not run it")
    p.add_argument("--json", action="store_true", help="with --print: as JSON (argv, cwd, the whole environment)")
    p.add_argument("extra", nargs="*", help="after --: arguments for the game or the console player")

    p = sub.add_parser("list", help="the library and how prepared each game is")
    p.add_argument("folders", nargs="*", help="also scan these folders")
    p.add_argument("--json", action="store_true")

    p = sub.add_parser("identify", help="what sources are (console, ID, title) and how prepared")
    p.add_argument("sources", nargs="+")
    p.add_argument("--json", action="store_true")

    p = sub.add_parser("settings", help="Default settings, or a game's")
    p.add_argument("items", nargs="*", metavar="[GAME] KEY=VALUE")
    p.add_argument("--unset", action="append", default=[], metavar="KEY",
                   help="remove the key's line (a game then follows Default settings)")
    p.add_argument("--reset", action="store_true", help="remove every setting line (comments stay)")

    p = sub.add_parser("folders", help="the library folders list scans")
    p.add_argument("action", nargs="?", choices=("add", "remove"))
    p.add_argument("folder", nargs="?")

    p = sub.add_parser("where", help="the data folder, or a game's folders")
    p.add_argument("game", nargs="?")
    return ap


def main(argv=None):
    a = parser().parse_args(argv)
    home = Home(a.home)
    return {"prepare": cmd_prepare, "play": cmd_play, "list": cmd_list, "identify": cmd_identify,
            "settings": cmd_settings,
            "folders": cmd_folders, "where": cmd_where}[a.cmd](home, a)


if __name__ == "__main__":
    sys.exit(main())
