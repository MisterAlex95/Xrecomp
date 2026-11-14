package fr.adserver.xrecomp;

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
        assertNull(LibraryScanner.scanLibrary(app, (LibraryScanner.Tree)null, null, null));
    }

    /** XBA-178: the one folder of an older launcher (library_tree / library_path) becomes the first entry, once. */
    @Test
    public void singleFolderMigratesAsTheFirstEntry() {
        GameStore.prefs(app).edit().putString(Library.PREF_TREE, "content://tree/primary%3Axbox")
                .putString(Library.PREF_PATH, "/storage/emulated/0/xbox").commit();
        java.util.List<Library.Folder> l = Library.folders(app);
        assertEquals(1, l.size());
        assertEquals("content://tree/primary%3Axbox", l.get(0).tree);
        assertEquals("/storage/emulated/0/xbox", l.get(0).path);
        assertEquals("xbox", l.get(0).name());
        // Written once as library_folders; the old keys go.
        assertNull(GameStore.prefs(app).getString(Library.PREF_TREE, null));
        assertNull(GameStore.prefs(app).getString(Library.PREF_PATH, null));
        assertEquals(Library.foldersJson(l), GameStore.prefs(app).getString(Library.PREF_FOLDERS, null));

        // Then folders are added after it, and the first keeps its place.
        assertTrue(Library.addFolder(app, "content://tree/primary%3Agba", null));
        assertTrue(Library.addFolder(app, "content://tree/primary%3Aps1", "/storage/emulated/0/ps1"));
        l = Library.folders(app);
        assertEquals(3, l.size());
        assertEquals("content://tree/primary%3Axbox", l.get(0).tree);
        assertNull("through the Storage Access Framework", l.get(1).path);
        assertEquals("gba", l.get(1).name());
        assertEquals("content://tree/primary%3Axbox", Library.folderTree(app));
        assertTrue(Library.anyFolderPath(app));

        // The same folder again updates its entry in place (All files access granted since).
        Library.addFolder(app, "content://tree/primary%3Agba", "/storage/emulated/0/gba");
        l = Library.folders(app);
        assertEquals(3, l.size());
        assertEquals("/storage/emulated/0/gba", l.get(1).path);

        assertTrue(Library.removeFolder(app, "content://tree/primary%3Axbox"));
        l = Library.folders(app);
        assertEquals(2, l.size());
        assertEquals("content://tree/primary%3Agba", Library.folderTree(app));
        assertEquals(2, LibraryScanner.folderRoots(app, true).size());
        assertTrue(LibraryScanner.folderRoots(app, true).get(0).tree instanceof LibraryScanner.PathTree);
        assertTrue(LibraryScanner.folderRoots(app, false).get(0).tree instanceof LibraryScanner.DocTree);
        assertEquals("content://tree/primary%3Agba", LibraryScanner.folderRoots(app, true).get(0).folder);
    }

    /** No folder, a SAF folder only, an unreadable library_folders value. */
    @Test
    public void folderListEdges() {
        assertTrue(Library.folders(app).isEmpty());
        assertNull(GameStore.prefs(app).getString(Library.PREF_FOLDERS, null));   // nothing written for nothing
        assertEquals(1, Library.folders(null, "content://tree/x", null).size());
        assertNull(Library.folders(null, "content://tree/x", null).get(0).path);
        assertTrue("library_folders wins over the old keys",
                   Library.folders("[]", "content://tree/x", "/x").isEmpty());
        assertEquals("unreadable: the old keys", 1, Library.folders("{bad", "content://tree/x", "/x").size());
        assertEquals("entries without a tree are dropped", 1,
                     Library.parseFolders("[{\"tree\":\"content://a\"},{\"path\":\"/b\"},3]").size());
    }

    /** Folders the app cannot read: a path without All files access, a tree without its persisted permission. */
    @Test
    public void unreadableFolders() {
        Library.addFolder(app, "content://tree/a", "/storage/emulated/0/a");
        Library.addFolder(app, "content://tree/b", null);
        assertEquals(new java.util.HashSet<>(java.util.Arrays.asList("content://tree/b")),
                     Library.unreadableFolders(app, true));
        assertEquals(new java.util.HashSet<>(java.util.Arrays.asList("", "content://tree/a", "content://tree/b")),
                     Library.unreadableFolders(app, false));
    }

    /** Several folders read as files: one index, each item stamped with its folder, a game per console. */
    @Test
    public void scanLibraryOverSeveralFolders() throws Exception {
        File xbox = new File(app.getExternalFilesDir(null), "xbox"), gba = new File(app.getExternalFilesDir(null), "gba");
        TestSupport.write(new File(xbox, "Halo/default.xbe"), TestSupport.xbe(0x4D530004, "Halo"));
        TestSupport.write(new File(gba, "Sample.gba"), ConsoleTest.gba("AXVE"));
        Library.addFolder(app, "content://tree/xbox", xbox.getAbsolutePath());
        Library.addFolder(app, "content://tree/gba", gba.getAbsolutePath());
        LibraryScanner.Result r = LibraryScanner.scanLibrary(app, LibraryScanner.folderRoots(app, true), null, null);
        assertEquals(2, r.games());
        LibraryIndex saved = LibraryIndex.read(Library.indexFile(app.getFilesDir()));
        LibraryIndex.Item halo = saved.find(LibraryIndex.fileLoc(new File(xbox, "Halo")));
        LibraryIndex.Item rom = saved.find(LibraryIndex.fileLoc(new File(gba, "Sample.gba")));
        assertEquals("content://tree/xbox", halo.folder);
        assertEquals(Console.XBOX, halo.console);
        assertEquals("content://tree/gba", rom.folder);
        assertEquals(Console.GBA, rom.console);
        assertTrue(rom.tid.startsWith("gba-AXVE-"));
        assertEquals("Sample", rom.title);
        assertEquals(LibraryIndex.fileLoc(xbox), saved.root);
        assertEquals("a rescan opens nothing", 2,
                     LibraryScanner.scanLibrary(app, LibraryScanner.folderRoots(app, true), null, null).reused);
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
