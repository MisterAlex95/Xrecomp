# Mods

A mod is a player's own change to a game they own: imported per title, turned on or off, and played without
touching the game's own files (epic XBA-285). Nothing here ships a mod: the toolkit knows the format, the player
brings the mods. One game's enhancement that cannot be derived is a mod, never toolkit code (XBA-236: the last
title layer, Fable's 16:9 scenes and front-end rate, became one). The code: `tools/mods/` (Python, used by Prepare on
the desktop and in the Android app), `src/kernel/xbox_mods.c` (what the mods ask of the Xbox runtime) and
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

Prepare forces it on the runtime as `RECOMP_MOD_SET` (the pack's options, like a title's `env_force`), and an Xbox
save state's identity carries its first 8 hex digits as a 32-bit word (`XboxStateIdentity.mod_set`, header bytes
116-119; 0 is no mod, a hash starting with 8 zeros counts as 1: `xbox_StateModSet` and `store.state_mod_set`, one
test vector in `tests/xbox_state` and `tools/mods/test_mods.py`). A state made under another mod set is refused
(`XBOX_STATE_EVERSION`) with the reason: « made without mods », « made with mods, none on now » or « made with
another set of mods ». States made before mods existed hold 0 there and still load without mods; the state format
version did not change.

## Patches

`patches` are applied by Prepare, in load order, to a copy of the game's binary, never to the original
(`tools/mods/patch.py`); the copy is what the analysis and the lift read, so the lifted code is the patched code. A
static recompiler cannot apply a code patch at run time (a code write has no effect, `src/kernel/recomp_smc.h`):
patching before Prepare is the only way, and it works the same with or without an interpreter.

- **BPS** (`tools/mods/bps.py`): our implementation of the format (source, target and patch CRC32 checked). A patch
  made for another binary is refused before anything is written: « mod X: patch p does not fit: it was made for
  another version of the game (PAL, 1.00) », or, when an earlier mod changed the binary first, « it was made for the
  unmodified game, and Y changed it first: put this mod first or turn the other off ».
- **IPS** (`tools/mods/ips.py`): records, runs and the truncation extension. IPS carries no checksum: the mod's
  `binary` SHA-256 is checked first (mandatory with `patches`).
- **xdelta**: not supported yet (refused by name).
- `bps.make` / `ips.make` make a patch from two files, for mod makers and tests. They are simple (equal runs and
  literal bytes), good for the few changed instructions and values of a binary patch, not general delta encoders.

### In Prepare (Xbox)

1. Step 1 reads the title's enabled mods (`tools.mods.patch.plan`; a problem stops Prepare with code `BAD_MOD` and the
   message). With patches, the patched bytes go to `<work>/mods/default.xbe` (the plain copy, `<work>/xbe/`, and the
   game's own file are left as they are), and every later step reads that copy.
2. The stamp's `"xbe"` is the lifted copy's SHA-256: another patch set, or none, analyses and lifts again, and turning
   the mods off brings the unpatched project and pack back. `"xbe_source"` keeps the game's own XBE and `"mods"` the
   mod-set hash: `--check` says `mods` when the store changed since. pack.json's `xbe_sha256` stays the game's own
   (what the app compares with the game folder); `"mods"` says what went in (`set`, `ids`, `binary_sha256`).
3. The options the mods force go with the layers' (`env_force`): `RECOMP_MOD_SET`, the `display` hint's, and
   `RECOMP_MOD_XBE`, the patched copy's path, so the runtime loads its headers and sections (a patch's data changes;
   its code changes came with the lift). A pack moved to another device without its work folder loads the disc's XBE
   and says so (`[MODS] ... cannot be opened`): its data changes are then missing.
4. A disc's other images (XBA-82: `--image-name` sub-prepares, the Android pack's `images`) get no mod: a mod is
   made for the title's `default.xbe`. They share the pack's options on Android, so Prepare also forces
   `RECOMP_MOD_FOR`, `<size>-<CRC32>` of the `default.xbe` the mods were made for, and the runtime applies nothing
   (no patched copy, no frames hint, no writes) to an XBE that does not match (`[MODS] this XBE is not the one the
   mods were made for`).
5. The discovery file (`discovered.jsonl`) is shared with the unpatched game's runs; its entries go through the
   analysis' gates against the binary being lifted, so an address that is not code in the patched copy is refused.

The other consoles' Prepares do not read the store yet (XBA-288): their runtimes read the ROM or disc at run time
too, so they need the same « load the patched copy » step as `RECOMP_MOD_XBE`.

## display

```json
"display": {
  "frames": "anamorphic-16:9",
  "wide_writes": [{"va": "0x00010FF0", "f32": [16.0, 9.0]}]
}
```

`frames: anamorphic-16:9` says the mod makes the game draw its 3D scenes squeezed into the console's 4:3 frame for a
16:9 screen. The runtime then presents those frames stretched to 16:9 and keeps 2D screens (menus, movies) at 4:3
(`xbox_FramebufferWindowSetWide`, the presenter's triangle count, `src/video/aspect_policy.h`), following
`RECOMP_ASPECT`: 16:9 always, 4:3 never, auto on Android (a 16:9 handheld) and not on a desktop (the window keeps the
console's shape).

A patch is fixed at Prepare, the aspect setting is not: a mod whose picture must follow the setting makes its switch
data. `wide_writes` are guest writes the runtime makes once the XBE is loaded (each start, so after a title reboot
too), only when it presents the frames 16:9; each is `va` and one of `f32`, `u32` (little-endian) or `hex`, inside
the loaded image, 8 writes and 64 bytes at most. Example: a camera that reads a display size through a pointer, zero
for « the viewport ». The patch points that load at 8 zero bytes the game never uses; `wide_writes` puts 16.0 / 9.0
there when the picture is presented 16:9, and at 4:3 the zeros keep the console's picture. A change that cannot be
made data (an immediate in an instruction) is static: it applies with either setting.

`tools/mods/display.py` checks the hint and turns it into `RECOMP_MOD_FRAMES` and `RECOMP_MOD_WIDE_WRITES`
(`<va hex>=<bytes hex>;...`); `src/kernel/xbox_mods.c` applies them (`[MODS]` lines). Xbox only for now.

## The app and the launchers

- **Android**: the store is `<external files>/mods` (`/sdcard/Android/data/<package>/files/mods/`, `PrepareService`
  passes `XBA_MODS_DIR`), where a computer can copy a mod over USB or adb until the app has its Mods screen (MOD-4).
  Settings › General › Enhancements (per game) turns the title's mods on or off for the next preparation
  (`XBA_NO_TITLE_LAYER=1` when off); pack.json `"title_layer"` says how the pack was made, and Home offers Prepare
  again when the switch differs.
- **macOS launcher and `xrecomp`**: the store is `<user data>/mods` (the same root as theirs); Manage › More ›
  Enhancements and `xrecomp prepare --no-title-layer` are the same switch.

## Known limits

- A patch fits one exact binary (version, region): the message names the version the mod was made for.
- Two BPS patches made for the unmodified game cannot both apply (the second sees the first's result); IPS patches
  touching different bytes can.
- A game may check its own data or cache data on Z: (a mod's change may need the cache cleared).
- Not yet: the file overlay of `files/` (MOD-2), the app's Mods screen (MOD-4), textures, the other consoles.
