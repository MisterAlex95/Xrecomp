"""layer_guard.py (profile-free WP6): the shipped profiles/ tree holds optional enhancements only, and each rule fails
on a planted layer in a synthetic tree. Runs with the rest of `pytest tools/` in the linux job: this is the CI check."""
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
import layer_guard as lg  # noqa: E402

GOOD = {"name": "T", "hooks": "t", "enhancements": ["16:9 scenes"], "exclude_manual": ["overrides.c"],
        "env_default": {"RECOMP_ASPECT": "2"}, "params": {"XBA_T_RATE": 16}, "status": "runs without it"}


def test_the_shipped_tree_is_clean():
    found = lg.scan(ROOT)
    assert found == [], "\n".join("profiles/%s: %s: %s" % f for f in found)


def tree(tmp_path, layers, allow=""):
    for key, obj in layers.items():
        d = tmp_path / "profiles" / key
        d.mkdir(parents=True, exist_ok=True)
        (d / ("title.json" if key.startswith("titles/") else "layer.json")).write_text(json.dumps(obj))
    (tmp_path / "tools" / "ci").mkdir(parents=True, exist_ok=True)
    (tmp_path / "tools" / "ci" / "layer_guard_allow.txt").write_text(allow)
    return [(layer, rule) for layer, rule, _ in lg.scan(str(tmp_path))]


def test_an_enhancement_layer_passes(tmp_path):
    assert tree(tmp_path, {"titles/58424104": GOOD}) == []
    assert tree(tmp_path / "empty", {}) == []                 # no profiles/ at all: the normal case


def test_a_d3d_address_fails(tmp_path):
    bad = dict(GOOD, params={"XBA_D3D_DEVICE_PTR": "0x00123450", "XBA_T_RATE": 16})
    assert tree(tmp_path, {"titles/58424104": bad}) == [("titles/58424104", "params:XBA_D3D_DEVICE_PTR")]


def test_what_a_general_mechanism_covers_fails(tmp_path):
    bad = dict(GOOD, layers=["engines/x"], seed_functions=["seeds.json"],
               env_default_android={"RECOMP_GPU_FPS_CAP": "30"}, env_force={"RECOMP_LSFG": "0"},
               env_default={"RECOMP_PB_WALK": "scan", "RECOMP_VBLANK": "plain", "RECOMP_ASPECT": "2"})
    assert sorted(tree(tmp_path, {"titles/58424104": bad})) == sorted([
        ("titles/58424104", "layers"), ("titles/58424104", "seed_functions"),
        ("titles/58424104", "env_default_android:RECOMP_GPU_FPS_CAP"), ("titles/58424104", "env_force:RECOMP_LSFG"),
        ("titles/58424104", "env_default:RECOMP_PB_WALK"), ("titles/58424104", "env_default:RECOMP_VBLANK")])


def test_a_layer_with_nothing_to_add_fails(tmp_path):
    assert tree(tmp_path, {"titles/58424104": {"name": "T", "project": "t", "status": "runs"}}) == \
        [("titles/58424104", "enhancements")]
    assert tree(tmp_path / "b", {"titles/58424104": dict(GOOD, enhancements=[])}) == \
        [("titles/58424104", "enhancements")]


def test_shipped_xdk_and_engine_layers_and_unknown_keys_fail(tmp_path):
    found = tree(tmp_path, {"xdk/d3d8": {"hooks": "d3d"}, "engines/x": {},
                            "titles/58424104": dict(GOOD, envdefault={"RECOMP_X": "1"})})
    assert sorted(found) == [("engines/x", "layer.json"), ("titles/58424104", "key:envdefault"),
                             ("xdk/d3d8", "layer.json")]
    (tmp_path / "profiles" / "engines" / "notes").mkdir()      # a folder without layer.json is no layer
    assert ("engines/notes", "layer.json") not in [(a, b) for a, b, _ in lg.scan(str(tmp_path))]


def test_the_allow_list(tmp_path):
    bad = dict(GOOD, env_force={"RECOMP_LSFG": "0"}, params={"XBA_D3D_DEVICE": 1})
    found = tree(tmp_path, {"titles/58424104": bad},
                 allow="# header\ntitles/58424104 env_force:RECOMP_LSFG   # a reason\n")
    assert found == [("titles/58424104", "params:XBA_D3D_DEVICE")]


def test_the_command_line_exit_code(tmp_path):
    tree(tmp_path, {"titles/58424104": dict(GOOD, params={"XBA_D3D_DEVICE_PTR": 1})})
    r = subprocess.run([sys.executable, os.path.join(HERE, "layer_guard.py"), "--root", str(tmp_path)],
                       capture_output=True, text=True)
    assert r.returncode == 1 and "profiles/titles/58424104: params:XBA_D3D_DEVICE_PTR" in r.stdout
    r = subprocess.run([sys.executable, os.path.join(HERE, "layer_guard.py"), "--root", ROOT],
                       capture_output=True, text=True)
    assert r.returncode == 0, r.stdout + r.stderr
