"""tools.prepare.profiles: picking the layers from an XBE, merging them, the
generated C, and the profiles/ tree of the toolkit itself (no game data)."""

import json
import os
import re
import shutil
import subprocess
import sys

import pytest

sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..")))

from tools.prepare import profiles                    # noqa: E402
from tools.prepare import __main__ as prep            # noqa: E402

ROOT = profiles.ROOT


def ident(libs, engines=(), probes=None):
    return {"xdk": {"libraries": dict(libs)}, "engines": [{"layer": e, "kind": "engine"} for e in engines],
            "probes": dict(probes or {})}


def layer(root, key, obj, **files):
    d = root / key
    d.mkdir(parents=True, exist_ok=True)
    (d / ("title.json" if key.startswith("titles/") else "layer.json")).write_text(json.dumps(obj))
    for fn, text in files.items():
        (d / fn.replace("_c", ".c").replace("_json", ".json")).write_text(text)
    return d


@pytest.fixture
def tree(tmp_path):
    # xdk layers as a developer would make one for an A/B: none ships, and the XBE never picks one (an old
    # "applies" is not read); a title's "layers" names it.
    layer(tmp_path, "xdk/d3d8", {"applies": {"libraries": ["D3D8", "D3D8LTCG"]}, "hooks": "d3d",
                                 "env_default": {"RECOMP_PB_EXEC": "1", "RECOMP_VBLANK": "paced"}})
    layer(tmp_path, "xdk/dsound", {"env_default": {"RECOMP_AC97_READY": "1"}})
    layer(tmp_path, "engines/renderware", {"env_default": {"RECOMP_VBLANK": "plain"}})
    return tmp_path


def test_layers_from_the_engines_only(tree):
    root = str(tree)
    assert profiles.auto_layers(None, root) == []
    assert profiles.auto_layers(ident({"D3D8LTCG": 5849, "DSOUND": 5849}), root) == []
    # Engines: only those with a folder.
    assert profiles.auto_layers(ident({"d3d8": 1}, ["renderware", "unreal2"]), root) == ["engines/renderware"]


def test_no_layer_is_no_profile(tree):
    assert profiles.resolve("4D530001", ident({"XNET": 5849}), str(tree)) is None
    assert profiles.resolve("4D530001", ident({"D3D8": 5849}), str(tree)) is None


def test_merge_lowest_first(tree):
    layer(tree, "titles/4d530001", {"name": "T", "project": "t", "hooks": "t", "layers": ["xdk/d3d8"],
                                    "env_default": {"RECOMP_PB_EXEC": "0"},
                                    "env_default_android": {"RECOMP_GPU_FPS_CAP": "30"},
                                    "params": {"XBA_D3D_DEVICE_PTR": "0x00862288", "XBA_N": 5}},
          title_c="void t_configure(void) {}\n")
    p = profiles.resolve("4D530001", ident({"D3D8": 5849}, ["renderware"]), str(tree))
    assert p["keys"] == ["xdk/d3d8", "engines/renderware", "titles/4d530001"]
    assert p["title"] and p["name"] == "T" and p["project"] == "t" and p["missing"] == ["xdk/d3d8"]
    assert p["hooks"] == ["d3d", "t"]
    # The engine's VBLANK over the XDK's, the title's PB_EXEC over the XDK's.
    assert p["env_default"]["desktop"] == {"RECOMP_PB_EXEC": "0", "RECOMP_VBLANK": "plain"}
    assert p["env_default"]["android"] == dict(p["env_default"]["desktop"], RECOMP_GPU_FPS_CAP="30")
    assert p["params"] == {"XBA_D3D_DEVICE_PTR": 0x00862288, "XBA_N": 5}
    assert [d for _, d in p["c_files"]] == ["titles_4d530001__title.c"]


def test_a_platform_value_of_a_lower_layer_loses_to_a_plain_higher_one(tree):
    layer(tree, "xdk/d3d8", {"env_default_android": {"RECOMP_GPU_FPS_CAP": "30"}})
    layer(tree, "titles/4d530001", {"layers": ["xdk/d3d8"], "env_default": {"RECOMP_GPU_FPS_CAP": "60"}})
    p = profiles.resolve("4D530001", ident({"D3D8": 1}), str(tree))
    assert p["env_default"] == {"android": {"RECOMP_GPU_FPS_CAP": "60"}, "desktop": {"RECOMP_GPU_FPS_CAP": "60"}}


def test_title_asks_for_layers(tree):
    """A title lists the layers it was proven with: picked even when the XBE does not select them."""
    layer(tree, "titles/4d530001", {"layers": ["xdk/dsound", "xdk/d3d8"]})
    p = profiles.resolve("4D530001", ident({"D3D8": 5849}), str(tree))
    assert p["keys"] == ["xdk/d3d8", "xdk/dsound", "titles/4d530001"] and p["missing"] == ["xdk/dsound", "xdk/d3d8"]
    p = profiles.resolve("4D530001", None, str(tree))
    assert p["keys"] == ["xdk/d3d8", "xdk/dsound", "titles/4d530001"]
    for bad in (["xdk/nope"], ["titles/4541005b"], ["../x"]):
        layer(tree, "titles/4d530001", {"layers": bad})
        with pytest.raises(profiles.ProfileError):
            profiles.resolve("4D530001", None, str(tree))


@pytest.mark.parametrize("obj", [
    {"env_default": {"PATH": "/"}},
    {"env_default": {"RECOMP_X": "é"}},
    {"env_force": ["RECOMP_X"]},
    {"params": {"D3D": 1}},
    {"params": {"XBA_X": "zz"}},
    {"params": {"XBA_X": -1}},
    {"params": {"XBA_X": True}},
    {"hooks": "1abc"},
    {"hooks": "xba_title"},
    {"seed_functions": ["../x.json"]},
    {"seed_functions": ["missing.json"]},
    {"exclude_manual": ["notes.md"]},
    {"enhancements": "16:9"},
    {"enhancements": [""]},
    {"enhancements": ["é"]},
])
def test_refused(tree, obj):
    layer(tree, "titles/4d530001", obj, notes_md="")
    with pytest.raises(profiles.ProfileError):
        profiles.resolve("4D530001", None, str(tree))


def test_files_and_exclude_manual(tree):
    layer(tree, "titles/4d530001", {"seed_functions": ["seeds.json"], "exclude_manual": ["overrides.c"]},
          seeds_json="[]", overrides_c="/* */\n")
    p = profiles.resolve("4D530001", None, str(tree))
    assert p["seed_functions"] == [str(tree / "titles" / "4d530001" / "seeds.json")]
    assert p["exclude_manual"] == ["titles_4d530001__overrides.c"]
    layer(tree, "xdk/d3d8", {"exclude_manual": ["o.c"]}, o_c="/* */\n")
    layer(tree, "titles/4d530001", {"layers": ["xdk/d3d8"], "exclude_manual": ["overrides.c"]},
          overrides_c="/* */\n")
    with pytest.raises(profiles.ProfileError):                 # tools.recomp takes one file
        profiles.resolve("4D530001", ident({"D3D8": 1}), str(tree))
    layer(tree, "xdk/d3d8", {"hooks": "t"})
    layer(tree, "titles/4d530001", {"layers": ["xdk/d3d8"], "hooks": "t"})
    with pytest.raises(profiles.ProfileError):                 # two layers, one prefix
        profiles.resolve("4D530001", ident({"D3D8": 1}), str(tree))


def test_generated_c_runs_the_layers_in_order(tree, tmp_path):
    """xba_layers.c built by the host's cc with two layers' hooks: defaults set
    first (the environment wins), then each layer lowest first."""
    cc = shutil.which("cc")
    if not cc:
        pytest.skip("no cc")
    layer(tree, "xdk/d3d8", {"hooks": "d3d", "env_default": {"RECOMP_PB_EXEC": "1", "RECOMP_Q": 'a"b?\\'}},
          d3d_c='#include <stdio.h>\n#include <stdlib.h>\n#include "xba_params.h"\n'
                'void d3d_configure(void) { printf("d3d %s %s %s\\n", getenv("RECOMP_PB_EXEC"), '
                'getenv("RECOMP_Q"), getenv("RECOMP_USER")); }\n'
                'void d3d_start(const char *g, const char *s) { printf("d3d start %s 0x%X\\n", g, XBA_N); }\n')
    layer(tree, "titles/4d530001", {"hooks": "t", "layers": ["xdk/d3d8"], "env_default": {"RECOMP_USER": "profile"},
                                    "env_default_desktop": {"RECOMP_PB_EXEC": "2"},
                                    "params": {"XBA_N": "0x0085FDE0"}},
          title_c='#include <stdio.h>\nvoid t_configure(void) { puts("t"); }\n'
                  'void t_start(const char *g, const char *s) { printf("t start %s\\n", s); }\n')
    p = profiles.resolve("4D530001", ident({"D3D8": 1}), str(tree))
    proj = tmp_path / "proj"
    prep.assemble_project(str(proj), p, {"title_id": "0x4D530001"})
    t = proj / "src" / "title"
    (tmp_path / "main.c").write_text(
        "void xba_title_configure(void);\nvoid xba_title_start(const char *, const char *);\n"
        'int main(void) { xba_title_configure(); xba_title_start("g", "s"); return 0; }\n')
    exe = str(tmp_path / "t")
    srcs = [str(tmp_path / "main.c")] + sorted(str(t / f) for f in os.listdir(t) if f.endswith(".c"))
    subprocess.check_call([cc, "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-Wall", "-Werror", "-o", exe] + srcs)
    env = dict(os.environ, RECOMP_USER="me")
    env.pop("RECOMP_PB_EXEC", None)
    out = subprocess.check_output([exe], text=True, env=env)
    assert out == 'd3d 2 a"b?\\ me\nt\nd3d start g 0x85FDE0\nt start s\n'


# ── The toolkit's own profiles/ tree ─────────────────────────────

# Fable and Burnout 3 both link D3D8LTCG 5849 (their runs print "[D3D] D3D8LTCG build 5849").
FABLE_LIBS = {"XAPILIB": 5849, "D3D8LTCG": 5849, "DSOUND": 5849}
BO3_LIBS = {"XAPILIB": 5849, "D3D8LTCG": 5849, "DSOUND": 5849, "XMV": 5849}


def _shipped(kind):
    d = os.path.join(profiles.PROFILES, kind)
    return sorted(n for n in os.listdir(d) if os.path.isdir(os.path.join(d, n))) if os.path.isdir(d) else []


def test_no_xdk_or_engine_layer_ships():
    """What profiles/xdk/* and engines/* did is the toolkit's default (the XDK switches, the D3D service, the DMA
    walk, run-time pacing): no layer, and no title layer asks for one (tools/ci/layer_guard.py checks it too)."""
    for kind in ("xdk", "engines"):
        assert [n for n in _shipped(kind) if profiles.load_layer("%s/%s" % (kind, n))] == []
    for n in _shipped("titles"):
        assert not profiles.load_layer("titles/" + n)["json"].get("layers"), n


def test_no_title_layer_ships():
    """XBA-236: Fable's layer, the last one, became a player's mod (docs/technical/mods.md): no title has a layer,
    and the prepare of any title runs on the toolkit's defaults."""
    assert _shipped("titles") == []
    for tid in ("4D5300D1", "4541005B"):
        assert profiles.find_title_layer(tid) is None and profiles.title_line(tid) is None
    assert profiles.resolve("4D5300D1", ident(FABLE_LIBS)) is None


def test_a_renderware_title_with_no_profile_gets_no_layer():
    """Black (45410083) has no title layer, and since WP4 the RenderWare markers pick no layer; since WP3 no XDK
    layer either: the generic template only, the toolkit's defaults do the rest (the DMA walk included)."""
    black = profiles.resolve("45410083", ident({"XAPILIB": 5849, "D3D8LTCG": 5849, "DSOUND": 5849}, ["renderware"]))
    assert black is None
    assert effective(black, "android") == XDK_ENV


def test_the_dma_walk_and_the_d3d_service_are_toolkit_defaults():
    """The D3D service lives in src/ and makes the DMA walk the default walk; the title template calls it for every
    title, after the layers and the XDK defaults (so a layer's or the environment's RECOMP_PB_WALK wins). A title
    with no layer at all gets the walk (it used to need xdk/d3d8ltcg's XBA_D3D_DMA_WALK)."""
    with open(os.path.join(ROOT, "src", "kernel", "recomp_d3d_service.c")) as f:
        svc = f.read()
    assert 'SETENV_DEFAULT("RECOMP_PB_WALK", "dma");' in svc
    with open(os.path.join(ROOT, "src", "kernel", "CMakeLists.txt")) as f:
        assert "recomp_d3d_service.c" in f.read()
    with open(os.path.join(ROOT, "templates", "new-game", "src", "main.c")) as f:
        main = f.read()
    conf, defaults = main.index("    recomp_d3d_configure();"), main.index("    recomp_xdk_defaults();")
    assert main.index("    xba_title_configure();") < defaults < conf < main.index("xbox_MemoryLayoutInit(xbe_data")
    assert main.index("    recomp_d3d_start(s_game_dir, s_save_dir);") < main.index("    xbe_entry_point();")
    for libs in ({"D3D8LTCG": 5849}, {"D3D8LTCG": 5788}, {"D3D8": 5455}, {}):
        assert profiles.resolve("4D530001", ident(libs)) is None
        assert effective(None, "android")["RECOMP_PB_WALK"] == "dma"


def test_every_layer_in_the_tree_is_well_formed():
    """Every layer's JSON passes the checks, alone on top of a bare title."""
    for n in _shipped("engines"):
        assert profiles.load_layer("engines/" + n)
    for n in _shipped("titles"):
        assert profiles.resolve(n, None)["title"]


def test_an_empty_profiles_tree_gives_no_profile(tmp_path):
    """A title prepares with no profiles/ at all: no layer, the generic template only."""
    for libs, engines in (({"D3D8LTCG": 5849}, ["renderware"]), (FABLE_LIBS, ())):
        assert profiles.resolve("4541005B", ident(libs, engines), str(tmp_path)) is None


# ── Effective settings: what a run starts with ─────────────────

def _xdk_defaults():
    """recomp_xdk_defaults' table, read from its C source (src/kernel/recomp_xdk_defaults.c)."""
    with open(os.path.join(ROOT, "src", "kernel", "recomp_xdk_defaults.c")) as f:
        src = f.read()
    table = src[src.index("k_defaults[][2] = {"):src.index("};", src.index("k_defaults[][2] = {"))]
    return dict(re.findall(r'\{"(RECOMP_[A-Z0-9_]+)", "([^"]*)"\}', table))


def effective(prof, plat):
    """The switches a run of this profile starts with, as main() sets them: the layers' env_default for the
    platform, then recomp_xdk_defaults, the pushbuffer executor (main(), the Android shell) and the D3D service's
    DMA walk where nothing set them; env_force over everything."""
    env = dict((prof or {}).get("env_default", {}).get(plat, {}))
    for k, v in dict(_xdk_defaults(), RECOMP_PB_EXEC="1", RECOMP_PB_WALK="dma").items():
        env.setdefault(k, v)
    env.update((prof or {}).get("env_force", {}))
    return env


XDK_ENV = {"RECOMP_PB_EXEC": "1", "RECOMP_VBLANK": "paced", "RECOMP_AC97_READY": "1",
           "RECOMP_DISPATCH_GATE": "1", "RECOMP_USB": "1", "RECOMP_KEYBOARD": "1", "RECOMP_PB_WALK": "dma"}

# The four reference titles: libraries, engines, keys, hooks, and the effective settings per platform. The
# settings are what they started with before the XDK layers went (main @ 261ad6c, with profiles/xdk/*: the
# layers' switches, XBA_D3D_DMA_WALK from xdk/d3d8ltcg on D3D8LTCG 5849; WP4 had already moved the RenderWare
# layer, Burnout 3's title.c and Fable's Android cap to run time), with one change by design: KOTOR (D3D8 5455)
# moves from the segment scan to the DMA walk (K-7: 5 of 5 runs each way without a difference).
REFERENCE = {
    "4D5300D1": ((("XAPILIB", 5849), ("D3D8LTCG", 5849), ("DSOUND", 5849)), (), None, None, XDK_ENV, XDK_ENV),
    "4541005B": ((("XAPILIB", 5849), ("D3D8LTCG", 5849), ("DSOUND", 5849), ("XMV", 5849)), ("renderware",),
                 None, None, XDK_ENV, XDK_ENV),
    "45410083": ((("XAPILIB", 5849), ("D3D8LTCG", 5849), ("DSOUND", 5849)), ("renderware",), None, None,
                 XDK_ENV, XDK_ENV),
    "4C410003": ((("XAPILIB", 5455), ("D3D8", 5455), ("DSOUND", 5455)), (), None, None, XDK_ENV, XDK_ENV),
}

XAPI_IMPORTS = (326, 327, 328, 67, 24)
DSOUND_IMPORTS = (175, 173, 44, 98)
D3D_IMPORTS = (3, 2, 1, 4, 168)
ALL_XDK_IMPORTS = tuple(sorted(set(XAPI_IMPORTS + DSOUND_IMPORTS + D3D_IMPORTS + (49, 164, 187, 190))))


def _synthetic(tmp_path, name, libs, imports, title_id=0x4D530001, engines=()):
    """tools.xbe_ident's answer for a synthetic XBE of our own (library table, kernel thunk table)."""
    from tools.xbe_ident import identify
    from tools.xbe_ident.test_xbe_ident import make_xbe
    r = identify(make_xbe(tmp_path / name, libs=tuple((n, b, 0) for n, b in libs), xapi_index=None,
                          imports=imports, title_id=title_id))
    r["engines"] = [{"layer": e, "kind": "engine"} for e in engines]   # the markers: test_xbe_ident
    return r


@pytest.mark.parametrize("tid", sorted(REFERENCE))
def test_the_reference_titles_start_with_the_same_settings(tmp_path, tid):
    """Whatever the XBE's library table or imports say (a stripped or renamed table included, XBA-106)."""
    libs, engines, keys, hooks, android, desktop = REFERENCE[tid]
    for table, imports in ((libs, ()), (libs, ALL_XDK_IMPORTS), ((), D3D_IMPORTS), ((("D3D8_QFE", 5849),), ())):
        p = profiles.resolve(tid, _synthetic(tmp_path, "%s-%d-%d.xbe" % (tid, len(table), len(imports)), table,
                                             imports, int(tid, 16), engines))
        assert (p and p["keys"]) == keys and (p and p["hooks"]) == hooks
        assert not (p and p["params"])
        assert effective(p, "android") == android and effective(p, "desktop") == desktop


@pytest.mark.parametrize("tid", sorted(REFERENCE))
def test_without_the_title_layer_only_the_title_specific_values_go(tmp_path, tid):
    """XBA_NO_TITLE_LAYER=1: no layer at all; the toolkit's defaults give the same settings."""
    libs, engines, _, _, _, _ = REFERENCE[tid]
    r = _synthetic(tmp_path, "%s.xbe" % tid, libs, (), int(tid, 16), engines)
    p = profiles.resolve(tid, r, title_layer=False)
    assert p is None
    assert effective(p, "desktop") == XDK_ENV and effective(p, "android") == XDK_ENV


def test_the_xdk_report_says_what_applies(tmp_path):
    r = _synthetic(tmp_path, "homebrew.xbe", (), (49, 187, 190))
    assert profiles.auto_layers(r) == [] and profiles.resolve("4D530001", r) is None
    assert profiles.xdk_report(r) == ["XDK: no library table; the toolkit's defaults apply "
                                      "(XDK switches, D3D service, DMA walk)"]
    r = _synthetic(tmp_path, "t.xbe", (("XAPILIB", 5849), ("D3D8LTCG", 5849)), ())
    assert profiles.xdk_report(r) == ["XDK: D3D8LTCG 5849, XAPILIB 5849; the toolkit's defaults apply "
                                      "(XDK switches, D3D service, DMA walk)"]
    assert "could not be identified" in profiles.xdk_report(None)[0]


def test_without_the_title_layer_a_title_is_any_title(tree):
    layer(tree, "titles/4d530001", {"name": "T", "layers": ["xdk/dsound"], "env_default": {"RECOMP_X": "1"}})
    p = profiles.resolve("4D530001", ident({"D3D8": 5849}, ["renderware"]), str(tree), title_layer=False)
    assert p["keys"] == ["engines/renderware"] and not p["title"] and "RECOMP_X" not in p["env_default"]["desktop"]
    assert profiles.resolve("4D530001", ident({"XNET": 5849}), str(tree), title_layer=False) is None


def test_the_enhancements_switch_state(monkeypatch, tmp_path):
    """What pack.json and --check say for a launcher's « Enhancements » switch: "on" / "off" for a title with mods
    turned on in the store (docs/technical/mods.md), nothing for any other."""
    import json as _json
    from tools.mods import store
    d = tmp_path / "xbox" / "58420001" / "wide"
    d.mkdir(parents=True)
    (d / "mod.json").write_text(_json.dumps({"format": 1, "id": "wide", "name": "W", "version": "1",
                                             "console": "xbox", "title_id": "58420001"}))
    monkeypatch.setenv("XBA_MODS_DIR", str(tmp_path))
    monkeypatch.delenv("XBA_NO_TITLE_LAYER", raising=False)
    monkeypatch.delenv("XBA_NO_MODS", raising=False)
    assert prep.title_layer_state("58420001") is None                  # installed, not turned on
    store.enable(str(tmp_path), "xbox", "58420001", "wide")
    assert prep.title_layer_state("58420001") == "on" and prep.title_layer_state("45410083") is None
    monkeypatch.setenv("XBA_NO_TITLE_LAYER", "1")
    assert prep.title_layer_state("58420001") == "off" and prep.title_layer_state("45410083") is None
    monkeypatch.setenv("XBA_NO_TITLE_LAYER", "0")
    assert prep.title_layer_state("58420001") == "on"
    monkeypatch.setenv("XBA_NO_MODS", "1")
    assert prep.title_layer_state("58420001") == "off"
