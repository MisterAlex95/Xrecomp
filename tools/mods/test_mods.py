"""tools.mods: mod.json validation, the per-title store, load order and the mod-set hash (MOD-1, XBA-286)."""
import json
import os

import pytest

from tools.mods import __main__ as cli
from tools.mods import display, package, store
from tools.mods.package import ModError

TID = "58420001"
SHA = "ab" * 32


def make_mod(parent, mid="widescreen", version="1.0.0", **extra):
    d = parent / mid
    d.mkdir(parents=True, exist_ok=True)
    data = {"format": 1, "id": mid, "name": "Wide " + mid, "version": version, "console": "xbox", "title_id": TID}
    data.update(extra)
    data = {k: v for k, v in data.items() if v is not None}
    (d / "mod.json").write_text(json.dumps(data))
    return d


def with_patch(d, name="patches/a.bps"):
    p = d / name
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_bytes(b"BPS1")
    return d


def err(fn, *a, **k):
    with pytest.raises(ModError) as e:
        fn(*a, **k)
    return e.value


def test_valid_mod(tmp_path):
    d = with_patch(make_mod(tmp_path, patches=["patches/a.bps"], binary={"sha256": SHA.upper(), "name": "PAL 1.0"},
                            display={"frames": "anamorphic-16:9"}, description="x", authors=["me"], _note="ok"))
    m = package.load(str(d), "xbox", "58420001", SHA)
    assert (m.id, m.version, m.title_id) == ("widescreen", "1.0.0", TID)
    assert m.binary["sha256"] == SHA
    assert m.patches == [os.path.join(str(d), "patches", "a.bps")]
    assert m.display == {"frames": "anamorphic-16:9"}
    assert len(m.json_sha256) == 64


@pytest.mark.parametrize("change,needle", [
    ({"format": None}, "format"),
    ({"format": "1"}, "format"),
    ({"name": None}, "\"name\" is missing"),
    ({"id": "Wide"}, "lower-case"),
    ({"version": "1 0"}, "version"),
    ({"console": "psp"}, "console"),
    ({"title_id": "12345"}, "8 hexadecimal"),
    ({"colour": "red"}, "unknown field \"colour\""),
    ({"patches": ["patches/a.bps"]}, "need \"binary\""),
    ({"binary": {"sha256": "12"}}, "64 hexadecimal"),
    ({"binary": {"sha256": SHA}, "patches": ["patches/a.xdelta"]}, "xdelta is not supported"),
    ({"binary": {"sha256": SHA}, "patches": ["patches/none.bps"]}, "not in the mod"),
    ({"display": {"frames": "stretch"}}, "display.frames"),
    ({"display": {"frames": "anamorphic-16:9", "wide_writes": [{"va": "10", "f32": [1]}]}}, "guest address"),
    ({"display": {"frames": "anamorphic-16:9", "wide_writes": [{"va": "0x10", "hex": "123"}]}}, "even number"),
    ({"display": {"frames": "anamorphic-16:9", "wide_writes": [{"va": "0x10", "hex": "00" * 65}]}}, "at most 64"),
])
def test_bad_schema(tmp_path, change, needle):
    d = make_mod(tmp_path, **change)
    if change.get("format") is None and "format" in change:
        data = json.loads((d / "mod.json").read_text())
        data.pop("format", None)
        (d / "mod.json").write_text(json.dumps(data))
    e = err(package.load, str(d))
    assert needle in str(e), str(e)


def test_newer_format_and_bad_json(tmp_path):
    e = err(package.load, str(make_mod(tmp_path, format=2)))
    assert e.code == "NEWER_FORMAT" and "newer XRecomp" in str(e)
    d = tmp_path / "broken"
    d.mkdir()
    (d / "mod.json").write_text("{nope")
    assert "not valid JSON" in str(err(package.load, str(d)))
    assert err(package.load, str(tmp_path / "absent")).code == "MISSING"


def test_folder_must_be_the_id(tmp_path):
    d = make_mod(tmp_path, mid="one")
    os.rename(d, tmp_path / "two")
    assert "folder is \"two\"" in str(err(package.load, str(tmp_path / "two")))


def test_wrong_title_and_binary(tmp_path):
    d = with_patch(make_mod(tmp_path, patches=["patches/a.bps"], binary={"sha256": SHA, "name": "NTSC 1.1"}))
    e = err(package.load, str(d), "xbox", "58420002")
    assert e.code == "WRONG_TITLE" and "made for title 58420001" in str(e)
    e = err(package.load, str(d), "gba", None)
    assert e.code == "WRONG_TITLE"
    e = err(package.load, str(d), "xbox", TID, "cd" * 32)
    assert e.code == "WRONG_BINARY"
    assert "NTSC 1.1" in str(e) and SHA[:12] in str(e) and "cdcdcdcdcdcd" in str(e)


@pytest.mark.parametrize("rel", ["../x.bps", "patches/../../x.bps", "/etc/x.bps", "patches\\a.bps", "C:/a.bps",
                                 "patches//a.bps", ""])
def test_paths_cannot_leave(tmp_path, rel):
    d = make_mod(tmp_path / "store", binary={"sha256": SHA}, patches=[rel])
    assert err(package.load, str(d)).code == "UNSAFE_PATH"


def test_symlinks_cannot_leave(tmp_path):
    outside = tmp_path / "outside.bps"
    outside.write_bytes(b"BPS1")
    d = make_mod(tmp_path / "store", binary={"sha256": SHA}, patches=["patches/a.bps"])
    (d / "patches").mkdir()
    os.symlink(str(outside), str(d / "patches" / "a.bps"))
    e = err(package.load, str(d))
    assert e.code == "UNSAFE_PATH" and "symbolic link" in str(e)
    # A link elsewhere in the tree (not named by mod.json) is refused too.
    d2 = make_mod(tmp_path / "store", mid="other")
    os.symlink(str(tmp_path), str(d2 / "files"))
    assert err(package.load, str(d2)).code == "UNSAFE_PATH"
    # A link that stays inside is fine.
    d3 = with_patch(make_mod(tmp_path / "store", mid="inside", binary={"sha256": SHA}, patches=["b.bps"]))
    os.symlink("patches/a.bps", str(d3 / "b.bps"))
    package.load(str(d3))


def tdir(root):
    return root / "xbox" / TID


def test_store_enable_order_disable(tmp_path):
    root = tmp_path / "mods"
    for mid in ("a", "b", "c"):
        make_mod(tdir(root), mid=mid)
    r = str(root)
    assert store.installed(r, "xbox", TID) == ["a", "b", "c"]
    assert store.read_enabled(r, "xbox", TID) == []
    assert store.enable(r, "xbox", TID, "b") == ["b"]
    assert store.enable(r, "xbox", TID, "a") == ["b", "a"]
    assert store.enable(r, "xbox", TID, "c", 0) == ["c", "b", "a"]
    assert store.enable(r, "xbox", TID, "a", 1) == ["c", "a", "b"]       # moving one already on
    assert [m.id for m in store.enabled_mods(r, "xbox", TID)] == ["c", "a", "b"]
    assert store.disable(r, "xbox", TID, "a") == ["c", "b"]
    assert json.loads((tdir(root) / "enabled.json").read_text()) == {"format": 1, "enabled": ["c", "b"]}
    assert err(store.enable, r, "xbox", TID, "zzz").code == "MISSING"


def test_enabled_but_broken_is_said(tmp_path):
    root = tmp_path / "mods"
    make_mod(tdir(root), mid="a")
    store.write_enabled(str(root), "xbox", TID, ["a", "gone"])
    e = err(store.enabled_mods, str(root), "xbox", TID)
    assert e.code == "MISSING" and "gone" in str(e)
    (tdir(root) / "enabled.json").write_text('{"enabled": ["a", "a"]}')
    assert "twice" in str(err(store.read_enabled, str(root), "xbox", TID))
    (tdir(root) / "enabled.json").write_text('[1]')
    assert "expected" in str(err(store.read_enabled, str(root), "xbox", TID))


def test_mod_set_hash(tmp_path):
    root = tmp_path / "mods"
    make_mod(tdir(root), mid="a")
    make_mod(tdir(root), mid="b")
    r = str(root)
    assert store.mod_set_hash([]) == "" and store.state_mod_set("") == 0
    store.write_enabled(r, "xbox", TID, ["a", "b"])
    ab = store.mod_set_hash(store.enabled_mods(r, "xbox", TID))
    assert ab == store.mod_set_hash(store.enabled_mods(r, "xbox", TID))            # stable
    store.write_enabled(r, "xbox", TID, ["b", "a"])
    assert store.mod_set_hash(store.enabled_mods(r, "xbox", TID)) != ab            # order counts
    store.write_enabled(r, "xbox", TID, ["a"])
    a = store.mod_set_hash(store.enabled_mods(r, "xbox", TID))
    assert a != ab
    make_mod(tdir(root), mid="a", version="1.0.1")
    assert store.mod_set_hash(store.enabled_mods(r, "xbox", TID)) != a             # version counts
    # Fixed vector: the same bytes give the same hash on every host (a mod.json written byte for byte).
    d = tdir(root) / "a"
    (d / "mod.json").write_bytes(b'{"format": 1, "id": "a", "name": "A", "version": "1", "console": "xbox", '
                                 b'"title_id": "58420001"}')
    h = store.mod_set_hash(store.enabled_mods(r, "xbox", TID))
    assert h == "92cf76b46b42b304602df8c41535f0073176a9a4a44f33062bf9188574dff000"
    assert store.state_mod_set(h) == int(h[:8], 16)
    # The state's 32-bit value: the vector src/kernel tests use (tests/xbox_state, RECOMP_MOD_SET).
    assert store.state_mod_set("8f3e09a1" + "0" * 56) == 0x8F3E09A1
    assert store.state_mod_set("00000000" + "f" * 56) == 1


def test_default_root():
    assert store.default_root({"XBA_MODS_DIR": "/m"}, "linux") == "/m"
    assert store.default_root({"HOME": "/h"}, "linux") == "/h/.local/share/xrecomp/mods"
    assert store.default_root({"HOME": "/h", "XRECOMP_HOME": "/x"}, "linux") == "/x/mods"
    assert store.default_root({"HOME": "/h"}, "darwin") == "/h/Library/Application Support/xboxrecomp/mods"


def test_display_env(tmp_path):
    w = [{"va": "0x10f88", "f32": [16.0, 9.0]}, {"va": "0x20000", "u32": [1]}, {"va": "0x30000", "hex": "aBcd"}]
    d1 = make_mod(tmp_path, mid="one", display={"frames": "anamorphic-16:9", "wide_writes": w})
    m = package.load(str(d1))
    assert m.display["wide_writes"][0]["va"] == "0x00010F88"
    assert display.env([m]) == {
        "RECOMP_MOD_FRAMES": "anamorphic-16:9",
        "RECOMP_MOD_WIDE_WRITES": "00010F88=0000804100001041;00020000=01000000;00030000=ABCD"}
    assert display.env([]) == {}


def test_cli(tmp_path, capsys):
    src = make_mod(tmp_path / "src", mid="a")
    root = str(tmp_path / "mods")
    assert cli.main(["check", str(src)]) == 0
    assert cli.main(["install", str(src), "--mods", root]) == 0
    assert cli.main(["enable", "a", "--mods", root, "--title", TID]) == 0
    assert cli.main(["list", "--mods", root, "--title", TID]) == 0
    assert " 1 a" in capsys.readouterr().out
    assert cli.main(["hash", "--mods", root, "--title", TID]) == 0
    assert len(capsys.readouterr().out.strip()) == 64
    assert cli.main(["check", str(src), "--title", "58420002"]) == 2
    assert "made for title" in capsys.readouterr().err
