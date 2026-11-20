"""
Mods: a player's own changes to a game they own, imported per title and turned on or off without touching the
game's files (epic XBA-285). The format and the store: docs/technical/mods.md.

    tools.mods.package   mod.json: the schema, loading and validation (ModError says what is wrong)
    tools.mods.store     the per-title store, the enabled list in load order, the mod-set hash
    tools.mods.display   the "display" hint (anamorphic frames) and what Prepare hands the runtime for it

Python 3 standard library only: this runs in the Android app's Prepare too (tools/prepare/toolkit_tree.json).
"""
