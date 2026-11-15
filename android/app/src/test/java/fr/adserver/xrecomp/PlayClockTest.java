package fr.adserver.xrecomp;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;

import org.junit.Rule;
import org.junit.Test;
import org.junit.rules.TemporaryFolder;

import java.io.File;

/**
 * Play time as the game process counts it (XBA-206, plain JUnit): on screen only (the background does not count), a
 * crash loses no more than the last beat, a game process the system restarts goes on with the same session, the
 * launcher takes the game's count (the wall clock for a session the game never counted), at most SESSION_MAX.
 */
public class PlayClockTest {
    @Rule
    public TemporaryFolder tmp = new TemporaryFolder();

    private static final long MIN = 60_000, T0 = 1_700_000_000_000L;

    @Test
    public void countsOnlyTheTimeOnScreen() {
        PlayClock c = PlayClock.begin(null, "58420001", T0, T0 + 2000);
        c.resume(T0 + 3000);
        c.pause(T0 + 3000 + 10 * MIN);   // Back to launcher: the game waits in the background
        c.resume(T0 + 60 * MIN);         // brought back 50 min later
        c.pause(T0 + 65 * MIN);          // Quit
        assertEquals(15 * MIN, c.total());
        assertEquals(15 * MIN, PlayClock.sessionLength(c, T0, T0 + 66 * MIN));
        c.resume(T0 + 70 * MIN);
        c.resume(T0 + 71 * MIN);         // a second onStart while on screen changes nothing
        c.pause(T0 + 72 * MIN);
        assertEquals(17 * MIN, c.total());
    }

    @Test
    public void aCrashCountsUpToTheLastBeat() throws Exception {
        File f = PlayClock.file(tmp.newFolder("files"));
        PlayClock c = PlayClock.begin(null, "gba-AXVE", T0, T0);
        c.resume(T0);
        assertTrue(c.write(f));
        for (int i = 1; i <= 7; i++) {
            c.beat(T0 + i * PlayClock.BEAT_MS);
            c.write(f);
        }
        // The process dies 40 s after its last beat: nothing more is written.
        PlayClock read = PlayClock.read(f);
        assertNotNull(read);
        assertEquals("gba-AXVE", read.tid);
        assertEquals(7 * PlayClock.BEAT_MS, read.total());
        assertEquals("the launcher, an hour later, counts 7 min, not 67",
                     7 * PlayClock.BEAT_MS, PlayClock.sessionLength(read, T0, T0 + 67 * MIN));
    }

    @Test
    public void aRestartedGameProcessGoesOnWithItsSession() {
        PlayClock c = PlayClock.begin(null, "58420001", T0, T0);
        c.resume(T0);
        c.beat(T0 + 5 * MIN);   // killed in the background while on screen? its last beat closes the stretch
        PlayClock again = PlayClock.begin(c, "58420001", T0, T0 + 30 * MIN);
        assertEquals(5 * MIN, again.total());
        again.resume(T0 + 30 * MIN);
        again.pause(T0 + 32 * MIN);
        assertEquals(7 * MIN, again.total());
        // Another launcher session: from zero.
        PlayClock next = PlayClock.begin(again, "58420001", T0 + 40 * MIN, T0 + 40 * MIN);
        assertEquals(0, next.total());
        assertEquals(T0 + 40 * MIN, next.created);
    }

    @Test
    public void theLauncherFallsBackToTheWallClock() {
        assertEquals("no clock: since launch", 20 * MIN, PlayClock.sessionLength(null, T0, T0 + 20 * MIN));
        PlayClock old = PlayClock.begin(null, "58420001", T0 - 99 * MIN, T0 - 99 * MIN);
        old.resume(T0 - 99 * MIN);
        old.pause(T0 - 90 * MIN);
        assertFalse("an older session's clock", old.counts(T0));
        assertEquals(20 * MIN, PlayClock.sessionLength(old, T0, T0 + 20 * MIN));
        // A game that read a stale session start but began after this one: its count is this session's.
        PlayClock stale = PlayClock.begin(null, "58420001", T0 - 99 * MIN, T0 + 1000);
        assertTrue(stale.counts(T0));
        assertEquals(0, PlayClock.sessionLength(null, 0, T0));
        assertEquals("at most SESSION_MAX", GameStore.SESSION_MAX,
                     PlayClock.sessionLength(null, T0, T0 + GameStore.SESSION_MAX + 5 * MIN));
        PlayClock huge = PlayClock.begin(null, "58420001", T0, T0);
        huge.resume(T0);
        huge.pause(T0 + 9 * 3600_000L);
        assertEquals(GameStore.SESSION_MAX, PlayClock.sessionLength(huge, T0, T0 + 9 * 3600_000L));
    }

    @Test
    public void unreadableFiles() throws Exception {
        File files = tmp.newFolder("files");
        assertNull(PlayClock.read(PlayClock.file(files)));
        TestSupport.write(PlayClock.file(files), "created=x\n".getBytes("UTF-8"));
        assertNull(PlayClock.read(PlayClock.file(files)));
        TestSupport.write(PlayClock.file(files), "created=5\nfg_ms=-9\n".getBytes("UTF-8"));
        assertEquals(0, PlayClock.read(PlayClock.file(files)).total());
    }

    @Test
    public void sessionAddAndEndAddTheLength() {
        Library.Games g = new Library.Games();
        Library.Record r = new Library.Record();
        r.titleId = "gba-AXVE";
        g.put(r);
        Library.sessionStart(g, "gba-AXVE", T0);
        assertEquals(T0, r.lastPlayed);
        Library.sessionAdd(g, "gba-AXVE", T0, 12 * MIN);
        assertEquals(12 * MIN, r.playMs);
        Library.sessionEnd(g, "gba-AXVE", T0, T0 + 3 * MIN);
        assertEquals(15 * MIN, r.playMs);
        Library.sessionAdd(g, "gba-AXVE", 0, 12 * MIN);   // no session: nothing
        assertEquals(15 * MIN, r.playMs);
    }
}
