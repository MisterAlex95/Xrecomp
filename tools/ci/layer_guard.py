#!/usr/bin/env python3
"""Profiles are optional enhancements (profile-free WP6): what a shipped layer under profiles/ may hold.

Runs in the linux CI job through tools/ci/test_layer_guard.py (the `pytest tools/` pass), or by hand:

    python3 tools/ci/layer_guard.py [--root DIR]

A title runs on the toolkit alone: its defaults (the XDK switches, the D3D service, the DMA walk, run-time pacing and
the frame-lock verdict) and what the analysis and the runs find. A title layer only adds what cannot be derived, an
enhancement of one game (a 16:9 mod, a front-end rate), and the title still runs with it left out
(--no-title-layer / XBA_NO_TITLE_LAYER=1). So a shipped tree fails when:

  - profiles/xdk/<name>/ or profiles/engines/<name>/ holds a layer.json: what those layers did is a toolkit default
    now; a developer's A/B layer is not shipped;
  - a title.json has a key outside KNOWN_KEYS (a typo, or a key this guard does not know yet);
  - a title.json says no "enhancements": a layer with nothing to add (a name and a state) is deleted, the XBE's
    certificate and the prepare give those;
  - a title.json carries what a general mechanism covers:
      "layers"            picking an xdk or engine layer: they are defaults or found from the XBE
      "seed_functions"    the analysis and the discovery loop find code (static discovery, gated seeds)
      "params" XBA_D3D_*  the D3D service finds the device at run time (RECOMP_D3D_* for an A/B)
      env_default*, env_force of a DENY_ENV switch: the toolkit's defaults and run-time decisions

A justified exception goes into tools/ci/layer_guard_allow.txt, one per line with its reason:

    <layer> <rule>            e.g.  titles/58424104 env_default:RECOMP_PB_WALK   # why

where <rule> is what a finding prints after the layer (`layers`, `seed_functions`, `params:XBA_D3D_X`,
`env_force:RECOMP_X`, `layer.json`, `key:<name>`, `enhancements`).
"""
import argparse
import json
import os
import sys

KNOWN_KEYS = {"title_id", "name", "project", "status", "hooks", "enhancements", "exclude_manual", "params",
              "env_default", "env_default_android", "env_default_desktop", "env_force", "seed_functions", "layers",
              "_why"}
ENV_FIELDS = ("env_default", "env_default_android", "env_default_desktop", "env_force")

# Switches a general mechanism sets or decides: a title layer that sets them takes the decision away from it.
DENY_ENV = {
    # recomp_xdk_defaults (src/kernel/recomp_xdk_defaults.c, XBA-106) and the title template's main
    "RECOMP_DISPATCH_GATE", "RECOMP_VBLANK", "RECOMP_USB", "RECOMP_KEYBOARD", "RECOMP_AC97_READY", "RECOMP_PB_EXEC",
    "RECOMP_XDK_DEFAULTS",
    # the D3D service and its DMA walk (src/kernel/recomp_d3d_service.c, WP3)
    "RECOMP_PB_WALK", "RECOMP_D3D_SERVICE", "RECOMP_D3D_DEVICE_PTR", "RECOMP_D3D_DEVICE", "RECOMP_D3D_SW_METHODS",
    # pacing and frame generation decided at run time (src/kernel/frame_pacing.h, frame_lock.h, WP4)
    "RECOMP_GPU_FPS_CAP", "RECOMP_PACING", "RECOMP_FLIP_PACING", "RECOMP_FRAME_LOCK", "RECOMP_LSFG",
}

ALLOW = os.path.join("tools", "ci", "layer_guard_allow.txt")


def load_allow(path):
    out = set()
    try:
        with open(path, encoding="utf-8") as f:
            for line in f:
                line = line.split("#", 1)[0].split()
                if len(line) == 2:
                    out.add((line[0], line[1]))
    except OSError:
        pass
    return out


def _subdirs(d):
    return sorted(n for n in os.listdir(d) if os.path.isdir(os.path.join(d, n))) if os.path.isdir(d) else []


def title_findings(key, obj):
    """[(rule, why)] for one title.json's object."""
    if not isinstance(obj, dict):
        return [("title.json", "not a JSON object")]
    out = []
    for k in sorted(obj):
        if k not in KNOWN_KEYS:
            out.append(("key:%s" % k, "a key this guard does not know (tools/ci/layer_guard.py KNOWN_KEYS)"))
    en = obj.get("enhancements")
    if not (isinstance(en, list) and en):
        out.append(("enhancements", "says no enhancement: a title layer only adds enhancements; one with nothing to "
                                    "add is deleted (the certificate and the prepare give a name and a state)"))
    if obj.get("layers"):
        out.append(("layers", "picks xdk or engine layers: those are toolkit defaults or found from the XBE"))
    if obj.get("seed_functions"):
        out.append(("seed_functions", "seeds code: static discovery and the gated discovery loop find it"))
    params = obj.get("params")
    for k in sorted(params) if isinstance(params, dict) else ():
        if k.startswith("XBA_D3D_"):
            out.append(("params:%s" % k, "a D3D address: the D3D service finds the device at run time "
                                         "(RECOMP_D3D_* overrides it for an A/B)"))
    for field in ENV_FIELDS:
        env = obj.get(field)
        for k in sorted(env) if isinstance(env, dict) else ():
            if k in DENY_ENV:
                out.append(("%s:%s" % (field, k), "a switch a general mechanism sets or decides at run time"))
    return out


def scan(root, allow_path=None):
    """[(layer, rule, why)] for the profiles/ tree under root, the allow-list applied."""
    allow = load_allow(allow_path or os.path.join(root, ALLOW))
    profiles = os.path.join(root, "profiles")
    found = []
    for kind in ("xdk", "engines"):
        for n in _subdirs(os.path.join(profiles, kind)):
            if os.path.isfile(os.path.join(profiles, kind, n, "layer.json")):
                found.append(("%s/%s" % (kind, n), "layer.json",
                              "a shipped %s layer: what it does belongs in the toolkit's defaults" % kind))
    for n in _subdirs(os.path.join(profiles, "titles")):
        key = "titles/%s" % n
        path = os.path.join(profiles, "titles", n, "title.json")
        if not os.path.isfile(path):
            if not os.listdir(os.path.join(profiles, "titles", n)):
                continue            # an empty folder git does not even keep
            found.append((key, "title.json", "a title folder without title.json"))
            continue
        try:
            with open(path, encoding="utf-8") as f:
                obj = json.load(f)
        except ValueError as e:
            found.append((key, "title.json", "not JSON: %s" % e))
            continue
        found += [(key, rule, why) for rule, why in title_findings(key, obj)]
    return [f for f in found if (f[0], f[1]) not in allow]


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--root", default=os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
    args = ap.parse_args(argv)
    found = scan(args.root)
    for layer, rule, why in found:
        print("profiles/%s: %s: %s" % (layer, rule, why))
    if found:
        print("%d finding(s): a title layer holds optional enhancements only (docs/technical/profiles.md); a real "
              "exception goes into %s with its reason" % (len(found), ALLOW), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
