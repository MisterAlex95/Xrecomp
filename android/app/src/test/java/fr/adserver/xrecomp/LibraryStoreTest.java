package fr.adserver.xrecomp;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;

import org.junit.Before;
import org.junit.Rule;
import org.junit.Test;
import org.junit.rules.TemporaryFolder;
import org.junit.runner.RunWith;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.annotation.Config;

import java.io.File;
import java.util.Arrays;
import java.util.List;
import java.util.Map;

/**
 * games.json and index.json: round trips, corrupt files, grouping of variants, play time, and the migration from the
 * one-game launcher (four start states, idempotence). Files only, no Context (Robolectric for org.json).
 */
@RunWith(RobolectricTestRunner.class)
@Config(sdk = 34)
public class LibraryStoreTest {
    @Rule
    public TemporaryFolder tmp = new TemporaryFolder();

    private File files, ext;

    @Before
    public void setUp() {
        files = new File(tmp.getRoot(), "files");
        ext = new File(tmp.getRoot(), "ext");
    }

    private static Library.Record record(String tid, String title, long lastPlayed) {
        Library.Record r = new Library.Record();
        r.titleId = tid;
        r.title = title;
        r.source = "file:/sdcard/xbox/" + title + ".iso";
        r.dataDir = "/data/games/" + tid;
        r.dataKind = Library.KIND_IMPORTED;
        r.xbeSha256 = "ab";
        r.addedAt = 10;
        r.lastPlayed = lastPlayed;
        r.playMs = 1234;
        return r;
    }

    @Test
    public void gamesRoundTrip() throws Exception {
        Library.Games g = new Library.Games();
        g.put(record("4D5300D1", "Fable", 100));
        g.put(record("4541000D", "Burnout 3", 200));
        g.selected = "4D5300D1";
        File f = Library.gamesFile(files);
        Library.write(f, g);
        assertFalse(new File(f.getPath() + ".tmp").exists());
        Library.Games back = Library.read(f);
        assertEquals("4D5300D1", back.selected);
        assertEquals(2, back.games.size());
        Library.Record r = back.get("4541000D");
        assertEquals("Burnout 3", r.title);
        assertEquals("file:/sdcard/xbox/Burnout 3.iso", r.source);
        assertEquals("/data/games/4541000D", r.dataDir);
        assertEquals(Library.KIND_IMPORTED, r.dataKind);
        assertEquals("ab", r.xbeSha256);
        assertEquals(10, r.addedAt);
        assertEquals(200, r.lastPlayed);
        assertEquals(1234, r.playMs);
        assertEquals("Fable", back.selected().title);
        assertEquals(Arrays.asList("Burnout 3", "Fable"),
                     Arrays.asList(back.byLastPlayed().get(0).title, back.byLastPlayed().get(1).title));
        back.remove("4D5300D1");
        assertEquals("", back.selected);
        assertEquals("Burnout 3", back.selected().title);   // the most recently played
    }

    @Test
    public void missingAndCorruptGames() throws Exception {
        File f = Library.gamesFile(files);
        assertTrue(Library.read(f).games.isEmpty());
        assertNull(Library.read(f).selected());
        TestSupport.write(f, "{\"v\":1, \"games\": {");
        assertTrue(Library.read(f).games.isEmpty());
        assertFalse(f.exists());
        assertTrue("kept aside, not lost", new File(f.getPath() + ".bad").isFile());
        TestSupport.write(f, "{\"v\":1,\"games\":{\"A\":{\"title\":\"x\",\"play_ms\":-5},\"B\":3}}");
        Library.Games g = Library.read(f);
        assertEquals(1, g.games.size());
        assertEquals(0, g.get("A").playMs);
    }

    @Test
    public void playTime() {
        Library.Games g = new Library.Games();
        g.put(record("4D5300D1", "Fable", 0));
        g.get("4D5300D1").playMs = 0;
        Library.sessionStart(g, "4D5300D1", 1000);
        assertEquals(1000, g.get("4D5300D1").lastPlayed);
        Library.sessionEnd(g, "4D5300D1", 1000, 61_000);
        assertEquals(60_000, g.get("4D5300D1").playMs);
        Library.sessionEnd(g, "4D5300D1", 1000, 1000 + 100 * GameStore.SESSION_MAX);   // capped
        assertEquals(60_000 + GameStore.SESSION_MAX, g.get("4D5300D1").playMs);
        Library.sessionEnd(g, "4D5300D1", 5000, 1000);                                  // clock went back
        assertEquals(60_000 + GameStore.SESSION_MAX, g.get("4D5300D1").playMs);
        Library.sessionEnd(g, "NOPE", 1, 2);
    }

    private static LibraryIndex.Item item(String loc, String tid, String title, long mtime) {
        LibraryIndex.Item i = new LibraryIndex.Item();
        i.loc = loc;
        i.name = loc.substring(loc.lastIndexOf('/') + 1);
        i.tid = tid;
        i.title = title;
        i.mtime = mtime;
        i.size = 7;
        i.region = 4;
        i.xbeSha256 = "cd";
        i.xbe = "xbe/" + tid + "-cd/default.xbe";
        return i;
    }

    @Test
    public void indexRoundTripAndGrouping() throws Exception {
        LibraryIndex x = new LibraryIndex();
        x.root = "file:/sdcard/xbox";
        x.scannedAt = 99;
        x.items.add(item("file:/x/Fable.iso", "4D5300D1", "Fable", 5));
        x.items.add(item("file:/x/Fable (v2).iso", "4D5300D1", "Fable", 9));
        x.items.add(item("file:/x/burnout", "4541000D", "Burnout 3", 1));
        x.items.get(2).kind = LibraryIndex.KIND_FOLDER;
        x.items.get(1).parts.addAll(Arrays.asList("file:/x/Fable (v2).iso", "file:/x/Fable (v2).2.iso"));   // XBA-126
        LibraryIndex.Item bad = new LibraryIndex.Item();
        bad.loc = "file:/x/linux.iso";
        bad.name = "linux.iso";
        bad.status = LibraryIndex.NOT_XISO;
        x.items.add(bad);
        File f = Library.indexFile(files);
        LibraryIndex.write(f, x);
        LibraryIndex back = LibraryIndex.read(f);
        assertEquals("file:/sdcard/xbox", back.root);
        assertEquals(99, back.scannedAt);
        assertEquals(4, back.items.size());
        LibraryIndex.Item b = back.find("file:/x/burnout");
        assertEquals(LibraryIndex.KIND_FOLDER, b.kind);
        assertEquals("4541000D", b.tid);
        assertEquals(4, b.region);
        assertEquals(new File(files, "library/xbe/4541000D-cd/default.xbe"), b.xbeFile(files));
        assertTrue(b.sameFingerprint("file:/x/burnout", 7, 1));
        assertEquals(Arrays.asList("file:/x/Fable (v2).iso", "file:/x/Fable (v2).2.iso"),
                     back.find("file:/x/Fable (v2).iso").parts);
        assertTrue(back.find("file:/x/Fable.iso").parts.isEmpty());
        assertFalse(b.sameFingerprint("file:/x/burnout", 7, 2));

        Map<String, List<LibraryIndex.Item>> games = back.games();
        assertEquals(Arrays.asList("4541000D", "4D5300D1"), Arrays.asList(games.keySet().toArray()));
        List<LibraryIndex.Item> fable = games.get("4D5300D1");
        assertEquals(2, fable.size());
        assertEquals("file:/x/Fable (v2).iso", fable.get(0).loc);                    // newest first
        assertEquals("file:/x/Fable (v2).iso", LibraryIndex.active(fable, null).loc);
        assertEquals("file:/x/Fable.iso", LibraryIndex.active(fable, "file:/x/Fable.iso").loc);
        assertEquals("file:/x/Fable (v2).iso", LibraryIndex.active(fable, "file:/gone.iso").loc);
        assertNull(LibraryIndex.active(null, null));
        assertEquals(1, back.others().size());
        assertEquals("linux.iso", back.others().get(0).name);
    }

    @Test
    public void corruptIndexIsEmpty() throws Exception {
        File f = Library.indexFile(files);
        assertTrue(LibraryIndex.read(f).items.isEmpty());
        TestSupport.write(f, "not json");
        assertTrue(LibraryIndex.read(f).items.isEmpty());
        // An « ok » item without a title ID is not a game.
        TestSupport.write(f, "{\"items\":[{\"loc\":\"file:/a.iso\",\"status\":\"ok\"},{\"name\":\"no loc\"}]}");
        LibraryIndex x = LibraryIndex.read(f);
        assertEquals(1, x.items.size());
        assertEquals(LibraryIndex.BAD_XBE, x.items.get(0).status);
    }

    @Test
    public void locations() {
        assertEquals("file:/sdcard/xbox/a.iso", LibraryIndex.fileLoc(new File("/sdcard/xbox/a.iso")));
        assertEquals(new File("/sdcard/xbox/a.iso"), LibraryIndex.locFile("file:/sdcard/xbox/a.iso"));
        assertNull(LibraryIndex.locFile("doc:primary:xbox/a.iso"));
        assertEquals("doc:primary:xbox", LibraryIndex.docLoc("primary:xbox"));
        String k = Library.localKey("/sdcard/xbox/thing");
        assertTrue(k.matches("local-[0-9a-f]{8}"));
        assertEquals(k, Library.localKey("/sdcard/xbox/thing"));
        assertTrue(Library.isLocalKey(k));
        assertTrue(SaveCaches.safeName(k));
    }

    // ---- Migration ----------------------------------------------------------------------------------------------

    private File gameDir(File dir, int tid, String name) throws Exception {
        TestSupport.write(new File(dir, "default.xbe"), TestSupport.xbe(tid, name));
        return dir;
    }

    private Library.Legacy legacy() {
        Library.Legacy l = new Library.Legacy();
        l.defaultGameDir = new File(ext, "game");
        return l;
    }

    @Test
    public void migrateNothing() throws Exception {
        Library.Games g = Library.migrate(files, legacy(), 5);
        assertTrue(g.games.isEmpty());
        assertFalse("nothing written: a game chosen later is migrated then", Library.gamesFile(files).exists());
        assertFalse(Library.cacheOwnerFile(files).exists());
    }

    @Test
    public void migrateImportedCopy() throws Exception {
        Library.Legacy l = legacy();
        gameDir(l.defaultGameDir, 0x4D5300D1, "Fable");
        l.lastPlayed = 777;
        l.playMs = 3_600_000;
        Library.Games g = Library.migrate(files, l, 5);
        Library.Record r = g.selected();
        assertNotNull(r);
        assertEquals("4D5300D1", r.titleId);
        assertEquals("4D5300D1", g.selected);
        assertEquals("Fable", r.title);
        assertEquals(l.defaultGameDir.getAbsolutePath(), r.dataDir);
        assertEquals(Library.KIND_IMPORTED, r.dataKind);
        assertEquals("", r.source);
        assertEquals(GameStore.sha256(new File(l.defaultGameDir, "default.xbe")), r.xbeSha256);
        assertEquals(777, r.lastPlayed);
        assertEquals(3_600_000, r.playMs);
        assertEquals(5, r.addedAt);
        assertTrue(r.hasData());
        assertEquals("4D5300D1", SaveCaches.owner(Library.cacheOwnerFile(files)));
        assertTrue("nothing moved", new File(l.defaultGameDir, "default.xbe").isFile());
        assertEquals("4D5300D1", Library.read(Library.gamesFile(files)).selected().titleId);
    }

    @Test
    public void migrateChosenImportedCopyAndInPlaceFolder() throws Exception {
        Library.Legacy l = legacy();
        gameDir(l.defaultGameDir, 0x4D5300D1, "Fable");
        l.chosenDir = l.defaultGameDir.getAbsolutePath() + "/.";   // another spelling of the same folder
        l.chosenTitle = "Fable: The Lost Chapters";
        Library.Record r = Library.migrate(files, l, 5).selected();
        assertEquals(Library.KIND_IMPORTED, r.dataKind);
        assertEquals("Fable: The Lost Chapters", r.title);

        File files2 = new File(tmp.getRoot(), "files2");
        File folder = gameDir(new File(tmp.getRoot(), "sdcard/xbox/Burnout"), 0x4541000D, "Burnout 3");
        Library.Legacy m = legacy();
        m.chosenDir = folder.getAbsolutePath();
        r = Library.migrate(files2, m, 5).selected();
        assertEquals("4541000D", r.titleId);
        assertEquals(Library.KIND_IN_PLACE, r.dataKind);
        assertEquals(folder.getAbsolutePath(), r.dataDir);
        assertEquals(LibraryIndex.fileLoc(folder), r.source);
        assertEquals("Burnout 3", r.title);
    }

    @Test
    public void migrateRecompCfg() throws Exception {
        File dir = gameDir(new File(tmp.getRoot(), "adb/game"), 0x12345678, "Pushed");
        Library.Legacy l = legacy();
        gameDir(l.defaultGameDir, 0x4D5300D1, "Fable");   // RECOMP_GAME_DIR wins over files/game, as gameDir() does
        l.cfgDir = dir.getAbsolutePath();
        Library.Record r = Library.migrate(files, l, 5).selected();
        assertEquals("12345678", r.titleId);
        assertEquals(Library.KIND_CFG, r.dataKind);
        assertEquals(dir.getAbsolutePath(), r.dataDir);
    }

    @Test
    public void migrateAFolderWithoutAnXbe() throws Exception {
        // The chosen folder lost its default.xbe (or never was a game): a local key, the folder's name.
        File dir = new File(tmp.getRoot(), "broken");
        TestSupport.write(new File(dir, "default.xbe"), "not an xbe");
        Library.Legacy l = legacy();
        l.chosenDir = dir.getAbsolutePath();
        Library.Record r = Library.migrate(files, l, 5).selected();
        assertTrue(r.titleId, Library.isLocalKey(r.titleId));
        assertEquals("broken", r.title);
    }

    @Test
    public void migrateIsIdempotent() throws Exception {
        Library.Legacy l = legacy();
        gameDir(l.defaultGameDir, 0x4D5300D1, "Fable");
        Library.Games g = Library.migrate(files, l, 5);
        // The player plays on; a second migrate must not reset the record from the preferences.
        g.get("4D5300D1").playMs = 99;
        Library.write(Library.gamesFile(files), g);
        l.chosenDir = gameDir(new File(tmp.getRoot(), "elsewhere"), 0x11111111, "Other").getAbsolutePath();
        Library.Games again = Library.migrate(files, l, 6);
        assertEquals(1, again.games.size());
        assertEquals(99, again.get("4D5300D1").playMs);
        assertEquals(5, again.get("4D5300D1").addedAt);
        // A corrupt games.json is set aside and migrated again.
        TestSupport.write(Library.gamesFile(files), "{");
        Library.Games rebuilt = Library.migrate(files, l, 7);
        assertEquals("11111111", rebuilt.selected().titleId);
        assertTrue(new File(Library.gamesFile(files).getPath() + ".bad").isFile());
        assertEquals("the cache owner stays", "4D5300D1", SaveCaches.owner(Library.cacheOwnerFile(files)));
    }

    @Test
    public void importDir() {
        assertEquals(new File(ext, "games/4D5300D1"), Library.importDir(ext, "4D5300D1"));
    }
}
