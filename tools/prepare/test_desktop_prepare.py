"""tools.prepare (desktop): the title project main.c and the profile digest
the stamps hold."""

import json
import os
import sys

sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..")))

from tools.prepare import __main__ as prep           # noqa: E402
from tools.prepare import profiles                    # noqa: E402

INFO = {"title": "Burnout 3", "title_id": "0x4541005B", "entry_point": "0x001D2B27"}


def _profile(tmp_path, **kv):
    d = tmp_path / "titles" / "4541005b"
    d.mkdir(parents=True)
    (d / "title.c").write_text("void bo3_configure(void) {}\nvoid bo3_start(const char *g, const char *s) {}\n")
    (d / "title.json").write_text(json.dumps(dict(kv, hooks="bo3")))
    return profiles.resolve("4541005B", None, str(tmp_path))


def test_main_c_gets_name_and_entry_point(tmp_path):
    """The template's placeholders were left in: a run printed
    `=== YOUR_GAME_NAME` and `Entry point: 0x00000000`."""
    proj = tmp_path / "project"
    name = prep.assemble_project(str(proj), _profile(tmp_path, name="Burnout 3: Takedown",
                                                     project="burnout3"), INFO)
    assert name == "burnout3"
    main = (proj / "src" / "main.c").read_text()
    assert "YOUR_GAME_NAME" not in main
    assert '"=== Burnout 3: Takedown - Static Recompilation ===\\n"' in main
    assert "#define YOUR_GAME_ENTRY_POINT   0x001D2B27" in main
    assert " *   Title ID:    0x4541005B" in main
    assert " *   Entry point: 0x001D2B27" in main
    assert '"game/default.xbe"' in main
    assert (proj / "src" / "title" / "titles_4541005b__title.c").is_file()
    assert "    bo3_configure();" in (proj / "src" / "title" / "xba_layers.c").read_text()
    assert (proj / "src" / "title" / "xba_params.h").is_file()


def test_main_c_without_profile_uses_the_xbe_title(tmp_path):
    proj = tmp_path / "project"
    info = dict(INFO, title='50% "off" \\ */ ??=')
    assert prep.assemble_project(str(proj), None, info) == "title_4541005b"
    main = (proj / "src" / "main.c").read_text()
    # A printf format, a C string and a comment: escaped for all three.
    assert '"=== 50%% \\"off\\" \\\\ * / ?\\?= - Static Recompilation ===\\n"' in main


def test_profile_digest_follows_the_files(tmp_path):
    p = _profile(tmp_path)
    a = prep.profile_digest(p)
    assert a == prep.profile_digest(p)
    (tmp_path / "titles" / "4541005b" / "seed_functions.json").write_text("[]")
    b = prep.profile_digest(p)
    assert b != a
    assert prep.profile_digest(None) not in (a, b)
    # A lower layer's file counts too.
    x = tmp_path / "engines" / "renderware"
    x.mkdir(parents=True)
    (x / "layer.json").write_text(json.dumps({}))
    ident = {"xdk": {"libraries": {"D3D8": 5849}}, "engines": [{"layer": "renderware", "kind": "engine"}]}
    c = prep.profile_digest(profiles.resolve("4541005B", ident, str(tmp_path)))
    (x / "rw.c").write_text("/* */\n")
    assert prep.profile_digest(profiles.resolve("4541005B", ident, str(tmp_path))) not in (b, c)


def test_sync_generated_keeps_unchanged_files(tmp_path):
    """XBA-65: a re-lift rewrites only what changed, so the build recompiles only that."""
    gen, new = tmp_path / "gen", tmp_path / "gen.new"
    gen.mkdir()
    new.mkdir()
    for fn, text in (("a.c", "same"), ("b.c", "old"), ("gone.c", "x")):
        (gen / fn).write_text(text)
    os.utime(gen / "a.c", (1000, 1000))
    (new / "a.c").write_text("same")
    (new / "b.c").write_text("new")
    (new / "c.c").write_text("added")
    assert prep.sync_generated(str(new), str(gen)) == (1, 2, 1)
    assert sorted(os.listdir(gen)) == ["a.c", "b.c", "c.c"] and not new.exists()
    assert os.stat(gen / "a.c").st_mtime == 1000             # untouched: no rebuild
    assert (gen / "b.c").read_text() == "new"
