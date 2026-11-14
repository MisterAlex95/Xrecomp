package fr.adserver.xrecomp;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertSame;
import static org.junit.Assert.assertTrue;

import org.junit.Before;
import org.junit.Rule;
import org.junit.Test;
import org.junit.rules.TemporaryFolder;

import java.io.File;
import java.util.List;

/**
 * The library screen's model (plain JUnit): records joined with the scan index, one entry per title ID with its chip,
 * the grid's order, games that leave it, the record made for a game seen in the folder only, the pack hash rule.
 * Synthetic XBEs only (TestSupport.xbe).
 */
public class LibraryScreenTest {
    @Rule
    public TemporaryFolder tmp = new TemporaryFolder();

    private File files, root;
    private static final String FABLE = "4D5300D1", BURNOUT = "4541000D", HALO = "4D530004";

    @Before
    public void setUp() {
        files = new File(tmp.getRoot(), "files");
        root = new File(tmp.getRoot(), "xbox");
    }

    private LibraryIndex.Item iso(String tid, String title, String name, long mtime) {
        LibraryIndex.Item i = new LibraryIndex.Item();
        i.loc = LibraryIndex.fileLoc(new File(root, name));
        i.kind = LibraryIndex.KIND_ISO;
        i.name = name;
        i.size = 7L << 30;
        i.mtime = mtime;
        i.tid = tid;
        i.title = title;
        i.xbeSha256 = "ab";
        i.xbe = "xbe/" + tid + "-00000000/default.xbe";
        return i;
    }

    private Library.Record record(String tid, String title, File data) throws Exception {
        Library.Record r = new Library.Record();
        r.titleId = tid;
        r.title = title;
        if (data != null) {
            TestSupport.write(new File(data, "default.xbe"), TestSupport.xbe((int)Long.parseLong(tid, 16), title));
            r.dataDir = data.getAbsolutePath();
            r.dataKind = Library.KIND_IMPORTED;
        }
        return r;
    }

    private static LibraryScreen.Env env() {
        LibraryScreen.Env e = new LibraryScreen.Env();
        e.accessible = true;
        return e;
    }

    @Test
    public void recordsAndIndexJoinedSortedByTitle() throws Exception {
        LibraryIndex x = new LibraryIndex();
        x.items.add(iso(BURNOUT, "Burnout 3", "Burnout 3.iso", 1));
        x.items.add(iso(FABLE, "Fable", "Fable.iso", 1));
        LibraryIndex.Item junk = new LibraryIndex.Item();
        junk.loc = "file:/x/notes.iso";
        junk.status = LibraryIndex.NOT_XISO;
        x.items.add(junk);
        Library.Games g = new Library.Games();
        g.put(record(FABLE, "Fable", new File(tmp.getRoot(), "games/" + FABLE)));
        g.put(record(HALO, "Halo", new File(tmp.getRoot(), "game")));   // imported earlier, not in the folder

        List<LibraryScreen.Entry> l = LibraryScreen.entries(files, g, x, env());
        assertEquals(3, l.size());
        assertEquals("Burnout 3", l.get(0).title());
        assertEquals("Fable", l.get(1).title());
        assertEquals("Halo", l.get(2).title());
        assertEquals(GameState.Chip.NOT_IMPORTED, l.get(0).chip);
        assertNull(l.get(0).record);
        assertEquals(GameState.Chip.READY, l.get(1).chip);   // no toolchain: the shell runs it its own way
        assertNotNull(l.get(1).active);
        assertEquals(GameState.Chip.READY, l.get(2).chip);
        assertTrue(l.get(2).variants.isEmpty());
    }

    @Test
    public void aGameWithNeitherFilesNorSourceLeavesTheGrid() throws Exception {
        Library.Games g = new Library.Games();
        g.put(record(FABLE, "Fable", null));   // its import deleted, its ISO gone
        Library.Record inPlace = record(HALO, "Halo", null);
        inPlace.dataDir = new File(tmp.getRoot(), "gone").getAbsolutePath();
        inPlace.dataKind = Library.KIND_IN_PLACE;
        g.put(inPlace);
        List<LibraryScreen.Entry> l = LibraryScreen.entries(files, g, new LibraryIndex(), env());
        assertEquals(1, l.size());
        assertEquals(HALO, l.get(0).tid);
        assertEquals(GameState.Chip.SOURCE_GONE, l.get(0).chip);
        assertEquals(GameState.Chip.GONE, LibraryScreen.entry(files, FABLE, g, new LibraryIndex(), env()).chip);
    }

    @Test
    public void variantsAndActiveSource() {
        LibraryIndex x = new LibraryIndex();
        x.items.add(iso(FABLE, "Fable", "Fable (old).iso", 1));
        x.items.add(iso(FABLE, "Fable", "Fable.iso", 5));
        Library.Games g = new Library.Games();
        LibraryScreen.Entry e = LibraryScreen.entries(files, g, x, env()).get(0);
        assertEquals(2, e.variants.size());
        assertEquals("Fable.iso", e.active.name);   // the most recently modified
        Library.Record r = new Library.Record();
        r.titleId = FABLE;
        r.source = x.items.get(0).loc;
        g.put(r);
        e = LibraryScreen.entries(files, g, x, env()).get(0);
        assertEquals("the record's source wins", "Fable (old).iso", e.active.name);
    }

    @Test
    public void noAccessAndQueue() {
        LibraryIndex x = new LibraryIndex();
        x.items.add(iso(FABLE, "Fable", "Fable.iso", 1));
        LibraryScreen.Env env = env();
        env.accessible = false;
        assertEquals(GameState.Chip.NO_FILE_ACCESS, LibraryScreen.entries(files, new Library.Games(), x, env).get(0).chip);
        env = env();
        env.queue.add(new PrepareProtocol.Job(BURNOUT, "/a", "Burnout 3"));
        env.queue.add(new PrepareProtocol.Job(FABLE, "/b", "Fable"));
        assertEquals(2, LibraryScreen.entries(files, new Library.Games(), x, env).get(0).queuePos);
        env.importingTid = FABLE;
        assertEquals(GameState.Chip.IMPORTING, LibraryScreen.entries(files, new Library.Games(), x, env).get(0).chip);
        env.importingTid = null;
        env.runningTid = FABLE;
        assertEquals(GameState.Chip.RUNNING, LibraryScreen.entries(files, new Library.Games(), x, env).get(0).chip);
    }

    @Test
    public void recordForAFolderReadInPlace() {
        LibraryIndex.Item folder = new LibraryIndex.Item();
        File dir = new File(root, "Fable");
        folder.loc = LibraryIndex.fileLoc(dir);
        folder.kind = LibraryIndex.KIND_FOLDER;
        folder.name = "Fable";
        folder.tid = FABLE;
        folder.title = "Fable";
        folder.xbeSha256 = "cd";
        LibraryIndex x = new LibraryIndex();
        x.items.add(folder);
        LibraryScreen.Entry e = LibraryScreen.entries(files, new Library.Games(), x, env()).get(0);
        Library.Record r = LibraryScreen.recordFor(e, 42);
        assertSame(r, e.record);
        assertEquals(FABLE, r.titleId);
        assertEquals("Fable", r.title);
        assertEquals(dir.getAbsolutePath(), r.dataDir);
        assertEquals(Library.KIND_IN_PLACE, r.dataKind);
        assertEquals(folder.loc, r.source);
        assertEquals("cd", r.xbeSha256);
        assertEquals(42, r.addedAt);
        assertSame("made once", r, LibraryScreen.recordFor(e, 99));

        LibraryIndex.Item image = iso(BURNOUT, "Burnout 3", "Burnout 3.iso", 1);
        x.items.add(image);
        LibraryScreen.Entry b = LibraryScreen.entries(files, new Library.Games(), x, env()).get(0);
        Library.Record rb = LibraryScreen.recordFor(b, 1);
        assertEquals("an image is imported first", "", rb.dataDir);
        assertEquals(image.loc, rb.source);
    }

    @Test
    public void localKeysHaveNoPrepareNorArt() {
        Library.Games g = new Library.Games();
        Library.Record r = new Library.Record();
        r.titleId = Library.localKey("/x/homebrew");
        r.title = "Homebrew";
        r.dataDir = new File(tmp.getRoot(), "hb").getAbsolutePath();
        r.dataKind = Library.KIND_IN_PLACE;
        g.put(r);
        LibraryScreen.Env env = env();
        env.toolchain = true;
        LibraryScreen.Entry e = LibraryScreen.entry(files, r.titleId, g, new LibraryIndex(), env);
        assertFalse(e.realTid());
        assertFalse(e.facts.toolchain);
    }

    @Test
    public void packHashRule() {
        String a = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
        assertTrue(LibraryScreen.isSha256(a));
        assertTrue(LibraryScreen.isSha256(a.toUpperCase()));
        assertFalse(LibraryScreen.isSha256("00"));
        assertFalse(LibraryScreen.isSha256(a.replace('a', 'g')));
        assertTrue("no pack: no mismatch", LibraryScreen.packMatches(files, FABLE, a));
    }

    @Test
    public void versionText() {
        assertEquals("1.02", LibraryScreen.versionText(0x102));
        assertEquals("0.00", LibraryScreen.versionText(0));
    }

    /** XBA-178: a game of another console is listed « Not playable yet », keyed by console + title key. */
    @Test
    public void otherConsolesAreNotPlayableYet() throws Exception {
        LibraryIndex x = new LibraryIndex();
        x.items.add(iso(FABLE, "Fable", "Fable.iso", 1));
        LibraryIndex.Item rom = new LibraryIndex.Item();
        rom.loc = "file:/sdcard/gba/Sample.gba";
        rom.kind = LibraryIndex.KIND_ROM;
        rom.name = "Sample.gba";
        rom.console = Console.GBA;
        rom.key = "AXVE-1A2B3C4D";
        rom.tid = "gba-AXVE-1A2B3C4D";
        rom.title = "Sample";
        rom.folder = "content://tree/gba";
        x.items.add(rom);
        LibraryScreen.Env env = env();
        env.toolchain = true;
        List<LibraryScreen.Entry> l = LibraryScreen.entries(files, new Library.Games(), x, env);
        assertEquals(2, l.size());
        assertEquals(FABLE, l.get(0).tid);
        assertEquals(GameState.Chip.NOT_IMPORTED, l.get(0).chip);   // the Xbox game: unchanged
        LibraryScreen.Entry e = l.get(1);
        assertEquals("gba-AXVE-1A2B3C4D", e.tid);
        assertEquals(Console.GBA, e.console());
        assertTrue(e.otherConsole());
        assertFalse("no Prepare, no theme, no XBE art", e.realTid());
        assertFalse(e.facts.toolchain);
        assertEquals(GameState.Chip.NOT_PLAYABLE_YET, e.chip);
        assertEquals(R.string.LIB_CHIP_NOT_PLAYABLE_YET, LibraryScreen.chipLabel(e.chip));
        assertFalse(GameState.playable(e.chip));
        assertNull(LibraryScreen.art(files, new java.util.HashMap<>(), e));

        // A record left from it (games.json keyed the same way), its file gone: it leaves the grid.
        Library.Games g = new Library.Games();
        Library.Record r = new Library.Record();
        r.titleId = "gba-AXVE-1A2B3C4D";
        r.title = "Sample";
        g.put(r);
        x.items.remove(rom);
        assertEquals(1, LibraryScreen.entries(files, g, x, env).size());
        assertEquals(GameState.Chip.GONE, LibraryScreen.entry(files, r.titleId, g, x, env).chip);
    }

    /** A library folder the app cannot read: only its games show « No file access ». */
    @Test
    public void noAccessPerFolder() {
        LibraryIndex x = new LibraryIndex();
        LibraryIndex.Item fable = iso(FABLE, "Fable", "Fable.iso", 1), halo = iso(HALO, "Halo", "Halo.iso", 1),
                burnout = iso(BURNOUT, "Burnout 3", "Burnout 3.iso", 1);
        fable.folder = "content://tree/a";
        halo.folder = "content://tree/b";
        burnout.folder = "";   // an older index: the first folder's
        x.items.add(fable);
        x.items.add(halo);
        x.items.add(burnout);
        LibraryScreen.Env env = env();
        env.noAccess = new java.util.HashSet<>(java.util.Arrays.asList("content://tree/b"));
        List<LibraryScreen.Entry> l = LibraryScreen.entries(files, new Library.Games(), x, env);
        assertEquals(GameState.Chip.NOT_IMPORTED, l.get(0).chip);   // Burnout 3
        assertEquals(GameState.Chip.NOT_IMPORTED, l.get(1).chip);   // Fable
        assertEquals(GameState.Chip.NO_FILE_ACCESS, l.get(2).chip);   // Halo
        env.noAccess = new java.util.HashSet<>(java.util.Arrays.asList("", "content://tree/a"));
        l = LibraryScreen.entries(files, new Library.Games(), x, env);
        assertEquals(GameState.Chip.NO_FILE_ACCESS, l.get(0).chip);
        assertEquals(GameState.Chip.NO_FILE_ACCESS, l.get(1).chip);
        assertEquals(GameState.Chip.NOT_IMPORTED, l.get(2).chip);
    }

    /** Library.folderOf: an item's folder, the first folder for an older item, null for a folder taken out. */
    @Test
    public void folderOfAnItem() {
        List<Library.Folder> folders = java.util.Arrays.asList(new Library.Folder("content://a", "/a"),
                                                               new Library.Folder("content://b", null));
        LibraryIndex.Item i = new LibraryIndex.Item();
        assertEquals("content://a", Library.folderOf(folders, i).tree);
        i.folder = "content://b";
        assertEquals("content://b", Library.folderOf(folders, i).tree);
        i.folder = "content://gone";
        assertNull(Library.folderOf(folders, i));
        assertNull(Library.folderOf(java.util.Collections.emptyList(), i));
        assertEquals(Console.GBA, Console.ofKey("gba-AXVE-1A2B3C4D"));
        assertEquals(Console.XBOX, Console.ofKey(FABLE));
        assertEquals(Console.XBOX, Console.ofKey("local-0a1b2c3d"));
    }
}
