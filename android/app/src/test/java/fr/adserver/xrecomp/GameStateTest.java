package fr.adserver.xrecomp;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertTrue;

import org.junit.Test;

/** The library chip of a game, by priority (plain JUnit: GameState does no I/O). */
public class GameStateTest {
    private static final String TID = "4D5300D1";

    private static GameState.Facts ready() {
        GameState.Facts f = new GameState.Facts();
        f.titleId = TID;
        f.dataKind = Library.KIND_IMPORTED;
        f.hasData = true;
        f.source = GameState.Source.PRESENT;
        f.toolchain = true;
        f.pack = Pack.Check.VALID;
        return f;
    }

    private static PrepareProtocol.Status status(String state) {
        PrepareProtocol.Status s = new PrepareProtocol.Status();
        s.state = state;
        s.titleId = TID;
        return s;
    }

    @Test
    public void readyAndRunning() {
        GameState.Facts f = ready();
        assertEquals(GameState.Chip.READY, GameState.chip(f));
        f.runningTid = "OTHER000";
        assertEquals(GameState.Chip.READY, GameState.chip(f));
        f.runningTid = TID;
        assertEquals(GameState.Chip.RUNNING, GameState.chip(f));
        assertTrue(GameState.playable(GameState.Chip.RUNNING));
        assertTrue(GameState.playable(GameState.Chip.READY));
        assertFalse(GameState.playable(GameState.Chip.NEEDS_PREPARING));
    }

    /**
     * XBA-157: a GBA game plays its ROM as it is: Ready while it is in a library folder (whatever the toolchain or
     * packs), Running while it runs, No file access when its folder cannot be read, gone without it. PS1 and N64 stay
     * « Not playable yet ».
     */
    @Test
    public void otherConsoles() {
        GameState.Facts f = new GameState.Facts();
        f.titleId = "gba-AXVE-1A2B3C4D";
        f.console = Console.GBA;
        f.source = GameState.Source.PRESENT;
        f.toolchain = true;
        f.pack = Pack.Check.NO_PACK;
        assertEquals(GameState.Chip.READY, GameState.chip(f));
        f.toolchain = false;
        assertEquals(GameState.Chip.READY, GameState.chip(f));
        // XBA-158: its Prepare shows on the card while it runs or waits; failed or done, it still plays.
        f.toolchain = true;
        PrepareProtocol.Status s = new PrepareProtocol.Status();
        s.state = PrepareProtocol.RUNNING;
        f.prepare = s;
        f.prepareFresh = true;
        assertEquals(GameState.Chip.PREPARING, GameState.chip(f));
        s.state = PrepareProtocol.QUEUED;
        f.prepareProcess = true;
        assertEquals(GameState.Chip.QUEUED, GameState.chip(f));
        s.state = PrepareProtocol.FAILED;
        assertEquals(GameState.Chip.READY, GameState.chip(f));
        f.pack = Pack.Check.VALID;
        assertEquals(GameState.Chip.READY, GameState.chip(f));
        f.prepare = null;
        f.prepareProcess = false;
        f.runningTid = "gba-AXVE-1A2B3C4D";
        assertEquals(GameState.Chip.RUNNING, GameState.chip(f));
        f.runningTid = TID;
        f.source = GameState.Source.NO_ACCESS;
        assertEquals(GameState.Chip.NO_FILE_ACCESS, GameState.chip(f));
        f.source = GameState.Source.MISSING;
        assertEquals(GameState.Chip.GONE, GameState.chip(f));
        assertTrue(Console.GBA.playable() && Console.GBA.directLaunch());
        assertTrue(Console.XBOX.playable());
        assertFalse(Console.XBOX.directLaunch());
        for (Console c : new Console[] { Console.PS1, Console.N64 }) {
            f.console = c;
            f.titleId = c.id + "-KEY";
            f.source = GameState.Source.PRESENT;
            f.runningTid = f.titleId;
            assertEquals(GameState.Chip.NOT_PLAYABLE_YET, GameState.chip(f));
            assertFalse(c.playable() || c.directLaunch());
        }
    }

    @Test
    public void runningBeatsEverything() {
        GameState.Facts f = ready();
        f.runningTid = TID;
        f.prepare = status(PrepareProtocol.RUNNING);
        f.prepareFresh = true;
        f.importingTid = TID;
        assertEquals(GameState.Chip.RUNNING, GameState.chip(f));
    }

    @Test
    public void preparing() {
        GameState.Facts f = ready();
        f.pack = Pack.Check.NO_PACK;
        f.prepare = status(PrepareProtocol.RUNNING);
        f.prepareFresh = true;
        f.importingTid = TID;
        assertEquals(GameState.Chip.PREPARING, GameState.chip(f));
        f.prepareFresh = false;
        f.prepareProcess = true;
        assertEquals(GameState.Chip.PREPARING, GameState.chip(f));
        // « running » left by a dead process: interrupted, which offers Prepare (Resume) again.
        f.prepareProcess = false;
        f.importingTid = null;
        assertEquals(GameState.Chip.NEEDS_PREPARING, GameState.chip(f));
        assertEquals(LauncherActivity.Prep.INTERRUPTED, GameState.prep(true, false, f.prepare, false, false));
    }

    @Test
    public void importing() {
        GameState.Facts f = ready();
        f.hasData = false;
        f.importingTid = TID;
        assertEquals(GameState.Chip.IMPORTING, GameState.chip(f));
    }

    @Test
    public void packStates() {
        GameState.Facts f = ready();
        f.pack = Pack.Check.NO_PACK;
        assertEquals(GameState.Chip.NEEDS_PREPARING, GameState.chip(f));
        f.prepare = status(PrepareProtocol.FAILED);
        assertEquals(GameState.Chip.PREPARE_FAILED, GameState.chip(f));
        f.prepare = status(PrepareProtocol.CANCELLED);
        assertEquals(GameState.Chip.NEEDS_PREPARING, GameState.chip(f));
        f.prepare = null;
        f.pack = Pack.Check.LIB_SIZE;
        assertEquals(GameState.Chip.PACK_INVALID, GameState.chip(f));
        f.pack = Pack.Check.WRONG_SHELL_ABI;
        assertEquals(GameState.Chip.PACK_INVALID, GameState.chip(f));
        f.pack = Pack.Check.VALID;
        f.packMatches = false;   // built from another version of the game
        assertEquals(GameState.Chip.PACK_INVALID, GameState.chip(f));
        f.packMatches = true;
        f.packDropped = true;    // the shell refused it; dropped
        f.pack = Pack.Check.NO_PACK;
        assertEquals(GameState.Chip.PACK_INVALID, GameState.chip(f));
        // A valid pack beats an old failed status (it was rebuilt since).
        f.packDropped = false;
        f.pack = Pack.Check.VALID;
        f.prepare = status(PrepareProtocol.FAILED);
        assertEquals(GameState.Chip.READY, GameState.chip(f));
    }

    @Test
    public void noToolchainSkipsPackStates() {
        GameState.Facts f = ready();
        f.toolchain = false;
        f.pack = Pack.Check.NO_PACK;
        f.prepare = status(PrepareProtocol.FAILED);
        assertEquals(GameState.Chip.READY, GameState.chip(f));
        assertEquals(LauncherActivity.Prep.OFF, GameState.prep(false, false, f.prepare, true, true));
    }

    @Test
    public void withoutData() {
        GameState.Facts f = ready();
        f.hasData = false;
        f.pack = Pack.Check.NO_PACK;
        assertEquals(GameState.Chip.NOT_IMPORTED, GameState.chip(f));
        f.pack = Pack.Check.VALID;   // prepared from the XBE cache before import: still to import
        assertEquals(GameState.Chip.NOT_IMPORTED, GameState.chip(f));
        f.source = GameState.Source.NEEDS_COPY;
        assertEquals(GameState.Chip.NEEDS_COPYING, GameState.chip(f));
        f.source = GameState.Source.NO_ACCESS;
        assertEquals(GameState.Chip.NO_FILE_ACCESS, GameState.chip(f));
        f.source = GameState.Source.MISSING;
        assertEquals(GameState.Chip.GONE, GameState.chip(f));
        assertFalse(GameState.inGrid(GameState.Chip.GONE));
        f.dataKind = Library.KIND_IN_PLACE;
        assertEquals(GameState.Chip.SOURCE_GONE, GameState.chip(f));
        f.dataKind = Library.KIND_CFG;
        assertEquals(GameState.Chip.DATA_MISSING, GameState.chip(f));
        assertTrue(GameState.inGrid(GameState.Chip.DATA_MISSING));
        f.dataKind = "";
        assertEquals(GameState.Chip.GONE, GameState.chip(f));
    }

    @Test
    public void sourceGoneButDataPresentIsPlayable() {
        GameState.Facts f = ready();
        f.source = GameState.Source.MISSING;
        assertEquals(GameState.Chip.READY, GameState.chip(f));
    }

    @Test
    public void prepMatchesTheLauncher() {
        assertEquals(LauncherActivity.Prep.NEEDED, GameState.prep(true, false, null, false, false));
        assertEquals(LauncherActivity.Prep.PLAYABLE, GameState.prep(true, true, null, false, false));
        assertEquals(LauncherActivity.Prep.NEEDED, GameState.prep(true, false, status(PrepareProtocol.DONE), false,
                                                                  false));
        assertEquals(LauncherActivity.Prep.FAILED, GameState.prep(true, false, status(PrepareProtocol.FAILED), false,
                                                                  false));
        assertEquals(LauncherActivity.Prep.CANCELLED,
                     GameState.prep(true, false, status(PrepareProtocol.CANCELLED), false, false));
        assertEquals(LauncherActivity.Prep.RUNNING, GameState.prep(true, true, status(PrepareProtocol.RUNNING), true,
                                                                   false));
    }

    @Test
    public void queued() {
        GameState.Facts f = ready();
        f.pack = Pack.Check.NO_PACK;
        f.prepare = status(PrepareProtocol.QUEUED);
        f.prepareProcess = true;
        f.importingTid = TID;
        assertEquals(GameState.Chip.QUEUED, GameState.chip(f));
        assertEquals(LauncherActivity.Prep.QUEUED, GameState.prep(true, false, f.prepare, false, true));
        // « queued » with no :prepare process: the queue died with it, the Prepare is offered again.
        f.prepareProcess = false;
        f.importingTid = null;
        assertEquals(GameState.Chip.NEEDS_PREPARING, GameState.chip(f));
        assertEquals(LauncherActivity.Prep.INTERRUPTED, GameState.prep(true, false, f.prepare, true, false));
        // Queued before the import (« Prepare now » from the XBE cache): queued too.
        f.hasData = false;
        f.prepareProcess = true;
        assertEquals(GameState.Chip.QUEUED, GameState.chip(f));
        // Another title preparing does not make this one preparing.
        f.prepare = null;
        assertEquals(GameState.Chip.NOT_IMPORTED, GameState.chip(f));
    }
}
