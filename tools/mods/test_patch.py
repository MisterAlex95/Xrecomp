"""tools.mods: BPS and IPS patches, and the enabled mods applied in load order (MOD-3, XBA-288)."""
import json
import os
import zlib

import pytest

from tools.mods import bps, ips, patch, store
from tools.mods.package import ModError

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
TEST_XBE = os.path.join(ROOT, "tools", "conformance", "test.xbe")


def crc(b):
    return (zlib.crc32(b) & 0xFFFFFFFF).to_bytes(4, "little")


# ── BPS ──

def test_bps_hand_assembled_vector():
    """A patch written byte by byte from the specification (not by our encoder): source "ABCD" -> "ABXD"."""
    src, tgt = b"ABCD", b"ABXD"
    body = b"BPS1" + bytes([0x84, 0x84, 0x80])          # sizes 4, 4, metadata 0 (numbers end with bit 7 set)
    body += bytes([0x84])                               # SourceRead, length 2: ((2-1) << 2) | 0
    body += bytes([0x81]) + b"X"                        # TargetRead, length 1: ((1-1) << 2) | 1
    body += bytes([0x80])                               # SourceRead, length 1
    body += crc(src) + crc(tgt)
    p = body + crc(body)
    assert bps.apply(src, p) == tgt


def test_bps_copies_from_the_spec():
    """SourceCopy (negative offset) and TargetCopy (an overlapping run), hand-assembled: "abcdef" -> "efab" + "zzzz"."""
    src = b"abcdef"
    tgt = b"efabzzzz"
    body = b"BPS1" + bps.enc_num(6) + bps.enc_num(8) + bps.enc_num(0)
    body += bps.enc_num(((2 - 1) << 2) | 2) + bps.enc_num(4 << 1)            # SourceCopy 2 from +4: "ef"
    body += bps.enc_num(((2 - 1) << 2) | 2) + bps.enc_num((6 << 1) | 1)      # SourceCopy 2 from 6-6=0: "ab"
    body += bps.enc_num(((1 - 1) << 2) | 1) + b"z"                           # TargetRead "z"
    body += bps.enc_num(((3 - 1) << 2) | 3) + bps.enc_num(4 << 1)            # TargetCopy 3 from 4: "zzz"
    body += crc(src) + crc(tgt)
    assert bps.apply(src, body + crc(body)) == tgt


def test_bps_numbers():
    for n in (0, 1, 127, 128, 129, 16511, 16512, 1 << 32):
        b = bps.enc_num(n)
        assert bps._num(b, 0, len(b)) == (n, len(b))
    assert bps.enc_num(0) == b"\x80" and bps.enc_num(128) == b"\x00\x80"


@pytest.mark.parametrize("src,tgt", [(b"", b""), (b"same", b"same"), (b"0123456789" * 50, b"0123X56789" * 50),
                                     (b"short", b"longer target"), (b"longer source", b"short"),
                                     (bytes(range(256)) * 4, bytes(reversed(range(256))) * 4)])
def test_bps_round_trip(src, tgt):
    p = bps.make(src, tgt, b"meta")
    assert bps.apply(src, p) == tgt
    assert bps.info(p)["metadata"] == b"meta"


def test_bps_refusals():
    p = bps.make(b"original", b"modified")
    with pytest.raises(bps.BpsError) as e:
        bps.apply(b"another!", p)
    assert e.value.kind == "source"
    damaged = bytearray(p)
    damaged[6] ^= 1
    with pytest.raises(bps.BpsError) as e:
        bps.apply(b"original", bytes(damaged))
    assert e.value.kind == "patch"
    with pytest.raises(bps.BpsError) as e:
        bps.apply(b"x", b"PATCH...")
    assert e.value.kind == "format"


# ── IPS ──

def test_ips_hand_assembled_vector():
    p = b"PATCH" + b"\x00\x00\x02" + b"\x00\x01" + b"X" + b"\x00\x00\x05" + b"\x00\x00" + b"\x00\x03" + b"z" + b"EOF"
    assert ips.apply(b"ABCD", p) == b"ABXD\x00zzz"
    assert ips.apply(b"ABCDEFGH", b"PATCHEOF\x00\x00\x03") == b"ABC"          # truncation extension


@pytest.mark.parametrize("src,tgt", [(b"same", b"same"), (b"0123456789" * 50, b"0123X56789" * 50),
                                     (b"short", b"longer target"), (b"longer source", b"short")])
def test_ips_round_trip(src, tgt):
    assert ips.apply(src, ips.make(src, tgt)) == tgt


def test_ips_eof_offset():
    """A record cannot start at 0x454F46 ("EOF"): it starts a byte earlier."""
    src = bytes(0x454F50)
    tgt = bytearray(src)
    tgt[0x454F46] = 1
    p = ips.make(src, bytes(tgt))
    assert p[5:8] == (0x454F45).to_bytes(3, "big")
    assert ips.apply(src, p) == bytes(tgt)


def test_ips_refusals():
    for bad in (b"NOPE", b"PATCH\x00\x00", b"PATCH\x00\x00\x01\x00\x05ab", b"PATCH"):
        with pytest.raises(ips.IpsError):
            ips.apply(b"abc", bad)


# ── the enabled mods, in load order ──

TID = "12345678"         # tools/conformance/test.xbe's


def xbe_bytes():
    with open(TEST_XBE, "rb") as f:
        return f.read()


def install(root, mid, patches, binary_sha, display=None):
    d = root / "xbox" / TID / mid
    (d / "patches").mkdir(parents=True)
    names = []
    for i, (kind, raw) in enumerate(patches):
        n = "patches/p%d.%s" % (i, kind)
        (d / n).write_bytes(raw)
        names.append(n)
    data = {"format": 1, "id": mid, "name": mid.title(), "version": "1", "console": "xbox", "title_id": TID,
            "binary": {"sha256": binary_sha, "name": "test 1.0"}, "patches": names}
    if display:
        data["display"] = display
    (d / "mod.json").write_text(json.dumps(data))
    return d


def changed(data, at, value):
    b = bytearray(data)
    b[at] = value
    return bytes(b)


def test_xbe_title_id():
    assert patch.xbe_title_id(xbe_bytes()) == TID
    assert patch.xbe_title_id(b"nope" * 100) is None


def test_plan_applies_in_load_order(tmp_path):
    root = tmp_path / "mods"
    data = xbe_bytes()
    sha = patch.sha256(data)
    one = changed(data, 0x400, 0x11)
    install(root, "first", [("bps", bps.make(data, one))], sha)
    # The second is an IPS on top of the first's result (IPS carries no source check: the mod's binary does).
    two = changed(one, 0x401, 0x22)
    install(root, "second", [("ips", ips.make(one, two))], sha, display={"frames": "anamorphic-16:9"})
    store.write_enabled(str(root), "xbox", TID, ["first", "second"])
    p = patch.plan("xbox", TID, data, root=str(root), off=False)
    assert p.data == two and p.patched and p.sha256 == patch.sha256(two) and p.original_sha256 == sha
    assert p.ids() == ["first", "second"] and len(p.mod_set) == 64
    env = p.env("/w/mods/default.xbe")
    assert env == {"RECOMP_MOD_SET": p.mod_set, "RECOMP_MOD_FRAMES": "anamorphic-16:9",
                   "RECOMP_MOD_XBE": "/w/mods/default.xbe", "RECOMP_MOD_FOR": patch.binary_key(data)}
    assert patch.binary_key(b"123456789") == "9-CBF43926"         # the runtime's key (tests/xbox_mods: same CRC)
    assert data == xbe_bytes()                              # the original's bytes untouched
    # Switched off (the app's Enhancements switch): the game as it is.
    p = patch.plan("xbox", TID, data, root=str(root), off=True)
    assert p.data == data and not p.patched and p.mods == [] and p.env("/x") == {}
    # Nothing on: the same.
    store.write_enabled(str(root), "xbox", TID, [])
    p = patch.plan("xbox", TID, data, root=str(root), off=False)
    assert not p.patched and p.mod_set == ""


def test_plan_refuses_another_version(tmp_path):
    root = tmp_path / "mods"
    data = xbe_bytes()
    install(root, "wide", [("bps", bps.make(b"another", b"anothe2"))], "cd" * 32)
    store.write_enabled(str(root), "xbox", TID, ["wide"])
    with pytest.raises(ModError) as e:
        patch.plan("xbox", TID, data, root=str(root), off=False)
    assert e.value.code == "WRONG_BINARY" and "test 1.0" in str(e.value) and "cdcdcdcdcdcd" in str(e.value)


def test_plan_bps_made_for_another_file(tmp_path):
    """The mod names the right binary but its BPS was made for something else: refused by the BPS source check."""
    root = tmp_path / "mods"
    data = xbe_bytes()
    install(root, "wide", [("bps", bps.make(b"another", b"anothe2"))], patch.sha256(data))
    store.write_enabled(str(root), "xbox", TID, ["wide"])
    with pytest.raises(ModError) as e:
        patch.plan("xbox", TID, data, root=str(root), off=False)
    assert e.value.code == "WRONG_BINARY" and "another version" in str(e.value)


def test_plan_two_bps_on_the_original(tmp_path):
    """Two BPS made for the unmodified game: the second does not fit on top of the first, and the message says
    what to do."""
    root = tmp_path / "mods"
    data = xbe_bytes()
    sha = patch.sha256(data)
    install(root, "a", [("bps", bps.make(data, changed(data, 0x400, 1)))], sha)
    install(root, "b", [("bps", bps.make(data, changed(data, 0x500, 2)))], sha)
    store.write_enabled(str(root), "xbox", TID, ["a", "b"])
    with pytest.raises(ModError) as e:
        patch.plan("xbox", TID, data, root=str(root), off=False)
    assert "made for the unmodified game, and a changed it first" in str(e.value)


def test_switched_off():
    assert patch.switched_off({"XBA_NO_MODS": "1"}) and patch.switched_off({"XBA_NO_TITLE_LAYER": "1"})
    assert not patch.switched_off({}) and not patch.switched_off({"XBA_NO_MODS": "0"})
