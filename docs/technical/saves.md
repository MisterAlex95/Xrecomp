# Saves: one layout for every title (XBA-72)

The XDK's save APIs (`XCreateSaveGame`, `XFindFirstSaveGame`, `XDeleteSaveGame`, the title's `T:` data) are XAPI
code linked into the title: they are recompiled with it and reach the runtime as ordinary kernel file calls
(`NtCreateFile`, `NtQueryDirectoryFile`, ...) on `U:\` and `T:\`, which XAPI links to
`\Device\Harddisk0\Partition1\UDATA\<title id>` and `...\TDATA\<title id>`. Nothing is per title in the runtime: the
path table (`src/kernel/kernel_path.c`) maps those two folders once, for every title.

| Xbox | Host | Notes |
|---|---|---|
| `\Device\Harddisk0\Partition1\UDATA\<TID>\...` (`U:\`) | `<save dir>/UDATA/<tid>/...` (title ID in lower case) | The player's saves, one folder per save as XAPI makes them (`SaveMeta.xbx`, `Data.ssx`, ...). |
| `\Device\Harddisk0\Partition1\TDATA\<TID>\...` (`T:\`) | `<save dir>/TDATA/<tid>/...` | The title's own persistent data (settings, rosters). |
| `\Device\Harddisk0\Partition1\CACHE` | `<save dir>/TitleCache` | Scratch, not a save. |
| `Z:`, `\??\Z:`, cache partitions 3-5 | `<save dir>/Cache/<tid>/...` (title ID in lower case); the bare devices are `Partition3-5.img` | Scratch, not a save. Per title, as on a console, where XAPI gives each title its own cache partition. |

A name opened relative to a directory handle (XAPI opens the save folder, then `Data.ssx` in it) is joined to that
handle's host path (`posix_obj_path`, `kernel_file.c`). `tests/kernel_path` and `tests/kernel_fatx_overwrite` cover
the mapping and the overwrite of an existing save.

## Where the save dir is

| Host | Save dir | Kept |
|---|---|---|
| Linux, macOS | `$XDG_DATA_HOME/xboxrecomp` (`~/.local/share/xboxrecomp`) | Always: no prepare or rebuild writes there. |
| Windows | `%LOCALAPPDATA%\xboxrecomp` | Same. |
| Android | `<files dir>/save` (internal storage, private to the app) | Across Prepares (they write `work/<TID>` and `packs/<TID>`), app updates and game switches. |
| Any, scripted runs | `RECOMP_SAVE_DIR=<dir>` | Moves `UDATA`, `TDATA`, `TitleCache` and `Cache/<tid>` (Z:); a fresh `<dir>` starts with an empty Z:. The partition images stay. |

Before Z: was per title it was `<save dir>/Cache` itself, shared by every title (KOTOR's `currentgame` beside
Fable's `data` and `PageFile.bin`). That folder's old contents are left where they are: which title wrote what
cannot be told on a desktop, and a title rebuilds its utility drive (Fable unpacks its data again once). They can be
deleted by hand. On Android the live set has a known owner (`cache_owner`), so the app moves them into
`Cache/<owner>` at that title's next launch (`SaveCaches.migrateLegacyCache`). Until an XBE is mapped (no title ID) Z: is still the bare `Cache`.

On Android the folders that are not per title (`Cache`, `TitleCache`, the cache partitions) move aside when another
title is launched (`SaveCaches.java`, « Game library » in [app-shell.md](../android/app-shell.md)); `UDATA` and
`TDATA` never move: they are already per title. Manage › Storage lists and deletes a title's saves on their own
(`Storage.java`).

## Memory units

None is attached. The emulated USB host (`src/usb/ohci.c`) has gamepads on its root hub ports and nothing else, so
XAPI's memory unit driver finds no unit: titles offer only the hard disk in their save screens, and every
save goes to `UDATA`. A title that insists on a memory unit (none known) would be the case for a later
virtual MU on the same save dir.
