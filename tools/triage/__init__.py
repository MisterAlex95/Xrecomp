"""
Library triage (XBA-63): what each disc is built with, and how the library
groups, from the XBEs alone (no disassembly, no boot).

    python -m tools.triage /dumps/*.iso /dumps/SomeGame/ [--json triage.json] [--markdown triage.md]

For each disc (an .iso/.xiso image, or a folder of extracted files):

  * default.xbe: title, title ID, XDK build, libraries, engines, profile
    layers (tools.xbe_ident);
  * every other .xbe on the disc, and which of them default.xbe names in its
    strings ("d:\\menu.xbe", ...): a disc that launches another image needs
    multi-XBE support (XBA-82) before anything after the launch runs;
  * whether profiles/titles/<title id> exists.

Then the groups: titles by XDK build and by engine, biggest first, which is
the order to write shared fixes in (a layer helps every title of its group).

Nothing from the game is written out: names, numbers, our marker strings and
the disc's own file names.
"""

import os
import re
import tempfile

from tools.xbe_ident import identify, summary

XBE_NAME = re.compile(rb"[A-Za-z0-9_\\/:.\- ]{1,64}\.xbe", re.I)


def _named_xbes(data):
    """The .xbe file names a binary mentions (ASCII and UTF-16LE), lowercased
    base names."""
    names = set()
    for m in XBE_NAME.finditer(data):
        names.add(os.path.basename(m.group(0).decode("ascii", "replace").replace("\\", "/")).lower())
    utf16 = re.compile(rb"(?:[A-Za-z0-9_\\/:.\- ]\x00){1,64}\.\x00x\x00b\x00e\x00", re.I)
    for m in utf16.finditer(data):
        s = m.group(0).decode("utf-16-le", "replace").replace("\\", "/")
        names.add(os.path.basename(s).lower())
    return names


def _xbes_in_folder(root):
    for d, _, files in os.walk(root):
        for fn in sorted(files):
            if fn.lower().endswith(".xbe"):
                p = os.path.join(d, fn)
                yield os.path.relpath(p, root).replace(os.sep, "/"), p


def disc_xbes(data_dir, work, named):
    """[(disc path, file)] for the disc's .xbe files whose base name is in
    `named`: from a folder as they are, from a disc image (XBA-126) copied out
    into <work>/disc-xbe/ (only those few files: nothing else is extracted)."""
    if os.path.isdir(data_dir):
        return [(rel, p) for rel, p in _xbes_in_folder(data_dir) if os.path.basename(rel).lower() in named]
    from tools.xiso.xdvdfs import Xiso
    out = []
    with Xiso(data_dir) as iso:
        for d, e in sorted(iso.walk(), key=lambda x: (x[0], x[1].name)):
            if not e.name.lower().endswith(".xbe") or e.name.lower() not in named:
                continue
            rel = (d + "/" + e.name).lstrip("/")
            p = os.path.join(work, "disc-xbe", *rel.split("/"))
            if not (os.path.isfile(p) and os.path.getsize(p) == e.size):
                iso.extract(e, p)
            out.append((rel, p))
    return out


def triage_disc(source, profiles_root=None):
    """One disc's record (see the module docstring); "error" set when it
    could not be read."""
    from tools.prepare import profiles
    from tools.xiso.source import is_image_name
    rec = {"source": os.path.basename(os.path.normpath(source)), "xbes": []}
    tmp = None
    try:
        if os.path.isdir(source):
            items = list(_xbes_in_folder(source))
        elif is_image_name(source):                                 # also split parts, .cso (XBA-126)
            from tools.xiso.xdvdfs import Xiso
            tmp = tempfile.mkdtemp(prefix="xba-triage-")
            items = []
            with Xiso(source) as iso:
                for d, e in iso.walk():
                    if e.name.lower().endswith(".xbe"):
                        rel = (d + "/" + e.name).lstrip("/")
                        out = os.path.join(tmp, "%d.xbe" % len(items))
                        iso.extract(e, out)
                        items.append((rel, out))
        elif source.lower().endswith(".xbe"):
            items = [(os.path.basename(source), source)]
        else:
            rec["error"] = "not a disc image, a folder or an XBE"
            return rec
        main = next((p for rel, p in items if rel.lower() == "default.xbe"), None)
        if main is None:
            rec["error"] = "no default.xbe"
            return rec
        ident = identify(main)
        with open(main, "rb") as f:
            named = _named_xbes(f.read())
        rec.update(title=ident["title"], title_id=ident["title_id"], xdk=ident["xdk"]["build"],
                   xdk_mixed=ident["xdk"]["mixed"], libraries=ident["xdk"]["libraries"],
                   engines=[e["name"] for e in ident["engines"] if e["kind"] == "engine"],
                   middleware=[e["name"] for e in ident["engines"] if e["kind"] != "engine"],
                   layers=ident["layers"], summary=summary(ident),
                   title_profile=profiles.load_layer("titles/%s" % ident["title_id"].lower(),
                                                     profiles_root or profiles.PROFILES) is not None)
        for rel, p in items:
            if rel.lower() == "default.xbe":
                continue
            x = {"path": rel, "named_by_default": os.path.basename(rel).lower() in named}
            try:
                i = identify(p)
                x.update(title_id=i["title_id"], xdk=i["xdk"]["build"])
            except Exception as e:      # a damaged secondary image must not stop the triage
                x["error"] = str(e)[:120]
            rec["xbes"].append(x)
        rec["multi_xbe"] = any(x["named_by_default"] for x in rec["xbes"])
    except Exception as e:
        rec["error"] = str(e)[:200]
    finally:
        if tmp:
            for fn in os.listdir(tmp):
                os.remove(os.path.join(tmp, fn))
            os.rmdir(tmp)
    return rec


def groups(records):
    """{"xdk": [(build, [titles])...], "engine": [(name, [titles])...]}, biggest first."""
    by_xdk, by_engine = {}, {}
    for r in records:
        if r.get("error"):
            continue
        name = r.get("title") or r["source"]
        by_xdk.setdefault(r.get("xdk") or 0, []).append(name)
        for e in r.get("engines") or ["(no known engine)"]:
            by_engine.setdefault(e, []).append(name)
    order = lambda d: sorted(d.items(), key=lambda kv: (-len(kv[1]), str(kv[0])))
    return {"xdk": order(by_xdk), "engine": order(by_engine)}


def markdown(records):
    """The table for the « Compatibility » page, then the groups."""
    rows = ["| Title | Title ID | XDK | Engines | Other XBEs (named by default.xbe) | Profile |",
            "|---|---|---|---|---|---|"]
    for r in sorted(records, key=lambda r: (r.get("title") or r["source"]).lower()):
        if r.get("error"):
            rows.append("| %s | | | | %s | |" % (r["source"], "error: " + r["error"].replace("|", "/")))
            continue
        others = ", ".join(("**%s**" % x["path"]) if x["named_by_default"] else x["path"] for x in r["xbes"])
        rows.append("| %s | %s | %s%s | %s | %s | %s |" % (
            r["title"].replace("|", "/"), r["title_id"], r["xdk"] or "?", " (mixed)" if r["xdk_mixed"] else "",
            ", ".join(r["engines"]) or "", others or "", "yes" if r["title_profile"] else ""))
    g = groups(records)
    rows += ["", "**By XDK build** (biggest first): " + "; ".join(
        "%s: %d" % (k or "unknown", len(v)) for k, v in g["xdk"])]
    rows += ["**By engine**: " + "; ".join("%s: %d" % (k, len(v)) for k, v in g["engine"])]
    multi = [r for r in records if r.get("multi_xbe")]
    rows += ["**Discs that launch another XBE** (XBA-82): %d of %d" % (len(multi), len(records))]
    return "\n".join(rows)
