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

/**
 * Library.migrate(Context): from the app's preferences and files, once; the launched title ID in the preferences.
 * The library folder preferences, and LibraryScanner.scanLibrary in path mode over game folders (no image: the
 * native peek is not loaded on the JVM).
 */
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

    @Test
    public void folderPreferences() {
        assertNull(Library.folderTree(app));
        assertNull(Library.folderPath(app));
        Library.setScannedAt(app, 5);
        assertTrue(Library.setFolder(app, "content://tree/x", "/storage/emulated/0/xbox"));
        assertEquals("content://tree/x", Library.folderTree(app));
        assertEquals("/storage/emulated/0/xbox", Library.folderPath(app));
        assertEquals("a new folder is not scanned yet", 0, Library.scannedAt(app));
        Library.setFolder(app, "content://tree/y", null);   // the SAF fallback
        assertEquals("content://tree/y", Library.folderTree(app));
        assertNull(Library.folderPath(app));
        Library.clearFolder(app);
        assertNull(Library.folderTree(app));
        assertNull(LibraryScanner.folderTree(app, true));
        assertNull(LibraryScanner.scanLibrary(app, null, null, null));
    }

    @Test
    public void scanLibraryInPathMode() throws Exception {
        File root = new File(app.getExternalFilesDir(null), "xbox");
        TestSupport.write(new File(root, "Halo/default.xbe"), TestSupport.xbeWithImage(0x4D530004, "Halo",
                TestSupport.xpr0(0x0C, 128, 128, TestSupport.dxt1Picture())));
        TestSupport.write(new File(root, "Other/default.xbe"), TestSupport.xbe(0x4541000D, "Other"));
        Library.setFolder(app, "content://tree/x", root.getAbsolutePath());
        LibraryScanner.Tree tree = LibraryScanner.folderTree(app, true);   // with All files access
        assertTrue(tree instanceof LibraryScanner.PathTree);

        LibraryScanner.Result r = LibraryScanner.scanLibrary(app, tree, null, null);
        assertEquals(2, r.games());
        LibraryIndex saved = LibraryIndex.read(Library.indexFile(app.getFilesDir()));
        assertEquals(2, saved.items.size());
        assertEquals(saved.scannedAt, Library.scannedAt(app));
        assertTrue("the art hook cached Halo's title image", TitleArt.cacheFile(app.getFilesDir(), "4D530004").isFile());
        assertTrue(saved.find(LibraryIndex.fileLoc(new File(root, "Other"))).xbeFile(app.getFilesDir()).isFile());

        assertEquals(2, LibraryScanner.scanLibrary(app, tree, null, null).reused);

        // Without All files access: the SAF tree.
        assertTrue(LibraryScanner.folderTree(app, false) instanceof LibraryScanner.DocTree);
    }
}
