"""
Profiles in layers (XBA-66): what tools.prepare adds to the generic template
for one XBE, from profiles/ in the toolkit. docs/technical/profiles.md has the
rules; this module only reads the tree and merges it.

    profiles/xdk/<name>/layer.json       an XDK library (D3D8, DSOUND, XAPI...),
                                         picked by the XBE's library table
    profiles/engines/<name>/layer.json   an engine, picked by tools.xbe_ident's
                                         engines.json (its "layer")
    profiles/titles/<title id>/title.json  one title

Lowest first: toolkit (the runtime and the lifter themselves, no folder) ->
xdk -> engines -> title. A higher layer's value wins over a lower one's.

A layer's JSON (every key optional):

    "name"            what it is
    "applies"         xdk only: {"libraries": ["D3D8", ...], "builds": [min, max],
                      "probes": ["d3d_latch_wait", ...]} (no "builds": every build of
                      those libraries; "probes": tools.xbe_ident's code probes that
                      must all have matched, XBA-105)
    "hooks"           a C prefix: the layer's .c files define <prefix>_configure(void)
                      and <prefix>_start(const char *game_dir, const char *save_dir)
    "env_default"     {RECOMP_*: value} set before the hooks when nothing (the
                      environment, recomp.cfg) has set them; "env_default_android"
                      and "env_default_desktop" for one platform only
    "env_force"       {RECOMP_*: value} forced over everything (pack.env_force_of)
    "params"          {XBA_*: number} #defines in xba_params.h for the lower layers'
                      C (a title gives the addresses an XDK layer's code needs)
    "seed_functions"  files in the layer (tools.seed_from_log's format) for the disassembler
    "exclude_manual"  a .c file in the layer whose functions replace the generated ones
    "layers"          titles: the layers the title was proven with; picked even
                      when the XBE does not select them (and prepare says so)
    "project", "status"  titles: the CMake project name, a one-line state
"""

import json
import os
import re

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
PROFILES = os.path.join(ROOT, "profiles")
KINDS = ("xdk", "engines", "titles")

ENV_KEY = re.compile(r"RECOMP_[A-Z0-9_]+\Z")
PARAM_KEY = re.compile(r"XBA_[A-Z0-9_]+\Z")
C_IDENT = re.compile(r"[A-Za-z_][A-Za-z0-9_]*\Z")
PLATFORMS = ("android", "desktop")


class ProfileError(ValueError):
    pass


def _json_name(kind):
    return "title.json" if kind == "titles" else "layer.json"


def load_layer(key, root=PROFILES):
    """{"key", "kind", "dir", "json"} for "xdk/d3d8", or None when there is no such layer."""
    kind, _, name = key.partition("/")
    if kind not in KINDS or not name or "/" in name or name.startswith("."):
        raise ProfileError("%s: not a layer name (xdk/<name>, engines/<name>, titles/<title id>)" % key)
    d = os.path.join(root, kind, name)
    path = os.path.join(d, _json_name(kind))
    if not os.path.isfile(path):
        return None
    try:
        with open(path, encoding="utf-8") as f:
            obj = json.load(f)
    except ValueError as e:
        raise ProfileError("%s/%s: %s" % (key, _json_name(kind), e))
    if not isinstance(obj, dict):
        raise ProfileError("%s/%s: an object expected" % (key, _json_name(kind)))
    return {"key": key, "kind": kind, "dir": d, "json": obj}


def _applies(layer, ident):
    """An xdk layer's "applies" against tools.xbe_ident's answer."""
    a = layer["json"].get("applies") or {}
    want = {n.upper() for n in a.get("libraries", [])}
    if not want:
        return False
    builds = a.get("builds")
    found = (ident or {}).get("probes") or {}
    if any(not found.get(p) for p in a.get("probes", [])):
        return False
    for name, build in ((ident or {}).get("xdk", {}).get("libraries") or {}).items():
        if name.upper() in want and (not builds or builds[0] <= build <= builds[1]):
            return True
    return False


def _names(root, kind):
    d = os.path.join(root, kind)
    try:
        return sorted(n for n in os.listdir(d) if os.path.isfile(os.path.join(d, n, _json_name(kind))))
    except OSError:
        return []


def auto_layers(ident, root=PROFILES):
    """The xdk and engine layers this XBE selects (keys, lowest first), from
    tools.xbe_ident.identify's answer; [] without one."""
    if not ident:
        return []
    keys = [k for k in ("xdk/" + n for n in _names(root, "xdk")) if _applies(load_layer(k, root), ident)]
    for e in ident.get("engines", []):
        k = "engines/%s" % e["layer"]
        if e.get("kind") == "engine" and k not in keys and load_layer(k, root):
            keys.append(k)
    return keys


def _check_env(key, obj, field):
    env = obj.get(field) or {}
    if not isinstance(env, dict):
        raise ProfileError("%s: %s: an object of RECOMP_* names to strings expected" % (key, field))
    out = {}
    for k, v in env.items():
        if not ENV_KEY.match(k) or len(k) > 63:
            raise ProfileError("%s: %s: %r is not a RECOMP_[A-Z0-9_]+ name" % (key, field, k))
        if isinstance(v, bool) or not isinstance(v, (str, int)):
            raise ProfileError("%s: %s: %s must be a string" % (key, field, k))
        v = str(v)
        if len(v) > 255 or any(not (0x20 <= ord(c) <= 0x7e) for c in v):
            raise ProfileError("%s: %s: %s=%r is not printable ASCII" % (key, field, k, v))
        out[k] = v
    return out


def _param_value(key, k, v):
    if isinstance(v, bool):
        raise ProfileError("%s: params: %s must be a number" % (key, k))
    if isinstance(v, str):
        try:
            v = int(v, 0)
        except ValueError:
            raise ProfileError("%s: params: %s=%r is not a number" % (key, k, v))
    if not isinstance(v, int) or not 0 <= v <= 0xFFFFFFFF:
        raise ProfileError("%s: params: %s must be a number from 0 to 0xFFFFFFFF" % (key, k))
    return v


def _layer_file(layer, fn, field):
    if not isinstance(fn, str) or os.path.basename(fn) != fn or fn.startswith("."):
        raise ProfileError("%s: %s: %r must be a file name in the layer" % (layer["key"], field, fn))
    p = os.path.join(layer["dir"], fn)
    if not os.path.isfile(p):
        raise ProfileError("%s: %s: %s is missing" % (layer["key"], field, fn))
    return p


def c_name(layer_key, fn):
    """The name a layer's .c file gets in the project's src/title/."""
    return "%s__%s" % (layer_key.replace("/", "_"), fn)


def resolve(tid, ident, root=PROFILES):
    """The merged profile for title ID `tid` (8 hex digits), or None when no
    layer applies (the generic template only). ProfileError for a layer that
    is not well formed."""
    title_key = "titles/%s" % tid.lower()
    title = load_layer(title_key, root)
    auto = auto_layers(ident, root)
    keys = list(auto)
    missing = []
    for k in (title["json"].get("layers") or []) if title else []:
        if not isinstance(k, str) or k.startswith("titles/"):
            raise ProfileError("%s: layers: %r is not an xdk or engine layer" % (title_key, k))
        if not load_layer(k, root):
            raise ProfileError("%s: layers: %s does not exist" % (title_key, k))
        if k not in keys:
            keys.append(k)
            missing.append(k)
    # xdk layers by name (they are independent of each other), engines in the
    # order they were found or asked for.
    order = {k: i for i, k in enumerate(keys)}
    keys.sort(key=lambda k: (KINDS.index(k.split("/")[0]), k if k.startswith("xdk/") else "", order[k]))
    layers = [load_layer(k, root) for k in keys] + ([title] if title else [])
    if not layers:
        return None

    prof = {"tid": tid.upper(), "layers": layers, "keys": [l["key"] for l in layers], "missing": missing,
            "env_force": {}, "env_default": {p: {} for p in PLATFORMS}, "params": {},
            "seed_functions": [], "exclude_manual": [], "hooks": [], "c_files": [],
            "title": title is not None}
    for layer in layers:
        key, obj = layer["key"], layer["json"]
        prof["env_force"].update(_check_env(key, obj, "env_force"))
        # Per platform, what each platform ends up with: a layer's own
        # platform value over its plain one, a higher layer over a lower one.
        common = _check_env(key, obj, "env_default")
        for p in PLATFORMS:
            prof["env_default"][p].update(common)
            prof["env_default"][p].update(_check_env(key, obj, "env_default_" + p))
        params = obj.get("params") or {}
        if not isinstance(params, dict):
            raise ProfileError("%s: params: an object expected" % key)
        for k, v in params.items():
            if not PARAM_KEY.match(k):
                raise ProfileError("%s: params: %r is not an XBA_[A-Z0-9_]+ name" % (key, k))
            prof["params"][k] = _param_value(key, k, v)
        for fn in obj.get("seed_functions") or []:
            prof["seed_functions"].append(_layer_file(layer, fn, "seed_functions"))
        for fn in obj.get("exclude_manual") or []:
            if not fn.endswith(".c"):
                raise ProfileError("%s: exclude_manual: %s is not a .c file" % (key, fn))
            _layer_file(layer, fn, "exclude_manual")
            prof["exclude_manual"].append(c_name(key, fn))
        hooks = obj.get("hooks")
        if hooks is not None:
            if not isinstance(hooks, str) or not C_IDENT.match(hooks) or hooks.startswith("xba_title"):
                raise ProfileError("%s: hooks: %r is not a C prefix" % (key, hooks))
            if hooks in prof["hooks"]:
                raise ProfileError("%s: hooks: %s is already another layer's" % (key, hooks))
            prof["hooks"].append(hooks)
        for fn in sorted(os.listdir(layer["dir"])):
            if fn.endswith(".c") and os.path.isfile(os.path.join(layer["dir"], fn)):
                prof["c_files"].append((os.path.join(layer["dir"], fn), c_name(key, fn)))
    if len(prof["exclude_manual"]) > 1:
        raise ProfileError("exclude_manual: %s: tools.recomp takes one file" % ", ".join(prof["exclude_manual"]))
    if title:
        for k in ("name", "project", "status"):
            if title["json"].get(k):
                prof[k] = title["json"][k]
    return prof


def digest_into(h, prof):
    """Feed the profile's files (every layer, in order) to the hash `h`."""
    for layer in (prof or {}).get("layers", []):
        h.update(layer["key"].encode() + b"\0")
        for fn in sorted(os.listdir(layer["dir"])):
            p = os.path.join(layer["dir"], fn)
            if os.path.isfile(p):
                h.update(fn.encode() + b"\0")
                with open(p, "rb") as f:
                    h.update(f.read())


# ── Generated C ─────────────────────────────────────────────────

def _cstr(v):
    return '"%s"' % v.replace("\\", "\\\\").replace('"', '\\"').replace("?", "\\?")


def params_header(prof):
    lines = ["/* Generated by tools.prepare: the profile layers' params (%s). */"
             % ", ".join(prof["keys"]), "#ifndef XBA_PARAMS_H", "#define XBA_PARAMS_H"]
    lines += ["#define %s 0x%08Xu" % (k, v) for k, v in sorted(prof["params"].items())]
    lines += ["#endif", ""]
    return "\n".join(lines)


def layers_source(prof):
    """src/title/xba_layers.c: the two hooks main.c calls (xba_title_configure,
    xba_title_start). Configure sets the merged env defaults, then calls each
    layer's <prefix>_configure, lowest layer first; start likewise."""
    ed = prof["env_default"]
    both = {k: v for k, v in ed["android"].items() if ed["desktop"].get(k) == v}
    out = ["/* Generated by tools.prepare: the profile layers of this title, lowest first:",
           " *   %s" % "\n *   ".join(prof["keys"]),
           " * docs/technical/profiles.md. */",
           "#include <stdlib.h>",
           "",
           "#ifndef _WIN32",
           "#define XBA_SETENV_DEFAULT(k, v) setenv((k), (v), 0)",
           "#else",
           "#define XBA_SETENV_DEFAULT(k, v) do { if (!getenv(k)) _putenv_s((k), (v)); } while (0)",
           "#endif",
           ""]
    for p in prof["hooks"]:
        out += ["void %s_configure(void);" % p,
                "void %s_start(const char *game_dir, const char *save_dir);" % p]
    out += ["", "void xba_title_configure(void)", "{",
            "    /* Defaults: the environment and recomp.cfg win over them. */"]
    out += ["    XBA_SETENV_DEFAULT(%s, %s);" % (_cstr(k), _cstr(v)) for k, v in both.items()]
    for plat, cond in (("android", "#ifdef __ANDROID__"), ("desktop", "#ifndef __ANDROID__")):
        own = {k: v for k, v in ed[plat].items() if k not in both}
        if own:
            out.append(cond)
            out += ["    XBA_SETENV_DEFAULT(%s, %s);" % (_cstr(k), _cstr(v)) for k, v in own.items()]
            out.append("#endif")
    out += ["    %s_configure();" % p for p in prof["hooks"]]
    out += ["}", "", "void xba_title_start(const char *game_dir, const char *save_dir)", "{",
            "    (void)game_dir; (void)save_dir;"]
    out += ["    %s_start(game_dir, save_dir);" % p for p in prof["hooks"]]
    out += ["}", ""]
    return "\n".join(out)
