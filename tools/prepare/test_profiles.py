"""tools.prepare.profiles: picking the layers from an XBE, merging them, the
generated C, and the profiles/ tree of the toolkit itself (no game data)."""

import json
import os
import shutil
import subprocess
import sys

import pytest

sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..")))

from tools.prepare import profiles                    # noqa: E402
from tools.prepare import __main__ as prep            # noqa: E402

ROOT = profiles.ROOT


def ident(libs, engines=()):
    return {"xdk": {"libraries": dict(libs)}, "engines": [{"layer": e, "kind": "engine"} for e in engines]}


def layer(root, key, obj, **files):
    d = root / key
    d.mkdir(parents=True, exist_ok=True)
    (d / ("title.json" if key.startswith("titles/") else "layer.json")).write_text(json.dumps(obj))
    for fn, text in files.items():
        (d / fn.replace("_c", ".c").replace("_json", ".json")).write_text(text)
    return d


@pytest.fixture
def tree(tmp_path):
    layer(tmp_path, "xdk/d3d8", {"applies": {"libraries": ["D3D8", "D3D8LTCG"]}, "hooks": "d3d",
                                 "env_default": {"RECOMP_PB_EXEC": "1", "RECOMP_VBLANK": "paced"}})
    layer(tmp_path, "xdk/dsound", {"applies": {"libraries": ["DSOUND"], "builds": [5558, 5849]},
                                   "env_default": {"RECOMP_AC97_READY": "1"}})
    layer(tmp_path, "engines/renderware", {"env_default": {"RECOMP_VBLANK": "plain"}})
    return tmp_path


def test_layers_from_the_library_table(tree):
    root = str(tree)
    assert profiles.auto_layers(None, root) == []
    assert profiles.auto_layers(ident({"D3D8LTCG": 5849, "DSOUND": 5849}), root) == ["xdk/d3d8", "xdk/dsound"]
    # A build outside the layer's range, a library nobody has a layer for.
    assert profiles.auto_layers(ident({"DSOUND": 4627, "XNET": 5849}), root) == []
    # Engines: only those with a folder; lowercase library names match too.
    assert profiles.auto_layers(ident({"d3d8": 1}, ["renderware", "unreal2"]), root) == \
        ["xdk/d3d8", "engines/renderware"]


def test_no_layer_is_no_profile(tree):
    assert profiles.resolve("4D530001", ident({"XNET": 5849}), str(tree)) is None


def test_merge_lowest_first(tree):
    layer(tree, "titles/4d530001", {"name": "T", "project": "t", "hooks": "t",
                                    "env_default": {"RECOMP_PB_EXEC": "0"},
                                    "env_default_android": {"RECOMP_GPU_FPS_CAP": "30"},
                                    "params": {"XBA_D3D_DEVICE_PTR": "0x00862288", "XBA_N": 5}},
          title_c="void t_configure(void) {}\n")
    p = profiles.resolve("4D530001", ident({"D3D8": 5849}, ["renderware"]), str(tree))
    assert p["keys"] == ["xdk/d3d8", "engines/renderware", "titles/4d530001"]
    assert p["title"] and p["name"] == "T" and p["project"] == "t" and p["missing"] == []
    assert p["hooks"] == ["d3d", "t"]
    # The engine's VBLANK over the XDK's, the title's PB_EXEC over the XDK's.
    assert p["env_default"]["desktop"] == {"RECOMP_PB_EXEC": "0", "RECOMP_VBLANK": "plain"}
    assert p["env_default"]["android"] == dict(p["env_default"]["desktop"], RECOMP_GPU_FPS_CAP="30")
    assert p["params"] == {"XBA_D3D_DEVICE_PTR": 0x00862288, "XBA_N": 5}
    assert [d for _, d in p["c_files"]] == ["titles_4d530001__title.c"]


def test_a_platform_value_of_a_lower_layer_loses_to_a_plain_higher_one(tree):
    layer(tree, "xdk/d3d8", {"applies": {"libraries": ["D3D8"]}, "env_default_android": {"RECOMP_GPU_FPS_CAP": "30"}})
    layer(tree, "titles/4d530001", {"env_default": {"RECOMP_GPU_FPS_CAP": "60"}})
    p = profiles.resolve("4D530001", ident({"D3D8": 1}), str(tree))
    assert p["env_default"] == {"android": {"RECOMP_GPU_FPS_CAP": "60"}, "desktop": {"RECOMP_GPU_FPS_CAP": "60"}}


def test_title_asks_for_layers(tree):
    """A title lists the layers it was proven with: picked even when the XBE does not select them."""
    layer(tree, "titles/4d530001", {"layers": ["xdk/dsound", "xdk/d3d8"]})
    p = profiles.resolve("4D530001", ident({"D3D8": 5849}), str(tree))
    assert p["keys"] == ["xdk/d3d8", "xdk/dsound", "titles/4d530001"] and p["missing"] == ["xdk/dsound"]
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
    layer(tree, "xdk/d3d8", {"applies": {"libraries": ["D3D8"]}, "exclude_manual": ["o.c"]}, o_c="/* */\n")
    with pytest.raises(profiles.ProfileError):                 # tools.recomp takes one file
        profiles.resolve("4D530001", ident({"D3D8": 1}), str(tree))
    layer(tree, "xdk/d3d8", {"applies": {"libraries": ["D3D8"]}, "hooks": "t"})
    layer(tree, "titles/4d530001", {"hooks": "t"})
    with pytest.raises(profiles.ProfileError):                 # two layers, one prefix
        profiles.resolve("4D530001", ident({"D3D8": 1}), str(tree))


def test_generated_c_runs_the_layers_in_order(tree, tmp_path):
    """xba_layers.c built by the host's cc with two layers' hooks: defaults set
    first (the environment wins), then each layer lowest first."""
    cc = shutil.which("cc")
    if not cc:
        pytest.skip("no cc")
    layer(tree, "xdk/d3d8", {"applies": {"libraries": ["D3D8"]}, "hooks": "d3d",
                             "env_default": {"RECOMP_PB_EXEC": "1", "RECOMP_Q": 'a"b?\\'}},
          d3d_c='#include <stdio.h>\n#include <stdlib.h>\n#include "xba_params.h"\n'
                'void d3d_configure(void) { printf("d3d %s %s %s\\n", getenv("RECOMP_PB_EXEC"), '
                'getenv("RECOMP_Q"), getenv("RECOMP_USER")); }\n'
                'void d3d_start(const char *g, const char *s) { printf("d3d start %s 0x%X\\n", g, XBA_D3D_DEVICE); }\n')
    layer(tree, "titles/4d530001", {"hooks": "t", "env_default": {"RECOMP_USER": "profile"},
                                    "env_default_desktop": {"RECOMP_PB_EXEC": "2"},
                                    "params": {"XBA_D3D_DEVICE": "0x0085FDE0"}},
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

FABLE_LIBS = {"XAPILIB": 5849, "D3D8": 5849, "DSOUND": 5849}
BO3_LIBS = {"XAPILIB": 5849, "D3D8LTCG": 5849, "DSOUND": 5849, "XMV": 5849}


@pytest.mark.parametrize("tid,libs", [("4D5300D1", FABLE_LIBS), ("4541005B", BO3_LIBS)])
def test_the_first_titles_resolve(tid, libs):
    for ident_ in (ident(libs), None):          # also when the XBE cannot be identified
        p = profiles.resolve(tid, ident_)
        xdk = [k for k in p["keys"] if k.startswith("xdk/")]
        assert p["keys"][-1] == "titles/" + tid.lower()
        if ident_ and "D3D8LTCG" in libs:       # picked by the library table only, not asked by the title
            assert xdk == ["xdk/d3d8", "xdk/d3d8ltcg", "xdk/dsound", "xdk/xapi"]
        else:
            assert xdk == ["xdk/d3d8", "xdk/dsound", "xdk/xapi"]
        asked = [k for k in p["layers"][-1]["json"]["layers"]]
        assert p["missing"] == ([k for k in asked if k.startswith("engines/")] if ident_ else asked)
        # What both had in their own title.c before the layers: now from the XDK layers.
        common = {"RECOMP_PB_EXEC": "1", "RECOMP_AC97_READY": "1", "RECOMP_VBLANK": "paced",
                  "RECOMP_DISPATCH_GATE": "1", "RECOMP_USB": "1", "RECOMP_KEYBOARD": "1"}
        for plat in ("android", "desktop"):
            assert common.items() <= p["env_default"][plat].items()
        assert "XBA_D3D_DEVICE_PTR" in p["params"] and p["params"]["XBA_D3D_SW_METHODS"]


def test_fable_and_burnout_keep_their_own_values():
    fable = profiles.resolve("4D5300D1", ident(FABLE_LIBS))
    assert fable["env_default"]["android"]["RECOMP_GPU_FPS_CAP"] == "30"
    assert "RECOMP_GPU_FPS_CAP" not in fable["env_default"]["desktop"]
    assert fable["params"] == {"XBA_D3D_DEVICE_PTR": 0x00862288, "XBA_D3D_DEVICE": 0x0085FDE0,
                               "XBA_D3D_SW_METHODS": 0x7000}
    assert fable["exclude_manual"] == ["titles_4d5300d1__overrides.c"]
    bo3 = profiles.resolve("4541005B", ident(BO3_LIBS))
    # The engine layer (XBA-95) carries what Burnout 3 needs as a RenderWare title.
    assert "engines/renderware" in bo3["keys"]
    assert bo3["env_force"] == {"RECOMP_LSFG": "0"}
    assert bo3["params"] == {"XBA_D3D_DEVICE_PTR": 0x0035FE48, "XBA_D3D_SW_METHODS": 0x20,
                             "XBA_D3D_DMA_WALK": 1}   # from xdk/d3d8ltcg
    for plat in ("android", "desktop"):
        assert bo3["env_default"][plat]["RECOMP_GPU_FPS_CAP"] == "60"
        assert bo3["env_default"][plat]["RECOMP_PB_WALK"] == "dma"
        assert bo3["env_default"][plat]["RECOMP_TITLE_KEVENTS"] == "1"


def test_a_renderware_title_with_no_profile_gets_the_engine_layer():
    """Black (45410083) has no title layer: the RenderWare markers pick the engine layer."""
    black = profiles.resolve("45410083", ident({"XAPILIB": 5849, "D3D8LTCG": 5849, "DSOUND": 5849}, ["renderware"]))
    assert black["keys"] == ["xdk/d3d8", "xdk/d3d8ltcg", "xdk/dsound", "xdk/xapi", "engines/renderware"]
    assert not black["title"]
    assert black["env_force"] == {"RECOMP_LSFG": "0"}
    assert black["env_default"]["android"]["RECOMP_PB_WALK"] == "dma"


def test_d3d8ltcg_5849_gets_the_dma_walk_without_a_title_profile():
    """XDK 5849's D3D8LTCG (Black, Burnout 3): its fence wait needs the DMA walk, so the D3D8LTCG layer turns it on
    for any such title (XBA_D3D_DMA_WALK), and the D3D8 layer then adopts the fence event once it finds the device.
    D3D8 (Fable) and other D3D8LTCG builds are left as they were."""
    black = profiles.resolve("45410083", ident({"XAPILIB": 5849, "D3D8LTCG": 5849, "DSOUND": 5849}))
    assert black["keys"] == ["xdk/d3d8", "xdk/d3d8ltcg", "xdk/dsound", "xdk/xapi"] and not black["title"]
    assert black["params"] == {"XBA_D3D_DMA_WALK": 1}
    assert "#define XBA_D3D_DMA_WALK 0x00000001u" in profiles.params_header(black)
    for libs in ({"D3D8": 5849}, {"D3D8LTCG": 5788}):
        p = profiles.resolve("4D530001", ident(libs))
        assert "xdk/d3d8ltcg" not in p["keys"] and "XBA_D3D_DMA_WALK" not in p["params"]
    assert "XBA_D3D_DMA_WALK" not in profiles.resolve("4D5300D1", ident(FABLE_LIBS))["params"]


def test_every_layer_in_the_tree_is_well_formed():
    """Every layer's JSON passes the checks, alone on top of a bare title."""
    keys = ["%s/%s" % (k, n) for k in ("xdk", "engines") for n in profiles._names(profiles.PROFILES, k)]
    assert "xdk/d3d8" in keys
    for k in keys:
        l = profiles.load_layer(k)
        if k.startswith("xdk/"):
            assert l["json"].get("applies", {}).get("libraries"), k
    for n in profiles._names(profiles.PROFILES, "titles"):
        assert profiles.resolve(n, None)["title"]


def test_the_layer_c_compiles(tmp_path):
    """Each title's assembled src/title/ (its layers' C and the generated files)
    passes the host compiler's syntax check, as a desktop POSIX build and as
    Android (the __ANDROID__ branches)."""
    cc = shutil.which("cc")
    if not cc:
        pytest.skip("no cc")
    inc = tmp_path / "inc" / "recomp" / "gen"
    inc.mkdir(parents=True)
    shutil.copy(os.path.join(ROOT, "templates", "runtime", "recomp_types.h"), inc / "recomp_types.h")
    for tid, libs in (("4D5300D1", FABLE_LIBS), ("4541005B", BO3_LIBS)):
        proj = tmp_path / tid
        prep.assemble_project(str(proj), profiles.resolve(tid, ident(libs)), {"title_id": "0x" + tid})
        t = proj / "src" / "title"
        for fn in sorted(os.listdir(t)):
            if not fn.endswith(".c"):
                continue
            for extra in ([], ["-D__ANDROID__"]):
                subprocess.check_call([cc, "-std=gnu11", "-fsyntax-only", "-Werror=implicit-function-declaration",
                                       "-I", str(tmp_path / "inc"), "-I", os.path.join(ROOT, "include")]
                                      + extra + [str(t / fn)])
