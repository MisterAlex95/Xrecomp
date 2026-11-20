# Mods

A mod is a player's own change to a game they own: imported per title, turned on or off, and played without
touching the game's own files (epic XBA-285). Nothing here ships a mod: the toolkit knows the format, the player
brings the mods. The code: `tools/mods/` (Python, used by Prepare on the desktop and in the Android app) and
`src/kernel/xbox_state.c` (the mod set in a save state's identity).

## A mod package

One folder, named after the mod's id:

    widescreen/
      mod.json
      patches/default.bps

### mod.json

```json
{
  "format": 1,
  "id": "widescreen",
  "name": "Widescreen",
  "version": "1.0.0",
  "console": "xbox",
  "title_id": "58420001",
  "binary": {"sha256": "<SHA-256 of the game binary it was made for>", "name": "PAL, 1.00"},
  "patches": ["patches/default.bps"],
  "display": {"frames": "anamorphic-16:9"},
  "description": "...",
  "authors": ["..."],
  "url": "..."
}
```

| Field | | |
|---|---|---|
| `format` | required | the format version, 1. A mod with a higher one is refused with « made for a newer XRecomp ». |
| `id` | required | lower-case letters, digits, `.`, `_`, `-` (64 at most); the folder's name. |
| `name`, `version` | required | a line of text; a version of letters, digits, `.`, `+`, `_`, `-` (32 at most). |
| `console` | required | `xbox`, `x360`, `gba`, `ps1`, `n64` or `gc`. |
| `title_id` | required | the game: an Xbox / Xbox 360 title ID (8 hex digits), another console's game key. |
| `binary` | with `patches` | `sha256` of the game binary (the XBE, XEX, DOL or ROM) the mod was made for, and its `name` (version, region) for the messages. |
| `patches` | optional | 1 to 16 BPS (`.bps`) or IPS (`.ips`) files, applied in this order before Prepare (« Patches » below). |
| `display` | optional | how to present what the mod draws (« display » below). |
| `description`, `authors`, `url` | optional | text for the player. |

Any other key is an error (a typo would otherwise be silently ignored), except keys starting with `_` (comments).

### Checks

`tools.mods.package.load` refuses a mod, with a message naming the mod and the field, when:

- mod.json is not JSON, not an object, larger than 64 KB, or breaks the table above (`BAD_MOD`, `NEWER_FORMAT`);
- it is for another console or title (`WRONG_TITLE`);
- it names a `binary` and the game's is another one (`WRONG_BINARY`: « made for another version of this game: PAL,
  1.00 (SHA-256 ab12...), yours is cd34... »);
- a path it names is absolute, uses `\`, has a `..` or empty part, or resolves through a symbolic link outside the
  mod's folder; or any symbolic link in the folder points outside it (`UNSAFE_PATH`);
- a file it names is missing (`MISSING`).

## Store

Per title, under the mods root:

    <mods root>/<console>/<title id>/
      enabled.json        {"format": 1, "enabled": ["first", "second"]}
      <mod id>/mod.json

The mods root is `--mods DIR` (Prepare, `tools.mods`) or `$XBA_MODS_DIR`, else `<user data>/mods`: `$XRECOMP_HOME`,
or `~/.local/share/xrecomp` (Linux), `~/Library/Application Support/xboxrecomp` (macOS), `%APPDATA%\xrecomp`
(Windows) — the root `xrecomp` and the macOS launcher already share. The Android app's is `files/mods`.

`enabled.json` lists the mods that are on, in load order: the first is applied first and each later one on top of
it (its patch is applied to the earlier ones' result; where two change the same thing, the later wins). A mod that
is on but missing or not valid for the game stops Prepare with the reason: the player asked for it, so it is not
skipped in silence.

From the command line:

```bash
python -m tools.mods check   path/to/widescreen --title 58420001 --binary default.xbe
python -m tools.mods install path/to/widescreen
python -m tools.mods enable  --title 58420001 widescreen [--at 0]
python -m tools.mods list    --title 58420001
python -m tools.mods disable --title 58420001 widescreen
python -m tools.mods hash    --title 58420001
```

## The mod set

The mod-set hash names exactly which mods are on, at which version and in which order: SHA-256 over
`xrecomp-modset-1\n` then, per enabled mod in load order, `<id>\t<version>\t<SHA-256 of its mod.json bytes>\n`;
empty for no mod (`tools.mods.store.mod_set_hash`). The same mods give the same hash on every host.

Prepare forces it on the runtime as `RECOMP_MOD_SET` (MOD-3, XBA-288) (the pack's options, like a title's `env_force`), and an Xbox
save state's identity carries its first 8 hex digits as a 32-bit word (`XboxStateIdentity.mod_set`, header bytes
116-119; 0 is no mod, a hash starting with 8 zeros counts as 1: `xbox_StateModSet` and `store.state_mod_set`, one
test vector in `tests/xbox_state` and `tools/mods/test_mods.py`). A state made under another mod set is refused
(`XBOX_STATE_EVERSION`) with the reason: « made without mods », « made with mods, none on now » or « made with
another set of mods ». States made before mods existed hold 0 there and still load without mods; the state format
version did not change.
