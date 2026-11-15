"""The frame generation pins (src/framegen/lsfg_sources.txt, XBA-187) and the places that repeat them: the Android
image's lsfg-sources stage and NOTICE. Offline: nothing is downloaded."""

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def pins():
    out = []
    for line in (ROOT / "src/framegen/lsfg_sources.txt").read_text().splitlines():
        if line.strip() and not line.startswith("#"):
            name, sha, url = line.split()
            out.append((name, sha, url))
    return out


def test_pins_are_well_formed():
    p = pins()
    assert [n for n, _, _ in p] == ["lsfg-vk-android", "volk", "dxvk", "spirv-headers"]
    for name, sha, url in p:
        assert re.fullmatch(r"[0-9a-f]{64}", sha), name
        # A commit archive, never a branch or tag that can move.
        assert re.fullmatch(r"https://codeload\.github\.com/[\w.-]+/[\w.-]+/tar\.gz/[0-9a-f]{40}", url), name


def test_android_image_adds_the_same_tarballs():
    df = (ROOT / "tools/docker/android.Dockerfile").read_text()
    stage = df[df.index("FROM scratch AS lsfg-sources"):]
    stage = stage[:stage.index("\nFROM ")]
    adds = re.findall(r"ADD --checksum=sha256:([0-9a-f]{64}) \\\n\s+(\S+) /(\S+)", stage)
    assert sorted(adds) == sorted((sha, url, f"{name}-{sha[:16]}.tar.gz") for name, sha, url in pins())


def test_notice_names_the_pinned_commits():
    notice = (ROOT / "NOTICE").read_text()
    for _, _, url in pins():
        assert url.rsplit("/", 1)[1][:8] in notice, url
