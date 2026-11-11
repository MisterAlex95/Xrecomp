package fr.adserver.xboxrecomp;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;

import android.app.Application;

import org.junit.Before;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.RuntimeEnvironment;
import org.robolectric.annotation.Config;

import java.io.File;

/** Library.migrate(Context): from the app's preferences and files, once; the launched title ID in the preferences. */
@RunWith(RobolectricTestRunner.class)
@Config(sdk = 34)
public class LibraryContextTest {
    private Application app;

    @Before
    public void setUp() {
        app = RuntimeEnvironment.getApplication();
        GameStore.prefs(app).edit().clear().commit();
        GameStore.deleteTree(Library.dir(app.getFilesDir()));
    }

    @Test
    public void migratesTheChosenGameOnce() throws Exception {
        File dir = new File(app.getExternalFilesDir(null), "game");
        TestSupport.write(new File(dir, "default.xbe"), TestSupport.xbe(0x4D5300D1, "Fable"));
        GameStore.remember(app, dir.getAbsolutePath(), "Fable TLC");
        GameStore.sessionStart(app, 1000);
        GameStore.sessionEnd(app, 61_000);

        Library.Games g = Library.migrate(app);
        Library.Record r = g.selected();
        assertEquals("4D5300D1", r.titleId);
        assertEquals("Fable TLC", r.title);
        assertEquals(Library.KIND_IMPORTED, r.dataKind);
        assertEquals(60_000, r.playMs);
        assertEquals(1000, r.lastPlayed);
        assertEquals("4D5300D1", GameStore.prefs(app).getString(Library.PREF_TITLE_ID, null));
        // The one-game preferences stay: an older APK still finds its game.
        assertEquals(dir.getAbsolutePath(), GameStore.chosenDir(app));

        GameStore.remember(app, "/elsewhere", "Other");
        assertEquals("4D5300D1", Library.migrate(app).selected().titleId);
    }

    @Test
    public void nothingToMigrate() {
        assertTrue(Library.migrate(app).games.isEmpty());
        assertNull(GameStore.prefs(app).getString(Library.PREF_TITLE_ID, null));
    }
}
