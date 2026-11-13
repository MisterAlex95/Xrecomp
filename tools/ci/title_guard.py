#!/usr/bin/env python3
"""No per-title code in the toolkit (XBA-109): a retail title ID, a game's name or one game's guest address in
src/, include/, templates/, tools/ or android/ fails the build.

Runs in the linux CI job through tools/ci/test_title_guard.py (the `pytest tools/` pass), or by hand:

    python3 tools/ci/title_guard.py [--root DIR] [--list-tokens FILE]

What is checked: CODE TOKENS only. Each file is split by a small lexer of its language into four kinds of text:

    code       identifiers, numbers, operators, preprocessor lines
    string     string and character literals (C, C++, Java, Python, shell quotes are code)
    docstring  a Python string literal standing alone as a statement (module, class, def or any bare string)
    comment    // and /* */ (C family), # (Python, shell, CMake, text lists), <!-- --> (XML)

    rule                  code  string  docstring  comment
    retail title ID        yes    yes      yes        no
    game name              yes    yes      no         no
    game address           yes    yes      yes        no

Comments are history: « Fable does X, hence this generic path » explains why generic code exists and stays allowed,
title ID included. A docstring is prose too, so it may name a game, but it is often printed (argparse
`description=__doc__`) and holds copy-paste usage examples, so a title ID or a game address in it fails.

  retail title ID  8 hex digits, as a 0x literal or a bare word (strings, identifiers: title_4d5300d1), whose two
                   high bytes are upper-case ASCII letters (the publisher code: 4D53 « MS », 4541 « EA ») and whose
                   low word is below 0x1000 (the game number). Our synthetic IDs are listed in SYNTHETIC_TITLE_IDS.
  game name        GAME_NAMES, matched on whole words of identifiers and literals (camelCase, snake_case and digits
                   split words: isFableHook, BURNOUT3_X and "Burnout 3" match; Halogen does not).
  game address     a 0x literal whose value one game owns: every 0x value >= 0x10000 in profiles/titles/** (a title's
                   profile is by definition per-title data) plus EXTRA_GAME_ADDRESSES.

Not scanned: Markdown (docs), binary files, test sources (tools/**/test_*.py, conftest.py, android/**/src/test/**,
android/**/src/androidTest/**: tests' own data), and whatever tools/ci/title_guard_allow.txt lists (whole files, or
one token in one file, each with its reason). profiles/ and docs/ are outside the scanned trees.
"""
import argparse
import ast
import fnmatch
import io
import os
import re
import sys
import tokenize

SCANNED = ("src", "include", "templates", "tools", "android")
SKIP_DIRS = {".git", "__pycache__", ".gradle", "build", ".cxx", "node_modules", ".venv"}
SELF = ("tools/ci/title_guard.py", "tools/ci/title_guard_allow.txt")   # the lists themselves
TEST_GLOBS = ("tools/*test_*.py", "tools/*conftest.py", "android/*/src/test/*", "android/*/src/androidTest/*")

# Ours, not a publisher's: the synthetic titles of tests, tools/conformance and tools/android/make_test_library.sh.
# « XB » (0x5842) is no publisher's code: every 5842xxxx is ours.
SYNTHETIC_PREFIXES = ("5842",)
SYNTHETIC_TITLE_IDS = {"58424104"}

# Lower case, words separated by one space. Distinctive names only: a common word (« Black ») would flag colour code;
# its title ID and addresses are checked instead.
GAME_NAMES = (
    "burnout", "fable", "kotor", "knights of the old republic", "halo", "half life", "panzer dragoon",
    "jet set radio", "ninja gaiden", "splinter cell", "project gotham", "morrowind", "conker", "mechassault",
    "crimson skies", "oddworld", "dead or alive", "psychonauts", "jade empire", "outrun",
)

# One game's addresses that are not (yet) in a profile: the Burnout 3 thunk table / .rdata base and Halo's thunk
# table, both once used as defaults.
EXTRA_GAME_ADDRESSES = {0x0036B7C0, 0x00253090}

C_EXT = {".c", ".h", ".cpp", ".cc", ".hpp", ".m", ".mm", ".java", ".kt", ".kts", ".gradle", ".js", ".glsl", ".frag",
         ".vert", ".inc"}
HASH_EXT = {".sh", ".cmake", ".txt", ".properties", ".env", ".gitignore", ".toml", ".cfg", ".ini", ".yml", ".yaml",
            ".pro", ".i386", ".amd64"}
HASH_NAMES = {"CMakeLists.txt", "Dockerfile", "Makefile", "fixinclude", "lowercase", "cl", "link"}
SKIP_EXT = {".md", ".wav", ".png", ".jpg", ".jpeg", ".webp", ".ico", ".xbe", ".bin", ".jar", ".so", ".a", ".o",
            ".zip", ".gz", ".ttf", ".otf", ".keystore", ".jks", ".iso", ".pdf", ".svg"}

HEX_LIT = re.compile(r"\b0[xX]([0-9A-Fa-f]+)")
# also inside identifiers: title_4d5300d1, k_4D5300D1_hooks (an underscore is not alphanumeric)
HEX8_WORD = re.compile(r"(?<![0-9A-Za-z])(?:0[xX])?([0-9A-Fa-f]{8})(?![0-9A-Za-z])")
WORD = re.compile(r"[A-Z]+(?![a-z])|[A-Z]?[a-z]+|[0-9]+")


# ---------------------------------------------------------------- lexers: yield (kind, line, text)

def lex_c(src):
    """C, C++, Objective-C, Java, Kotlin, Gradle, GLSL: // and /* */ comments, "..." and '...' literals."""
    i, n, line = 0, len(src), 1
    buf, buf_line = [], 1
    while i < n:
        c = src[i]
        if c == "/" and src.startswith("//", i):
            j = src.find("\n", i)
            j = n if j < 0 else j
            yield "comment", line, src[i:j]
            i = j
        elif c == "/" and src.startswith("/*", i):
            j = src.find("*/", i + 2)
            j = n if j < 0 else j + 2
            yield "comment", line, src[i:j]
            line += src.count("\n", i, j)
            i = j
        elif c in "\"'":
            if buf:
                yield "code", buf_line, "".join(buf)
                buf = []
            j = i + 1
            while j < n and src[j] != c and src[j] != "\n":
                j += 2 if src[j] == "\\" else 1
            yield "string", line, src[i + 1:min(j, n)]
            i = j + 1
        else:
            if c == "\n":
                if buf:
                    yield "code", buf_line, "".join(buf)
                    buf = []
                line += 1
            else:
                if not buf:
                    buf_line = line
                buf.append(c)
            i += 1
    if buf:
        yield "code", buf_line, "".join(buf)


def lex_hash(src):
    """Shell, CMake, Dockerfile, text lists: # starts a comment at line start or after a blank; the rest is code
    (quoted words are code too: the rules on code and strings are the same, so it makes no difference)."""
    for no, text in enumerate(src.splitlines(), 1):
        m = re.search(r"(^|\s)#", text)
        if m:
            code, comment = text[:m.start()], text[m.start():]
            if code.strip():
                yield "code", no, code
            yield "comment", no, comment
        elif text.strip():
            yield "code", no, text


def lex_xml(src):
    """<!-- --> comments; the rest is code, one piece per line."""
    pos = 0
    for m in list(re.finditer(r"<!--.*?-->", src, re.S)) + [None]:
        end = m.start() if m else len(src)
        line = src.count("\n", 0, pos) + 1
        for k, text in enumerate(src[pos:end].split("\n")):
            if text.strip():
                yield "code", line + k, text
        if m:
            yield "comment", src.count("\n", 0, m.start()) + 1, m.group(0)
            pos = m.end()


def lex_python(src):
    try:
        tree = ast.parse(src)
        doc_starts = {(node.value.lineno, node.value.col_offset) for node in ast.walk(tree)
                      if isinstance(node, ast.Expr) and isinstance(node.value, ast.Constant)
                      and isinstance(node.value.value, str)}
    except (SyntaxError, ValueError):
        doc_starts = set()
    try:
        for tok in tokenize.generate_tokens(io.StringIO(src).readline):
            if tok.type == tokenize.COMMENT:
                yield "comment", tok.start[0], tok.string
            elif tok.type == tokenize.STRING:
                yield ("docstring" if tok.start in doc_starts else "string"), tok.start[0], tok.string
            elif tok.type in (tokenize.NAME, tokenize.NUMBER, tokenize.OP):
                yield "code", tok.start[0], tok.string
            elif tok.type == getattr(tokenize, "FSTRING_MIDDLE", -1):
                yield "string", tok.start[0], tok.string
    except (tokenize.TokenError, IndentationError, SyntaxError):
        yield from lex_hash(src)


def lexer_for(path):
    base = os.path.basename(path)
    ext = os.path.splitext(base)[1].lower()
    if ext in SKIP_EXT:
        return None
    if ext == ".py":
        return lex_python
    if ext in C_EXT:
        return lex_c
    if ext == ".xml":
        return lex_xml
    if ext == ".json":
        return lambda s: (("string", no, t) for no, t in enumerate(s.splitlines(), 1))
    if ext in HASH_EXT or base in HASH_NAMES or base.endswith("Dockerfile") or ext == "":
        return lex_hash
    return lex_hash


# ---------------------------------------------------------------- rules

def is_retail_title_id(h):
    v = int(h, 16)
    b0, b1 = v >> 24, (v >> 16) & 0xFF
    return 0x41 <= b0 <= 0x5A and 0x41 <= b1 <= 0x5A and (v & 0xFFFF) < 0x1000 \
        and h.upper() not in SYNTHETIC_TITLE_IDS and not h.upper().startswith(SYNTHETIC_PREFIXES)


def words(text):
    return " ".join(w.lower() for w in WORD.findall(text))


NAME_RE = re.compile(r"(?:^| )(" + "|".join(re.escape(n) for n in GAME_NAMES) + r")(?= |$)")


def findings_in(kind, text, game_addresses):
    """(rule, token) pairs found in one piece of text of the given kind; token is normalised (hex upper case without
    0x, name lower case) so the allow-list can name it."""
    if kind == "comment":
        return
    seen = set()
    for m in HEX8_WORD.finditer(text):
        if is_retail_title_id(m.group(1)):
            seen.add(("title-id", m.group(1).upper()))
    for m in list(HEX_LIT.finditer(text)) + list(HEX8_WORD.finditer(text)):    # 0x0036B7C0, sub_0036B7C0
        if int(m.group(1), 16) in game_addresses:
            seen.add(("game-address", "%08X" % int(m.group(1), 16)))
    if kind in ("code", "string"):
        for m in NAME_RE.finditer(words(text)):
            seen.add(("game-name", m.group(1)))
    yield from sorted(seen)


SUB_NAME = re.compile(r"\bsub_([0-9A-Fa-f]{8})\b")


def profile_addresses(root):
    """The guest addresses a title's profile names: every 0x value in the image range in its .json files (params,
    seed functions) and every sub_XXXXXXXX its .c/.h code calls or defines, plus EXTRA_GAME_ADDRESSES. Not its C
    constants (register offsets such as 0x100410 are generic) nor its NOTES.md prose."""
    out = set(EXTRA_GAME_ADDRESSES)
    top = os.path.join(root, "profiles", "titles")
    for d, _, files in os.walk(top):
        for f in files:
            ext = os.path.splitext(f)[1].lower()
            if ext not in (".json", ".c", ".h"):
                continue
            try:
                with open(os.path.join(d, f), encoding="utf-8", errors="replace") as fh:
                    src = fh.read()
            except OSError:
                continue
            if ext == ".json":
                values = (int(m.group(1), 16) for m in HEX_LIT.finditer(src))
            else:
                values = (int(m.group(1), 16) for kind, _, text in lex_c(src) if kind == "code"
                          for m in SUB_NAME.finditer(text))
            out.update(v for v in values if 0x10000 < v < 0x80000000)
    return out


# ---------------------------------------------------------------- allow-list

def load_allow(path):
    """Lines `<glob>` (a whole file) or `<path> <token>` (one normalised token in one file), `# reason` after."""
    globs, tokens = [], set()
    if not os.path.exists(path):
        return globs, tokens
    with open(path, encoding="utf-8") as fh:
        for raw in fh:
            line = raw.split("#", 1)[0].strip()
            if not line:
                continue
            parts = line.split()
            if len(parts) == 1:
                globs.append(parts[0])
            else:
                tokens.add((parts[0], " ".join(parts[1:]).lower()))
    return globs, tokens


def iter_files(root):
    for top in SCANNED:
        for d, dirs, files in os.walk(os.path.join(root, top)):
            dirs[:] = sorted(x for x in dirs if x not in SKIP_DIRS)
            for f in sorted(files):
                full = os.path.join(d, f)
                yield os.path.relpath(full, root).replace(os.sep, "/"), full


def scan(root, allow_path=None):
    """Every violation as (path, line, rule, token, kind)."""
    allow_path = allow_path or os.path.join(root, "tools", "ci", "title_guard_allow.txt")
    globs, allowed_tokens = load_allow(allow_path)
    addresses = profile_addresses(root)
    out = []
    for rel, full in iter_files(root):
        if any(fnmatch.fnmatch(rel, g) for g in SELF + TEST_GLOBS + tuple(globs)):
            continue
        lex = lexer_for(rel)
        if lex is None:
            continue
        try:
            with open(full, "rb") as fh:
                data = fh.read()
        except OSError:
            continue
        if b"\0" in data[:8192]:
            continue
        src = data.decode("utf-8", errors="replace")
        for kind, line, text in lex(src):
            for rule, token in findings_in(kind, text, addresses):
                if (rel, token.lower()) not in allowed_tokens:
                    out.append((rel, line, rule, token, kind))
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(description="No retail title ID, game name or game address in the toolkit's code "
                                             "tokens (XBA-109).")
    ap.add_argument("--root", default=os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
    ap.add_argument("--allow", help="allow-list file (default tools/ci/title_guard_allow.txt under --root)")
    args = ap.parse_args(argv)
    found = scan(args.root, args.allow)
    for rel, line, rule, token, kind in found:
        print("%s:%d: %s %s (in %s)" % (rel, line, rule, token, kind))
    if found:
        print("\n%d per-title token(s) in the toolkit. Move per-title values to profiles/titles/<id>/, make the "
              "code generic, or, for a real exception, add `<path> <token>  # reason` to "
              "tools/ci/title_guard_allow.txt (rules: tools/ci/title_guard.py)." % len(found))
        return 1
    print("title guard: clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())
