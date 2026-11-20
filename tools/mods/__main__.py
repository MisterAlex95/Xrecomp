"""
The mod store from the command line (docs/technical/mods.md):

    python -m tools.mods check FOLDER [--console xbox --title 58420001] [--binary FILE]
    python -m tools.mods install FOLDER                  copy a checked mod into the store
    python -m tools.mods list    --console xbox --title 58420001
    python -m tools.mods enable  --console xbox --title 58420001 MOD [--at N]
    python -m tools.mods disable --console xbox --title 58420001 MOD
    python -m tools.mods hash    --console xbox --title 58420001

--mods DIR is the store (default $XBA_MODS_DIR, else <user data>/mods). Exit 0, or 2 with the reason on stderr.
"""

import argparse
import hashlib
import os
import shutil
import sys

from tools.mods import package, store
from tools.mods.package import ModError


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def main(argv=None):
    ap = argparse.ArgumentParser(prog="tools.mods", description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("command", choices=("check", "install", "list", "enable", "disable", "hash"))
    ap.add_argument("mod", nargs="?", help="a mod folder (check, install) or a mod id (enable, disable)")
    ap.add_argument("--mods", help="the mod store (default: $XBA_MODS_DIR, else <user data>/mods)")
    ap.add_argument("--console", default="xbox", choices=package.CONSOLES)
    ap.add_argument("--title", help="the title ID / game key")
    ap.add_argument("--binary", help="check: the game binary the mod must fit")
    ap.add_argument("--at", type=int, help="enable: the place in the load order (0 first)")
    a = ap.parse_args(argv)
    root = a.mods or store.default_root()
    try:
        if a.command in ("check", "install"):
            if not a.mod:
                ap.error("%s needs a mod folder" % a.command)
            m = package.load(a.mod, a.console if a.title else None, a.title,
                             sha256_file(a.binary) if a.binary else None)
            print("%s %s: %s (%s %s)%s" % (m.id, m.version, m.name, m.console, m.title_id,
                                           ", %d patch(es)" % len(m.patches) if m.patches else ""))
            if a.command == "install":
                dest = os.path.join(store.title_dir(root, m.console, m.title_id), m.id)
                if os.path.exists(dest):
                    shutil.rmtree(dest)
                shutil.copytree(a.mod, dest, symlinks=False)
                package.load(dest, m.console, m.title_id)
                print("installed in %s (off: tools.mods enable to turn it on)" % dest)
            return 0
        if not a.title:
            ap.error("%s needs --title" % a.command)
        if a.command == "list":
            on = store.read_enabled(root, a.console, a.title)
            for i in store.installed(root, a.console, a.title):
                try:
                    m = package.load(os.path.join(store.title_dir(root, a.console, a.title), i), a.console, a.title)
                    what = "%s %s" % (m.version, m.name)
                except ModError as e:
                    what = "NOT VALID: %s" % e
                print("%s %-24s %s" % ("%2d" % (on.index(i) + 1) if i in on else " -", i, what))
            for i in on:
                if i not in store.installed(root, a.console, a.title):
                    print("%2d %-24s NOT INSTALLED" % (on.index(i) + 1, i))
            return 0
        if a.command in ("enable", "disable"):
            if not a.mod:
                ap.error("%s needs a mod id" % a.command)
            ids = (store.enable(root, a.console, a.title, a.mod, a.at) if a.command == "enable"
                   else store.disable(root, a.console, a.title, a.mod))
            print("load order: %s" % (" ".join(ids) or "(no mod on)"))
            return 0
        mods = store.enabled_mods(root, a.console, a.title)
        print(store.mod_set_hash(mods) or "(no mod on)")
        return 0
    except ModError as e:
        print("error: %s" % e, file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
