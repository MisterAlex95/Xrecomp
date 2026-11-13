"""
Library triage (XBA-63, XBA-124): what each disc is built with, what in its
image the runtime has to handle, and how the library groups, from the XBEs
alone (no disassembly, no boot).

    python -m tools.triage /dumps/*.iso /dumps/SomeGame/ [--tdata DIR] [--json triage.json] [--markdown triage.md]

For each disc (an .iso/.xiso image, or a folder of extracted files):

  * default.xbe: title, title ID, XDK build, libraries with their builds,
    engines, profile layers (tools.xbe_ident);
  * its image (tools.xbe_parser.image_report): the key pair (retail/debug),
    the section count and size, the sections not marked preload (loaded on
    demand by XeLoadSection: resident from boot here), sections that share a
    virtual address (code in both is a blocker: only one is lifted), TLS
    or not (a TLS-less image gets a TIB per thread all the same), what the
    parser had to work around;
  * the kernel imports whose route is still a stub (src/kernel/
    kernel_stub_table.c, tools/kernel_audit/stubs.py): what the title calls
    that does nothing yet;
  * tools.xbe_ident's code probes (d3d_latch_wait: D3D's fence wait spins
    on the 0x0310 latch, which picks the DMA walk for D3D8LTCG);
  * every other .xbe on the disc, and which of them default.xbe names in its
    strings ("d:\\menu.xbe", ...): a disc that launches another image needs
    multi-XBE support (XBA-82) before anything after the launch runs;
  * a title update (an XBE under $u, TDATA\\<title id>\\$u on the console's
    hard disk) and downloadable content ($c): looked for in a disc folder and
    under --tdata (a copy of the console's E:\\TDATA, or the runtime's save
    folder's TDATA). Neither is used by the runtime (docs/formats/xbe.md
    « Title updates and downloadable content »): reported so they are not
    silently ignored;
  * whether profiles/titles/<title id> exists.

Then the groups: titles by XDK build and by engine, biggest first, which is
the order to write shared fixes in (a layer helps every title of its group);
the libraries by build, and the stub kernel routes by how many titles import
them.

Nothing from the game is written out: names, numbers, our marker strings and
the disc's own file names. The JSON keys and the Markdown columns are stable:
a later run on the same library diffs cleanly.
"""

import os
import re
import tempfile

from tools.xbe_ident import blockers, identify, summary

XBE_NAME = re.compile(rb"[A-Za-z0-9_\\/:.\- ]{1,64}\.xbe", re.I)
ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
STUB_TABLE_C = os.path.join(ROOT, "src", "kernel", "kernel_stub_table.c")


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


def kernel_stubs(path=STUB_TABLE_C):
    """{ordinal: name} of the kernel routes that are still stubs (the generated
    table); {} when the file is not there (an app's toolkit without src/)."""
    try:
        with open(path, encoding="utf-8") as f:
            text = f.read()
    except OSError:
        return {}
    return {int(o): n for o, n in re.findall(r'\{\s*(\d+)\s*,\s*"([A-Za-z0-9_]+)"\s*\}', text)}


def _is_update_path(rel):
    """An XBE under a $u folder: a title update, not one of the disc's images."""
    return any(c.lower() == "$u" for c in rel.split("/")[:-1])


def _xbes_in_folder(root):
    for d, _, files in os.walk(root):
        for fn in sorted(files):
            if fn.lower().endswith(".xbe"):
                p = os.path.join(d, fn)
                yield os.path.relpath(p, root).replace(os.sep, "/"), p


def _content_dirs(root):
    """Downloadable content packages under root: the sub-folders of every $c folder."""
    n = []
    for d, dirs, _ in os.walk(root):
        if os.path.basename(d).lower() == "$c":
            n += sorted(dirs)
            dirs[:] = []
    return n


def _tdata_title(tdata, tid):
    """tdata/<title id> in whatever case it was written, or None."""
    if not tdata or not os.path.isdir(tdata):
        return None
    for name in os.listdir(tdata):
        if name.lower() == tid.lower() and os.path.isdir(os.path.join(tdata, name)):
            return os.path.join(tdata, name)
    return None


def _update_record(rel, path, ident):
    x = {"path": rel, "used": False}
    try:
        i = identify(path)
        x.update(title_id=i["title_id"], version=i["version"], xdk=i["xdk"]["build"],
                 same_title=i["title_id"] == ident["title_id"], newer=i["version"] > ident["version"])
    except Exception as e:      # a damaged update must not stop the triage
        x["error"] = str(e)[:120]
    return x


def triage_disc(source, profiles_root=None, tdata=None, stubs=None):
    """One disc's record (see the module docstring); "error" set when it
    could not be read. tdata: a TDATA folder to look for this title's update
    and content in. stubs: kernel_stubs() (read once by the caller)."""
    from tools.prepare import profiles
    rec = {"source": os.path.basename(os.path.normpath(source)), "xbes": []}
    tmp = None
    stubs = kernel_stubs() if stubs is None else stubs
    try:
        if os.path.isdir(source):
            items = list(_xbes_in_folder(source))
        elif source.lower().endswith((".iso", ".xiso")):
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
        img = ident["image"]
        rec.update(title=ident["title"], title_id=ident["title_id"], version=ident["version"],
                   xdk=ident["xdk"]["build"], xdk_mixed=ident["xdk"]["mixed"],
                   libraries=dict(sorted(ident["xdk"]["libraries"].items())),
                   engines=[e["name"] for e in ident["engines"] if e["kind"] == "engine"],
                   middleware=[e["name"] for e in ident["engines"] if e["kind"] != "engine"],
                   layers=ident["layers"], summary=summary(ident),
                   key=img["key"], sections=img["section_count"], section_bytes=img["section_bytes"],
                   headers_size=img["headers_size"], not_preloaded=img["not_preloaded"],
                   overlaps=img["overlaps"], tls=img["tls"], warnings=img["warnings"],
                   section_table=img["sections"],
                   kernel_imports=len(ident["kernel_imports"]),
                   stub_imports=sorted(stubs[o] for o in ident["kernel_imports"] if o in stubs),
                   probes=ident["probes"], blockers=blockers(ident),
                   title_profile=profiles.load_layer("titles/%s" % ident["title_id"].lower(),
                                                     profiles_root or profiles.PROFILES) is not None)
        updates, content = [], []
        for rel, p in items:
            if rel.lower() == "default.xbe":
                continue
            if _is_update_path(rel):
                updates.append(_update_record(rel, p, ident))
                continue
            x = {"path": rel, "named_by_default": os.path.basename(rel).lower() in named}
            try:
                i = identify(p)
                x.update(title_id=i["title_id"], xdk=i["xdk"]["build"])
            except Exception as e:      # a damaged secondary image must not stop the triage
                x["error"] = str(e)[:120]
            rec["xbes"].append(x)
        if os.path.isdir(source):
            content += _content_dirs(source)
        t = _tdata_title(tdata, ident["title_id"])
        if t:
            for rel, p in _xbes_in_folder(t):
                if _is_update_path(rel):
                    updates.append(_update_record("TDATA/%s/%s" % (os.path.basename(t), rel), p, ident))
            content += _content_dirs(t)
        rec["updates"] = updates
        rec["content"] = sorted(set(content))
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
    """{"xdk": [(build, [titles])...], "engine": [...], "library": [("D3D8 5849", [...])...],
    "stub": [(name, [titles])...]}, biggest first."""
    by_xdk, by_engine, by_lib, by_stub = {}, {}, {}, {}
    for r in records:
        if r.get("error"):
            continue
        name = r.get("title") or r["source"]
        by_xdk.setdefault(r.get("xdk") or 0, []).append(name)
        for e in r.get("engines") or ["(no known engine)"]:
            by_engine.setdefault(e, []).append(name)
        for lib, build in (r.get("libraries") or {}).items():
            by_lib.setdefault("%s %d" % (lib, build), []).append(name)
        for s in r.get("stub_imports") or []:
            by_stub.setdefault(s, []).append(name)
    order = lambda d: sorted(d.items(), key=lambda kv: (-len(kv[1]), str(kv[0])))
    return {"xdk": order(by_xdk), "engine": order(by_engine), "library": order(by_lib), "stub": order(by_stub)}


def _mb(n):
    return "%.1f MB" % (n / 1048576.0)


def _cell(s):
    return str(s).replace("|", "/").replace("\n", " ")


def markdown(records):
    """The table for the « Compatibility » page, then the groups."""
    rows = ["| Title | Title ID | XDK | Engines | Image | On demand | Overlaps | TLS | Stub imports | D3D latch wait "
            "| Other XBEs (named by default.xbe) | Update / DLC | Profile |",
            "|---|---|---|---|---|---|---|---|---|---|---|---|---|"]
    for r in sorted(records, key=lambda r: (r.get("title") or r["source"]).lower()):
        if r.get("error"):
            rows.append("| %s | | | | | | | | | | %s | | |" % (_cell(r["source"]), "error: " + _cell(r["error"])))
            continue
        others = ", ".join(("**%s**" % x["path"]) if x["named_by_default"] else x["path"] for x in r["xbes"])
        image = "%d sections, %s%s%s" % (r["sections"], _mb(r["section_bytes"]),
                                         ", debug keys" if r["key"] == "debug" else
                                         ", unknown keys" if r["key"] == "unknown" else "",
                                         ", %d warning(s)" % len(r["warnings"]) if r["warnings"] else "")
        overlaps = ", ".join("%s/%s%s" % (o["a"], o["b"], " (code: **blocker**)" if o["code"] else "")
                             for o in r["overlaps"])
        upd = []
        for u in r.get("updates", []):
            upd.append("update %s%s (not used)" % (u["path"], " v%d" % u["version"] if "version" in u else ""))
        if r.get("content"):
            upd.append("%d content package(s)" % len(r["content"]))
        latch = r["probes"].get("d3d_latch_wait", 0)
        rows.append("| %s | %s | %s%s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s |" % (
            _cell(r["title"]), r["title_id"], r["xdk"] or "?", " (mixed)" if r["xdk_mixed"] else "",
            _cell(", ".join(r["engines"])), image, _cell(", ".join(r["not_preloaded"])), _cell(overlaps),
            "yes" if r["tls"] else "no", _cell(", ".join(r["stub_imports"])), "yes" if latch else "",
            _cell(others), _cell("; ".join(upd)), "yes" if r["title_profile"] else ""))
    g = groups(records)
    rows += ["", "**By XDK build** (biggest first): " + "; ".join(
        "%s: %d" % (k or "unknown", len(v)) for k, v in g["xdk"])]
    rows += ["**By engine**: " + "; ".join("%s: %d" % (k, len(v)) for k, v in g["engine"])]
    rows += ["**Libraries by build**: " + "; ".join("%s: %d" % (k, len(v)) for k, v in g["library"])]
    rows += ["**Stub kernel routes imported** (titles): " + ("; ".join(
        "%s: %d" % (k, len(v)) for k, v in g["stub"]) or "none")]
    ok = [r for r in records if not r.get("error")]
    multi = [r for r in records if r.get("multi_xbe")]
    rows += ["**Discs that launch another XBE** (XBA-82): %d of %d" % (len(multi), len(records))]
    rows += ["**Images with sections loaded on demand**: %d of %d" % (
        sum(1 for r in ok if r["not_preloaded"]), len(ok))]
    rows += ["**Images without TLS** (a TIB per thread all the same): %d of %d" % (
        sum(1 for r in ok if not r["tls"]), len(ok))]
    rows += ["**Title updates found** (not used): %d; **content packages**: %d" % (
        sum(len(r.get("updates", [])) for r in ok), sum(len(r.get("content", [])) for r in ok))]
    blocked = [(r.get("title") or r["source"], b) for r in ok for b in r["blockers"]]
    rows += ["**Blockers**: " + ("; ".join("%s: %s" % (t, b) for t, b in blocked) if blocked else "none")]
    return "\n".join(rows)
