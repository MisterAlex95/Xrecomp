package fr.adserver.xrecomp;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertTrue;
import static org.junit.Assert.fail;

import android.content.Context;
import android.content.pm.ProviderInfo;
import android.net.Uri;
import android.os.ParcelFileDescriptor;

import org.junit.Before;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.Robolectric;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.RuntimeEnvironment;
import org.robolectric.annotation.Config;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileNotFoundException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;

/**
 * LogProvider (« Share log »): the Prepare log of every console that prepares is served, GameCube discs included
 * (AND-2: « gc-GALE01-0 » was refused, the mail app got an empty attachment); a key that leaves files/prepare is not.
 */
@RunWith(RobolectricTestRunner.class)
@Config(sdk = 34)
public class LogProviderTest {
    private Context c;
    private LogProvider provider;

    @Before
    public void setUp() {
        c = RuntimeEnvironment.getApplication();
        ProviderInfo info = new ProviderInfo();
        info.authority = LogProvider.authority(c);
        provider = Robolectric.buildContentProvider(LogProvider.class).create(info).get();
    }

    private void writeLog(String tid, String text) throws Exception {
        File d = new File(new File(c.getFilesDir(), "prepare"), tid);
        assertTrue(d.mkdirs() || d.isDirectory());
        Files.write(new File(d, "prepare.log").toPath(), text.getBytes(StandardCharsets.UTF_8));
    }

    private String read(Uri uri) throws Exception {
        try (ParcelFileDescriptor pfd = provider.openFile(uri, "r");
             FileInputStream in = new FileInputStream(pfd.getFileDescriptor())) {
            return new String(in.readAllBytes(), StandardCharsets.UTF_8);
        }
    }

    @Test
    public void servesEveryPreparedConsolesLog() throws Exception {
        for (String tid : new String[] {"4D530004", "gba-AXVE-1A2B3C4D", "ps1-SLUS-00594", "n64-NSME-0123ABCD",
                                        "gc-DOL-89ABCDEF", "gc-GALE01-0", "gc-GALE01-0-D2"}) {
            writeLog(tid, "log of " + tid);
            assertEquals(tid, "log of " + tid, read(LogProvider.uriFor(c, tid)));
            assertNotNull(tid, provider.query(LogProvider.uriFor(c, tid), null, null, null, null));
        }
    }

    @Test
    public void refusesKeysOutsideThePrepareFolder() throws Exception {
        File outside = new File(c.getFilesDir(), "x");   // what « gc-../x » would reach
        assertTrue(outside.mkdirs() || outside.isDirectory());
        Files.write(new File(outside, "prepare.log").toPath(), "secret".getBytes(StandardCharsets.UTF_8));
        for (String tid : new String[] {"gc-../x", "..", "gc-", "local-abc", "xbox-4D530004", "gba-a.b"}) {
            assertFalse(tid, LogProvider.preparedKey(tid));
            try {
                provider.openFile(LogProvider.uriFor(c, tid), "r");
                fail(tid + " was served");
            } catch (FileNotFoundException expected) {
                // refused
            }
        }
    }

    @Test
    public void keyRuleFollowsConsole() {
        assertTrue(LogProvider.preparedKey("gc-GALE01-0"));
        assertTrue(LogProvider.preparedKey("ABCDEF01"));
        assertFalse(LogProvider.preparedKey(null));
        assertFalse(LogProvider.preparedKey("gc-" + "A".repeat(41)));
    }

    /** « Share the last game's log »: the file the launcher dumps is served under its own name, nothing beside it. */
    @Test
    public void servesTheLastGamesLog() throws Exception {
        File f = LogProvider.lastGameLogFile(c);
        assertTrue(f.getParentFile().mkdirs() || f.getParentFile().isDirectory());
        Files.write(f.toPath(), "the last run".getBytes(StandardCharsets.UTF_8));
        Uri uri = LogProvider.lastGameUri(c);
        assertEquals("the last run", read(uri));
        android.database.Cursor q = provider.query(uri, null, null, null, null);
        assertNotNull(q);
        assertTrue(q.moveToFirst());
        assertEquals(LogProvider.LAST_GAME_LOG, q.getString(0));
        try {
            provider.openFile(uri.buildUpon().appendPath("x").build(), "r");
            fail("a path below last-game.log was served");
        } catch (FileNotFoundException expected) {
            // refused
        }
    }

    @Test
    public void exitHeaderSaysHowTheGameEnded() {
        String h = LogProvider.exitHeader(17219, 0L, LogProvider.reasonName(
                android.app.ApplicationExitInfo.REASON_CRASH_NATIVE), 11, "crash");
        assertTrue(h, h.startsWith("# game process 17219 ended "));
        assertTrue(h, h.endsWith(": native crash, status 11 (crash)\n"));
        assertEquals("not responding", LogProvider.reasonName(android.app.ApplicationExitInfo.REASON_ANR));
        assertEquals("reason 999", LogProvider.reasonName(999));
    }

    /** No record of the game's end (a fresh install): the dump still says so and is written. */
    @Test
    public void dumpLastGameWithoutARecord() throws Exception {
        File f = LogProvider.lastGameLogFile(c);
        assertTrue(LogProvider.dumpLastGame(c, f));
        String text = new String(Files.readAllBytes(f.toPath()), StandardCharsets.UTF_8);
        assertTrue(text, text.startsWith("# XRecomp: the game's last run\n"));
        assertTrue(text, text.contains("# ---- crash buffer ----"));
    }
}
