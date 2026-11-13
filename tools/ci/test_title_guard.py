"""title_guard.py (XBA-109): the toolkit is clean, and each rule fails on a planted violation in a synthetic tree.
Runs with the rest of `pytest tools/` in the linux job; this is the CI guard itself."""
import os
import subprocess
import sys

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
import title_guard as tg  # noqa: E402

# Built at run time so this file holds no title ID or game name as a token of its own (it is exempt as a test, but
# the guard's lists are the only place they should be spelled out).
TID = "4D53" + "00D1"           # a retail form: « MS » + game number
NAME = "Fa" + "ble"
ADDR = "0x0036" + "B7C0"        # in EXTRA_GAME_ADDRESSES
PROFILE_ADDR = "0x0012" + "3450"


def test_toolkit_is_clean():
    found = tg.scan(ROOT)
    assert found == [], "\n".join("%s:%d: %s %s (%s)" % f for f in found)


def tree(tmp_path, files, allow=""):
    for rel, text in files.items():
        p = tmp_path / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(text)
    (tmp_path / "tools" / "ci").mkdir(parents=True, exist_ok=True)
    (tmp_path / "tools" / "ci" / "title_guard_allow.txt").write_text(allow)
    (tmp_path / "profiles" / "titles" / "x").mkdir(parents=True, exist_ok=True)
    (tmp_path / "profiles" / "titles" / "x" / "title.json").write_text('{"params": {"P": "%s", "S": "0x20"}}'
                                                                         % PROFILE_ADDR)
    (tmp_path / "profiles" / "titles" / "x" / "title.c").write_text(
        "void sub_00456780(void);\nvoid f(void) { MEM32(eax + 0x100410) = 1; }\n")
    return [(rel, line, rule, token, kind) for rel, line, rule, token, kind in tg.scan(str(tmp_path))]


def rules(found):
    return sorted((rel, rule, kind) for rel, _, rule, _, kind in found)


@pytest.mark.parametrize("rel,text,rule,kind", [
    ("src/a.c", "if (tid == 0x%s) return;\n" % TID, "title-id", "code"),
    ("src/a.c", 'const char *t = "%s";\n' % TID.lower(), "title-id", "string"),
    ("src/a.c", "static int title_%s_hooks;\n" % TID.lower(), "title-id", "code"),
    ("src/a.h", "#ifndef %s3_STATE_H\n" % "BURNOUT", "game-name", "code"),
    ("src/a.c", "static int is%sHook;\n" % NAME, "game-name", "code"),
    ("src/a.c", 'puts("%s: The Lost Chapters");\n' % NAME, "game-name", "string"),
    ("src/a.c", "#define BASE %s\n" % ADDR, "game-address", "code"),
    ("src/a.c", "uint32_t p = %s;\n" % PROFILE_ADDR, "game-address", "code"),
    ("src/a.c", "extern void sub_00456780(void);\n", "game-address", "code"),
    ("tools/x.py", "TITLE = '%s'\n" % TID, "title-id", "string"),
    ("tools/x.py", '"""Usage: python3 -m tools.prepare %s"""\n' % TID, "title-id", "docstring"),
    ("tools/x.py", "def f():\n    '''Base %s.'''\n" % ADDR, "game-address", "docstring"),
    ("tools/x.py", "if %s_MODE:\n    pass\n" % NAME.upper(), "game-name", "code"),
    ("android/app/src/main/java/A.java", 'String s = "%s";\n' % NAME, "game-name", "string"),
    ("android/app/src/main/res/values/strings.xml", "<string name=\"a\">as %s does</string>\n" % NAME,
     "game-name", "code"),
    ("tools/x.sh", "run --title %s\n" % TID, "title-id", "code"),
    ("templates/new-game/CMakeLists.txt", "set(T %s)\n" % TID, "title-id", "code"),
])
def test_planted_violation_fails(tmp_path, rel, text, rule, kind):
    found = tree(tmp_path, {rel: text})
    assert rules(found) == [(rel, rule, kind)], found


@pytest.mark.parametrize("rel,text", [
    ("src/a.c", "/* %s (%s) does X, hence this generic path; %s was its base. */\nint x;\n" % (NAME, TID, ADDR)),
    ("src/a.c", "int x; // seen first in %s %s\n" % (NAME, TID)),
    ("tools/x.py", "# %s does X\nx = 1  # %s\n" % (NAME, TID)),
    ("tools/x.py", '"""%s does X: why this generic code exists."""\n' % NAME),     # a name in prose is history
    ("tools/x.sh", "# usage: run --title %s\nrun\n" % TID),
    ("android/app/src/main/res/values/strings.xml", "<!-- %s -->\n<string name=\"a\">b</string>\n" % NAME),
    ("src/a.c", "uint32_t id = 0x58424104, other = 0x58420003;\n"),                # ours (« XB »)
    ("src/a.c", "uint32_t magic = 0x48454258; /* 'XBEH': low word >= 0x1000 */\n"),
    ("src/a.c", "MEM32(eax + 0x100410) = 0; int s = 0x20;\n"),                    # a profile's C constant
    ("src/a.c", 'const char *s = "Halogen black";\n'),
    ("docs/notes.md", "%s %s %s\n" % (NAME, TID, ADDR)),                           # docs are not scanned
    ("src/README.md", "%s %s\n" % (NAME, TID)),                                    # nor Markdown in the trees
    ("tools/prepare/test_x.py", "TID = '%s'  # %s\n" % (TID, NAME)),               # tests' own data
    ("android/app/src/test/java/T.java", 'String s = "%s";\n' % NAME),
    ("tests/a/test_main.c", "int t = 0x%s;\n" % TID),                              # outside the scanned trees
])
def test_allowed_forms_pass(tmp_path, rel, text):
    assert tree(tmp_path, {rel: text}) == []


def test_allow_list_token_and_glob(tmp_path):
    files = {"src/a.c": "int a = %s;\nint b = 0x%s;\n" % (ADDR, TID), "tools/mock/x.py": "N = '%s'\n" % NAME}
    allow = "src/a.c %s   # reason\ntools/mock/*   # whole files\n" % ADDR[2:].upper()
    found = tree(tmp_path, files, allow)
    assert [(r, rule) for r, _, rule, _, _ in found] == [("src/a.c", "title-id")]


def test_line_numbers(tmp_path):
    found = tree(tmp_path, {"src/a.c": "/* a\n b */\nint x;\n\nint y = %s;\n" % ADDR,
                            "android/x.xml": "<a>\n<!-- c\n d -->\n<b>%s</b>\n" % NAME})
    assert sorted((r, line) for r, line, _, _, _ in found) == [("android/x.xml", 4), ("src/a.c", 5)]


def test_cli_exit_codes(tmp_path):
    tree(tmp_path, {"src/a.c": "int x;\n"})
    guard = os.path.join(HERE, "title_guard.py")
    ok = subprocess.run([sys.executable, guard, "--root", str(tmp_path)], capture_output=True, text=True)
    assert ok.returncode == 0, ok.stdout
    (tmp_path / "src" / "b.c").write_text("int t = 0x%s;\n" % TID)
    bad = subprocess.run([sys.executable, guard, "--root", str(tmp_path)], capture_output=True, text=True)
    assert bad.returncode == 1 and "src/b.c:1: title-id %s" % TID in bad.stdout
