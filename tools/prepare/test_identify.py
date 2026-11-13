"""tools.prepare.identify: one source's title ID, name and size for a
launcher's library scan, from the synthetic tools/conformance/test.xbe, alone,
in a folder and in an image made by tools/xiso/mkxiso.py. No game data."""

import json
import os
import shutil
import subprocess
import sys

import pytest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, ROOT)

from tools.prepare import identify                     # noqa: E402
from tools.xiso import mkxiso                          # noqa: E402

TEST_XBE = os.path.join(ROOT, "tools", "conformance", "test.xbe")


def _folder(tmp_path, name="game", data=True):
    d = tmp_path / name
    d.mkdir(parents=True)
    shutil.copy(TEST_XBE, d / "default.xbe")
    if data:
        (d / "media").mkdir()
        (d / "media" / "data.bin").write_bytes(b"\x01" * 5000)
    return d


def _image(tmp_path, data=True, **kw):
    out = tmp_path / "Synth (World).iso"
    mkxiso.build(str(_folder(tmp_path, "disc", data)), str(out), **kw)
    return out


def _cli(*args):
    p = subprocess.run([sys.executable, "-m", "tools.prepare.identify"] + [str(a) for a in args],
                       cwd=ROOT, capture_output=True, text=True)
    return p.returncode, p.stdout, p.stderr


def test_xbe_folder_and_image_agree(tmp_path):
    size = os.path.getsize(TEST_XBE)
    with open(TEST_XBE, "rb") as f:
        xbe_bytes = f.read()
    a = identify.identify(TEST_XBE)
    b = identify.identify(str(_folder(tmp_path)))
    iso = _image(tmp_path)
    out = tmp_path / "cache" / "12345678" / "default.xbe"
    c = identify.identify(str(iso), xbe_out=str(out))
    for r in (a, b, c):
        assert r["title_id"] == "12345678"
        assert r["name"] == "SYNTH"
        assert r["xbe_size"] == size
        assert r["has_title_image"] is False
    assert (a["kind"], b["kind"], c["kind"]) == ("xbe", "folder", "image")
    assert a["size"] == size and b["size"] == size
    assert c["size"] == os.path.getsize(iso)
    assert a["xbe"] == TEST_XBE and b["xbe"] == str(tmp_path / "game" / "default.xbe")
    # Only default.xbe comes out of the image, and only where asked.
    assert c["xbe"] == str(out) and out.read_bytes() == xbe_bytes
    assert sorted(os.listdir(out.parent)) == ["default.xbe"]
    assert identify.identify(str(iso))["xbe"] is None


def test_cli_json(tmp_path):
    iso = _image(tmp_path)
    out = tmp_path / "x.xbe"
    rc, stdout, _ = _cli(iso, "--json", "--xbe-out", out)
    assert rc == 0
    lines = stdout.splitlines()
    assert len(lines) == 1
    r = json.loads(lines[0])
    assert {"title_id", "name", "size", "kind", "xbe", "profile_name"} <= set(r)
    assert (r["title_id"], r["name"], r["kind"], r["xbe"]) == ("12345678", "SYNTH", "image", str(out))
    rc, stdout, _ = _cli(TEST_XBE)
    assert rc == 0 and stdout.startswith("12345678  SYNTH  (xbe, ")


@pytest.mark.parametrize("make,code", [
    (lambda t: t, "NO_XBE"),                                                       # a folder without default.xbe
    (lambda t: t / "missing.iso", "NO_XBE"),
    (lambda t: _image(t, data=False, truncate=-4096), "BAD_IMAGE"),                 # still being copied
    (lambda t: _image(t, corrupt="bad-magic"), "BAD_IMAGE"),
    (lambda t: (t / "default.xbe").write_bytes(b"XBEH" + b"\0" * 64) and t / "default.xbe", "BAD_XBE"),
    (lambda t: (t / "default.xbe").write_bytes(b"MZ" * 400) and t / "default.xbe", "BAD_XBE"),
])
def test_errors(tmp_path, make, code):
    d = tmp_path / "src"
    d.mkdir()
    src = make(d)
    with pytest.raises(identify.IdentifyError) as e:
        identify.identify(str(src))
    assert e.value.code == code
    rc, stdout, _ = _cli(src, "--json")
    assert rc == 1 and json.loads(stdout)["error"]["code"] == code


def test_profile_name(tmp_path, monkeypatch):
    t = tmp_path / "titles" / "12345678"
    t.mkdir(parents=True)
    (t / "title.json").write_text(json.dumps({"name": "Synthetic Title"}))
    monkeypatch.setattr(identify, "PROFILES", str(tmp_path))
    r = identify.identify(TEST_XBE)
    assert (r["name"], r["profile_name"]) == ("SYNTH", "Synthetic Title")
