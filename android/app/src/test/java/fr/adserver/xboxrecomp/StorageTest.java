package fr.adserver.xboxrecomp;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertTrue;

import org.junit.Before;
import org.junit.Rule;
import org.junit.Test;
import org.junit.rules.TemporaryFolder;

import java.io.File;
import java.util.ArrayList;
import java.util.List;

/**
 * What the Storage screen lists and deletes per game (plain JUnit): the imported copy (never a folder read in place
 * nor the player's library folder), the pack, the Prepare's work files, the caches (live or stashed) and the saves,
 * each on its own; titles left behind by a game that left the library.
 */
public class StorageTest {
    @Rule
    public TemporaryFolder tmp = new TemporaryFolder();

    private File files, ext, library;
    private Storage.Dirs d;
    private static final String FABLE = "4D5300D1", OTHER = "4541000D";

    @Before
    public void setUp() throws Exception {
        files = tmp.newFolder("files");
        ext = tmp.newFolder("ext");
        library = tmp.newFolder("xbox");
        d = new Storage.Dirs(files, ext);
    }

    private Library.Record imported(String tid) throws Exception {
        File dir = Library.importDir(ext, tid);
        TestSupport.write(new File(dir, "default.xbe"), new byte[100]);
        TestSupport.write(new File(dir, "data/level.bin"), new byte[1000]);
        Library.Record r = new Library.Record();
        r.titleId = tid;
        r.dataDir = dir.getAbsolutePath();
        r.dataKind = Library.KIND_IMPORTED;
        return r;
    }

    private static List<Storage.Kind> kinds(List<Storage.Item> l) {
        List<Storage.Kind> k = new ArrayList<>();
        for (Storage.Item i : l)
            k.add(i.kind);
        return k;
    }

    @Test
    public void ownedCopies() {
        assertTrue(Storage.ownedCopy(d, Library.importDir(ext, FABLE).getPath()));
        assertTrue(Storage.ownedCopy(d, new File(ext, "game").getPath()));
        assertFalse("the staging folder", Storage.ownedCopy(d, new File(ext, "games/.import").getPath()));
        assertFalse("the games folder itself", Storage.ownedCopy(d, new File(ext, "games").getPath()));
        assertFalse("the player's library folder", Storage.ownedCopy(d, new File(library, "Fable").getPath()));
        assertFalse(Storage.ownedCopy(d, ""));
        assertFalse(Storage.ownedCopy(new Storage.Dirs(files, null), new File(ext, "game").getPath()));
    }

    @Test
    public void itemsOfAnImportedGame() throws Exception {
        Library.Record r = imported(FABLE);
        TestSupport.write(new File(Pack.packDir(files, FABLE), "libtitle.so"), new byte[500]);   // (no JSON: plain JUnit)
        TestSupport.write(new File(d.workDir(FABLE), "gen/a.c"), new byte[300]);
        TestSupport.write(new File(d.saveDir(), "Cache/x"), new byte[50]);
        SaveCaches.writeOwner(d.ownerFile(), FABLE);
        TestSupport.write(new File(d.saveDir(), "UDATA/4d5300d1/save.bin"), new byte[20]);
        List<Storage.Item> l = Storage.items(d, FABLE, r);
        assertEquals(List.of(Storage.Kind.DATA, Storage.Kind.PACK, Storage.Kind.WORK, Storage.Kind.CACHES,
                             Storage.Kind.SAVES), kinds(l));
        assertEquals(1100, Storage.measure(l.get(0)));
        assertEquals(50, Storage.measure(l.get(3)));
        assertEquals(20, Storage.measure(l.get(4)));
    }

    @Test
    public void aFolderReadInPlaceIsNeverListedNorDeleted() throws Exception {
        File dir = new File(library, "Fable");
        TestSupport.write(new File(dir, "default.xbe"), new byte[100]);
        Library.Record r = new Library.Record();
        r.titleId = FABLE;
        r.dataDir = dir.getAbsolutePath();
        r.dataKind = Library.KIND_IN_PLACE;
        assertFalse(kinds(Storage.items(d, FABLE, r)).contains(Storage.Kind.DATA));
        // Even a forged item: delete() leaves anything outside the app's storage alone.
        Storage.Item forged = new Storage.Item(FABLE, Storage.Kind.DATA);
        forged.files.add(dir);
        Storage.delete(d, forged, null);
        assertTrue(new File(dir, "default.xbe").isFile());
        Storage.Item forged2 = new Storage.Item(FABLE, Storage.Kind.PACK);
        forged2.files.add(library);
        Storage.delete(d, forged2, null);
        assertTrue(library.isDirectory());
    }

    @Test
    public void deleteTheCopyKeepsTheRecordAndTheSaves() throws Exception {
        Library.Record r = imported(FABLE);
        TestSupport.write(new File(d.saveDir(), "UDATA/4d5300d1/save.bin"), new byte[20]);
        Library.Games g = new Library.Games();
        g.put(r);
        String dir = r.dataDir;
        Storage.delete(d, Storage.items(d, FABLE, r).get(0), g);
        assertFalse(new File(dir).exists());
        assertEquals("", g.get(FABLE).dataDir);
        assertTrue(new File(d.saveDir(), "UDATA/4d5300d1/save.bin").isFile());
    }

    @Test
    public void cachesLiveOrStashed() throws Exception {
        TestSupport.write(new File(d.saveDir(), "Cache/x"), new byte[10]);
        TestSupport.write(new File(SaveCaches.stashDir(d.saveDir(), OTHER), "Cache/y"), new byte[30]);
        SaveCaches.writeOwner(d.ownerFile(), FABLE);
        Storage.Item fable = Storage.items(d, FABLE, null).get(0);
        assertEquals(Storage.Kind.CACHES, fable.kind);
        Storage.Item other = Storage.items(d, OTHER, null).get(0);
        assertEquals(30, Storage.measure(other));
        Storage.delete(d, other, null);
        assertFalse(SaveCaches.stashDir(d.saveDir(), OTHER).exists());
        assertTrue("the live set stays", new File(d.saveDir(), "Cache/x").isFile());
        Storage.delete(d, fable, null);
        assertFalse(new File(d.saveDir(), "Cache").exists());
    }

    /** Z: per title (K-9): the owner's caches are Cache/<tid> inside the live set; sized and cleared with it. */
    @Test
    public void cachesWithPerTitleZ() throws Exception {
        TestSupport.write(new File(d.saveDir(), "Cache/4d5300d1/data/a.big"), new byte[40]);
        TestSupport.write(new File(d.saveDir(), "TitleCache/c.bin"), new byte[2]);
        TestSupport.write(new File(SaveCaches.stashDir(d.saveDir(), OTHER), "Cache/4541000d/y"), new byte[30]);
        SaveCaches.writeOwner(d.ownerFile(), FABLE);
        Storage.Item fable = Storage.items(d, FABLE, null).get(0);
        assertEquals(Storage.Kind.CACHES, fable.kind);
        assertEquals(42, Storage.measure(fable));
        Storage.Item other = Storage.items(d, OTHER, null).get(0);
        assertEquals(30, Storage.measure(other));
        Storage.delete(d, fable, null);
        assertFalse(new File(d.saveDir(), "Cache").exists());
        assertTrue("another title's stash stays", SaveCaches.stashDir(d.saveDir(), OTHER).exists());
    }

    @Test
    public void titlesLeftBehind() throws Exception {
        Library.Games g = new Library.Games();
        g.put(imported(FABLE));
        TestSupport.write(new File(Pack.packDir(files, "4D530004"), "libtitle.so"), new byte[10]);
        TestSupport.write(new File(d.workDir("58420001"), "x"), new byte[1]);
        TestSupport.write(new File(d.saveDir(), "TDATA/4541000d/x"), new byte[1]);
        TestSupport.write(new File(d.saveDir(), "UDATA/notatitle/x"), new byte[1]);
        assertEquals(List.of("4541000D", "4D530004", "4D5300D1", "58420001"), new ArrayList<>(Storage.titles(d, g)));
    }
}
