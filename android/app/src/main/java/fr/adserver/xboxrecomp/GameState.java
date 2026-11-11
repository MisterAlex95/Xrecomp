package fr.adserver.xboxrecomp;

/**
 * A library game's state, for its card's chip and Home's primary action: a pure function of what the launcher knows
 * about it (record, scan item, pack, Prepare status, the running game, the toolchain). No I/O here: the caller
 * gathers Facts, so the JVM tests cover every combination.
 *
 * Chip priority (first that applies): Running; Preparing; Importing; then, with the game's files present,
 * Prepare failed, Pack invalid, Ready, Needs preparing (without the on-device toolchain, Ready: the shell runs the
 * game its own way); without them, Not imported / Needs copying / No file access (a source is there), else
 * Source gone (an in-place folder that vanished), Data missing (a recomp.cfg folder that vanished), or GONE: neither
 * files nor source, the game leaves the grid (its pack and saves stay visible in Storage).
 */
final class GameState {
    private GameState() {}

    enum Chip {
        RUNNING, PREPARING, IMPORTING, PREPARE_FAILED, PACK_INVALID, READY, NEEDS_PREPARING, NOT_IMPORTED,
        NEEDS_COPYING, NO_FILE_ACCESS, SOURCE_GONE, DATA_MISSING, GONE
    }

    /** The active source in the library folder. */
    enum Source {
        /** Readable: an image to import, or a folder read in place. */
        PRESENT,
        /** A folder seen through the Storage Access Framework only: it must be copied to be played. */
        NEEDS_COPY,
        /** Listed, but the app lost its access (permission revoked, All files access off). */
        NO_ACCESS,
        /** Not in the library folder (or no library folder). */
        MISSING
    }

    static final class Facts {
        String titleId = "";
        /** The record's data_kind (Library.KIND_*), "" without a record. */
        String dataKind = "";
        /** The record's data_dir holds a default.xbe. */
        boolean hasData;
        Source source = Source.MISSING;
        /** The on-device Prepare is in this APK (Toolchain.available). */
        boolean toolchain;
        Pack.Check pack = Pack.Check.NO_PACK;
        /** False when the pack was built from another XBE than the active one (Pack.builtFrom). */
        boolean packMatches = true;
        /** The shell refused the pack (EXTRA_BAD_PACK) and the launcher dropped it. */
        boolean packDropped;
        /** status.json of this title, or null. */
        PrepareProtocol.Status prepare;
        /** status.json was updated recently (a live Prepare writes it every few seconds). */
        boolean prepareFresh;
        /** The ":prepare" process is alive. */
        boolean prepareProcess;
        /** The title ID of the game running now, or null. */
        String runningTid;
        /** The title ID being imported now, or null. */
        String importingTid;
    }

    static Chip chip(Facts f) {
        if (f.titleId.equals(f.runningTid))
            return Chip.RUNNING;
        boolean packValid = f.pack == Pack.Check.VALID && f.packMatches;
        LauncherActivity.Prep p = prep(f.toolchain, packValid, f.prepare, f.prepareFresh, f.prepareProcess);
        if (p == LauncherActivity.Prep.RUNNING)
            return Chip.PREPARING;
        if (f.titleId.equals(f.importingTid))
            return Chip.IMPORTING;
        if (f.hasData) {
            if (!f.toolchain)
                return Chip.READY;
            if (p == LauncherActivity.Prep.FAILED)
                return Chip.PREPARE_FAILED;
            boolean packPresent = f.pack != Pack.Check.NO_PACK && f.pack != Pack.Check.NO_TITLE_ID;
            if (f.packDropped || (packPresent && !packValid))
                return Chip.PACK_INVALID;
            return p == LauncherActivity.Prep.PLAYABLE ? Chip.READY : Chip.NEEDS_PREPARING;
        }
        switch (f.source) {
        case PRESENT:
            return Chip.NOT_IMPORTED;
        case NEEDS_COPY:
            return Chip.NEEDS_COPYING;
        case NO_ACCESS:
            return Chip.NO_FILE_ACCESS;
        default:
            break;
        }
        if (Library.KIND_IN_PLACE.equals(f.dataKind))
            return Chip.SOURCE_GONE;
        if (Library.KIND_CFG.equals(f.dataKind))
            return Chip.DATA_MISSING;
        return Chip.GONE;
    }

    /** The game can start now (the launcher may still have to end another running game first). */
    static boolean playable(Chip c) {
        return c == Chip.READY || c == Chip.RUNNING;
    }

    /** The game shows in the library grid. */
    static boolean inGrid(Chip c) {
        return c != Chip.GONE;
    }

    /**
     * The Prepare state of a title whose files are present (as LauncherActivity.prep): OFF without the toolchain;
     * RUNNING while a live Prepare reports; PLAYABLE with a valid pack built from the active XBE; else from the last
     * outcome.
     */
    static LauncherActivity.Prep prep(boolean toolchain, boolean packValid, PrepareProtocol.Status s,
                                      boolean statusFresh, boolean prepareProcess) {
        if (!toolchain)
            return LauncherActivity.Prep.OFF;
        if (s != null && s.running() && (statusFresh || prepareProcess))
            return LauncherActivity.Prep.RUNNING;
        if (packValid)
            return LauncherActivity.Prep.PLAYABLE;
        if (s == null)
            return LauncherActivity.Prep.NEEDED;
        switch (s.state) {
        case PrepareProtocol.FAILED:
            return LauncherActivity.Prep.FAILED;
        case PrepareProtocol.CANCELLED:
            return LauncherActivity.Prep.CANCELLED;
        case PrepareProtocol.RUNNING:
            return LauncherActivity.Prep.INTERRUPTED;
        default:
            return LauncherActivity.Prep.NEEDED;
        }
    }
}
