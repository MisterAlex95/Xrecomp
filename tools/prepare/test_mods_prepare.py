"""tools.prepare with the player's mods (MOD-3, XBA-288; docs/technical/mods.md): the patches go to a copy of the
XBE that is analysed and lifted, the original is never written, the stamp follows the patched copy and the mod set
(--check says "mods"), a mod made for another version stops the prepare with the reason, and turning the mods off
brings the unpatched project back. With the synthetic tools/conformance/test.xbe."""

import hashlib
import json
import os
import shutil
import sys

import pytest

sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..")))

from tools.mods import bps, store                       # noqa: E402
from tools.prepare import __main__ as prep             # noqa: E402

ROOT = prep.ROOT
TEST_XBE = os.path.join(ROOT, "tools", "conformance", "test.xbe")
TID = "12345678"

needs_pipeline = pytest.mark.skipif(
    not all(__import__("importlib").util.find_spec(m) for m in ("capstone", "pefile")),
    reason="the analysis pipeline needs capstone and pefile")


def sha(path):
    with open(path, "rb") as f:
        return hashlib.sha256(f.read()).hexdigest()


def install(root, mid, data, target, binary_sha, display=None):
    d = root / "xbox" / TID / mid
    (d / "patches").mkdir(parents=True)
    (d / "patches" / "default.bps").write_bytes(bps.make(data, target))
    m = {"format": 1, "id": mid, "name": "Test mod", "version": "1.0", "console": "xbox", "title_id": TID,
         "binary": {"sha256": binary_sha, "name": "synthetic 1.0"}, "patches": ["patches/default.bps"]}
    if display:
        m["display"] = display
    (d / "mod.json").write_text(json.dumps(m))


@needs_pipeline
def test_prepare_with_a_mod_and_back(tmp_path, monkeypatch, capsys):
    src = tmp_path / "src"
    src.mkdir()
    shutil.copy(TEST_XBE, src / "default.xbe")
    xbe, work, mods = str(src / "default.xbe"), tmp_path / "work", tmp_path / "mods"
    monkeypatch.setenv("XBA_MODS_DIR", str(mods))
    monkeypatch.delenv("XBA_NO_TITLE_LAYER", raising=False)
    monkeypatch.delenv("XBA_NO_MODS", raising=False)
    original = sha(xbe)
    with open(xbe, "rb") as f:
        data = f.read()
    stamp = lambda: json.loads((work / "stamp.json").read_text())        # noqa: E731
    env_h = work / "project" / "src" / "title" / "env_force.h"

    # 1. No mod: the game as it is.
    assert prep.main([xbe, "--work", str(work), "--no-build"]) == 0
    assert stamp()["xbe"] == original and stamp().get("mods", "") == ""
    assert not env_h.exists() and not (work / "mods").exists()

    # 2. A mod that changes one byte, turned on: --check sees it before the prepare does.
    target = bytearray(data)
    target[0x400] ^= 0xFF
    install(mods, "onebyte", data, bytes(target), original, display={"frames": "anamorphic-16:9"})
    store.enable(str(mods), "xbox", TID, "onebyte")
    # --check on a built title (a planted executable): stale, reason "mods".
    exe = work / "project" / "build" / "title_12345678"
    exe.parent.mkdir(parents=True, exist_ok=True)
    exe.write_text("#!/bin/sh\n")
    st = stamp()
    st["built"] = True
    (work / "stamp.json").write_text(json.dumps(st))
    chk = prep.check_state(xbe, str(work))
    assert chk["state"] == "stale" and "mods" in chk["reasons"] and chk["title_layer"] == "on", chk
    assert prep.check_mod_set(TID) == store.mod_set_hash(store.enabled_mods(str(mods), "xbox", TID))
    assert prep.main([xbe, "--work", str(work), "--no-build"]) == 0
    out = capsys.readouterr().out
    assert "mods on, in load order: onebyte 1.0 (binary patched)" in out
    patched = work / "mods" / "default.xbe"
    assert patched.read_bytes() == bytes(target)
    assert sha(xbe) == original                                           # never the original
    s = stamp()
    assert s["xbe"] == sha(str(patched)) and s["xbe_source"] == original and s["mods"] == prep.check_mod_set(TID)
    h = env_h.read_text()
    assert '"RECOMP_MOD_SET", "%s"' % s["mods"] in h and '"RECOMP_MOD_FRAMES", "anamorphic-16:9"' in h
    assert '"RECOMP_MOD_XBE", "%s"' % patched in h and '"RECOMP_MOD_FOR", "%d-' % len(data) in h
    st = stamp()
    st["built"] = True
    (work / "stamp.json").write_text(json.dumps(st))
    assert "mods" not in prep.check_state(xbe, str(work))["reasons"]       # up to date with the store again
    # The analysis ran on the patched copy (its JSON beside it).
    assert (work / "mods" / "default_analysis.json").is_file()

    # 3. Switched off for one prepare (the launcher's Enhancements switch): the original again.
    monkeypatch.setenv("XBA_NO_TITLE_LAYER", "1")
    assert prep.title_layer_state(TID) == "off"
    assert prep.main([xbe, "--work", str(work), "--no-build"]) == 0
    assert stamp()["xbe"] == original and stamp()["mods"] == "" and not env_h.exists()
    monkeypatch.delenv("XBA_NO_TITLE_LAYER")
    assert prep.title_layer_state(TID) == "on"

    # 4. A mod made for another version: refused with the version it expects; nothing lifted from it.
    install(mods, "other", data, bytes(target), "ab" * 32)
    store.enable(str(mods), "xbox", TID, "other")
    with pytest.raises(SystemExit) as e:
        prep.main([xbe, "--work", str(work), "--no-build"])
    assert "made for another version" in str(e.value.code) and "synthetic 1.0" in str(e.value.code)

    # 5. Every mod off: back to the unpatched project.
    store.write_enabled(str(mods), "xbox", TID, [])
    assert prep.title_layer_state(TID) is None
    assert prep.main([xbe, "--work", str(work), "--no-build"]) == 0
    assert stamp()["xbe"] == original and stamp()["mods"] == "" and not env_h.exists()
    assert sha(xbe) == original


def test_android_identify_patches_a_copy(tmp_path, monkeypatch):
    """The Android prepare's identify step: the mods' patches go to <work>/mods/default.xbe, the stamp follows that
    copy, pack.json's xbe_sha256 stays the game's own (what the app compares) and the mods' options are forced."""
    from tools.prepare import android, pack
    src = tmp_path / "game"
    src.mkdir()
    shutil.copy(TEST_XBE, src / "default.xbe")
    with open(TEST_XBE, "rb") as f:
        data = f.read()
    original = hashlib.sha256(data).hexdigest()
    target = bytearray(data)
    target[0x400] ^= 0xFF
    mods = tmp_path / "mods"
    install(mods, "onebyte", data, bytes(target), original)
    store.enable(str(mods), "xbox", TID, "onebyte")
    monkeypatch.setenv("XBA_MODS_DIR", str(mods))
    monkeypatch.delenv("XBA_NO_TITLE_LAYER", raising=False)

    class Stop(Exception):
        pass

    class A:
        work = str(tmp_path / "work")
        source = str(src)
        force = False
        python = None

    def no_parse(*a, **k):
        raise Stop()
    monkeypatch.setattr(android.toolkit, "digest", lambda: "11" * 32)
    r = android.Run(A(), prep)
    monkeypatch.setattr(r.procs, "run_logged", no_parse)
    with pytest.raises(Stop):
        r.identify()
    assert r.xbe == os.path.join(A.work, "mods", "default.xbe")
    assert open(r.xbe, "rb").read() == bytes(target)
    assert r.xbe_sha == original and r.stamp["xbe"] == hashlib.sha256(bytes(target)).hexdigest()
    assert r.mods.ids() == ["onebyte"] and r.mods.patched
    assert open(os.path.join(A.work, "xbe", "default.xbe"), "rb").read() == data      # the plain copy stays
    assert pack.sha256_file(str(src / "default.xbe")) == original
    # A disc's other image (other_images()): no mod, even of the same title.
    A.other_image = True
    A.work = str(tmp_path / "work-image")
    r = android.Run(A(), prep)
    monkeypatch.setattr(r.procs, "run_logged", no_parse)
    with pytest.raises(Stop):
        r.identify()
    assert r.mods.mods == [] and r.xbe == os.path.join(A.work, "xbe", "default.xbe")
