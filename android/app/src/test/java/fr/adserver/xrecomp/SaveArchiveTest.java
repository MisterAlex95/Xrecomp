package fr.adserver.xrecomp;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotEquals;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;
import static org.junit.Assert.fail;

import org.junit.Before;
import org.junit.Rule;
import org.junit.Test;
import org.junit.rules.TemporaryFolder;
import org.junit.runner.RunWith;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.annotation.Config;

import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.IOException;
import java.nio.file.Files;
import java.util.Arrays;
import java.util.Collections;
import java.util.zip.ZipEntry;
import java.util.zip.ZipOutputStream;

/**
 * Saves out and back as a zip (SaveArchive): only save folders go out, an import restores per title with the old saves
 * kept aside, and a hostile archive (« .. », absolute paths, other folders, no saves) is refused or skipped.
 * Robolectric for org.json.
 */
@RunWith(RobolectricTestRunner.class)
@Config(sdk = 34)
public class SaveArchiveTest {
    @Rule
    public TemporaryFolder tmp = new TemporaryFolder();

    private Storage.Dirs d;
    private static final String TID = "4D530004", OTHER = "4541000D", GBA = "gba-AXVE";

    @Before
    public void setUp() throws Exception {
        d = new Storage.Dirs(tmp.newFolder("files"), tmp.newFolder("ext"));
    }

    private File save(String rel, String text) throws IOException {
        File f = new File(d.saveDir(), rel);
        TestSupport.write(f, text.getBytes("UTF-8"));
        return f;
    }

    private static String read(File f) throws IOException {
        return new String(Files.readAllBytes(f.toPath()), "UTF-8");
    }

    private byte[] export(String... tids) throws IOException {
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        SaveArchive.export(d, Arrays.asList(tids), Collections.singletonMap(TID, "Sample Game"), "0.12.2", out);
        return out.toByteArray();
    }

    private SaveArchive.Summary importAll(byte[] zip) throws IOException {
        SaveArchive.Unpacked u = SaveArchive.unpack(d, new ByteArrayInputStream(zip));
        return SaveArchive.install(d, u, null);
    }

    private static byte[] zip(String... namesAndTexts) throws IOException {
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        try (ZipOutputStream z = new ZipOutputStream(out)) {
            for (int i = 0; i < namesAndTexts.length; i += 2) {
                z.putNextEntry(new ZipEntry(namesAndTexts[i]));
                z.write(namesAndTexts[i + 1].getBytes("UTF-8"));
                z.closeEntry();
            }
        }
        return out.toByteArray();
    }

    @Test
    public void keys() {
        assertEquals(TID, SaveArchive.keyOf("UDATA/4d530004/save.bin"));
        assertEquals(TID, SaveArchive.keyOf("tdata/4D530004/a/b"));
        assertEquals(GBA, SaveArchive.keyOf("gba-AXVE/game.sav"));
        assertEquals(TID, SaveArchive.keyOf("UDATA/4d530004/"));
        assertNull("not a title ID", SaveArchive.keyOf("UDATA/xyz/save.bin"));
        assertNull("not 8 digits", SaveArchive.keyOf("UDATA/4d53/save.bin"));
        assertNull("the caches", SaveArchive.keyOf("Cache/4d530004/x"));
        assertNull("a partition image", SaveArchive.keyOf("Partition3.img"));
        assertNull("no file under the folder", SaveArchive.keyOf("UDATA"));
        assertNull(SaveArchive.keyOf("../UDATA/4d530004/x"));
        assertNull(SaveArchive.keyOf("UDATA/4d530004/../../../x"));
        assertNull(SaveArchive.keyOf("/UDATA/4d530004/x"));
        assertNull(SaveArchive.keyOf("UDATA\\4d530004\\x"));
        assertNull(SaveArchive.keyOf("UDATA/4d530004//x"));
        assertNull(SaveArchive.keyOf(".stash/4D530004/x"));
        assertNull(SaveArchive.keyOf(".backup/gba-AXVE/x"));
    }

    /** Out and back on another device: the same files, only the saves (no caches), the manifest with names. */
    @Test
    public void roundTrip() throws Exception {
        save("UDATA/4d530004/0001/SaveMeta.xbx", "meta");
        save("UDATA/4d530004/0001/save.dat", "progress");
        save("TDATA/4d530004/settings.bin", "opts");
        save("UDATA/4541000d/x.bin", "other");
        save(GBA + "/game.sav", "gba");
        save("Cache/4d530004/big.bin", "cache");
        save("Partition3.img", "part");
        String hash = SaveArchive.hash(d, TID);
        byte[] zip = export(TID, GBA, "4D5300FF");

        SaveArchive.Unpacked peek = SaveArchive.unpack(d, new ByteArrayInputStream(zip));
        assertEquals(Arrays.asList(TID, GBA), peek.keys());
        assertEquals("Sample Game", peek.names.get(TID));
        assertEquals(4, peek.summary.files);
        assertTrue(peek.dir.getName().startsWith(SaveArchive.IMPORT_DIR + "-"));
        SaveArchive.discard(peek);
        assertFalse(peek.dir.exists());

        Storage.Dirs other = new Storage.Dirs(tmp.newFolder("files2"), tmp.newFolder("ext2"));
        Storage.Dirs mine = d;
        d = other;
        SaveArchive.Summary s = importAll(zip);
        assertEquals(Arrays.asList(TID, GBA), s.titles);
        assertEquals("progress", read(new File(other.saveDir(), "UDATA/4d530004/0001/save.dat")));
        assertEquals("opts", read(new File(other.saveDir(), "TDATA/4d530004/settings.bin")));
        assertEquals("gba", read(new File(other.saveDir(), GBA + "/game.sav")));
        assertFalse("not asked for", new File(other.saveDir(), "UDATA/4541000d").exists());
        assertFalse("never the caches", new File(other.saveDir(), "Cache").exists());
        assertFalse(new File(other.saveDir(), "Partition3.img").exists());
        assertEquals("the same content, the same hash", hash, SaveArchive.hash(other, TID));
        assertEquals("no import folder left", Collections.emptyList(), importLeftovers(other));
        d = mine;
    }

    /** An import replaces the title's saves, the old ones in .backup; another title is untouched. */
    @Test
    public void importReplacesAndKeepsABackup() throws Exception {
        save("UDATA/4d530004/save.dat", "new");
        byte[] zip = export(TID);
        GameStore.deleteTree(d.saveDir());
        save("UDATA/4D530004/save.dat", "old");   // an upper case folder: replaced all the same
        save("UDATA/4D530004/extra.dat", "old2");
        save("TDATA/4d530004/t.bin", "told");
        save("UDATA/4541000d/x.bin", "other");
        importAll(zip);
        File[] udata = new File(d.saveDir(), "UDATA").listFiles();
        assertEquals("one folder per title", 2, udata.length);
        assertEquals("new", read(new File(d.saveDir(), "UDATA/4d530004/save.dat")));
        assertFalse(new File(d.saveDir(), "UDATA/4d530004/extra.dat").exists());
        assertFalse("the title's saves are the archive's", new File(d.saveDir(), "TDATA/4d530004").exists());
        assertEquals("other", read(new File(d.saveDir(), "UDATA/4541000d/x.bin")));
        File backup = new File(d.saveDir(), SaveArchive.BACKUP_DIR + "/" + TID);
        assertEquals("old", read(new File(backup, "UDATA/4d530004/save.dat")));
        assertEquals("told", read(new File(backup, "TDATA/4d530004/t.bin")));
        assertEquals("the Storage screen does not list the folders aside",
                     new java.util.TreeSet<>(Arrays.asList(TID, OTHER)), Storage.titles(d, new Library.Games()));
    }

    private static java.util.List<String> importLeftovers(Storage.Dirs d) {
        java.util.List<String> l = new java.util.ArrayList<>();
        for (String n : d.saveDir().list()) {
            if (n.startsWith(SaveArchive.IMPORT_DIR))
                l.add(n);
        }
        return l;
    }

    @Test
    public void hostileArchives() throws Exception {
        save("UDATA/4d530004/save.dat", "mine");
        byte[] evil = zip("../escape.txt", "x", "UDATA/4d530004/../../../../escape2.txt", "x", "Cache/a.bin", "x",
                          "gba-AXVE/game.sav", "ok");
        SaveArchive.Summary s = importAll(evil);
        assertEquals(Collections.singletonList(GBA), s.titles);
        assertEquals(3, s.skipped);
        assertFalse(new File(d.filesDir, "escape.txt").exists());
        assertFalse(new File(d.saveDir(), "Cache").exists());
        assertEquals("mine", read(new File(d.saveDir(), "UDATA/4d530004/save.dat")));
        try {
            SaveArchive.unpack(d, new ByteArrayInputStream(zip("readme.txt", "no saves")));
            fail("an archive without saves");
        } catch (IOException expected) {
        }
        assertEquals("a refused archive leaves nothing", Collections.emptyList(), importLeftovers(d));
        try {
            SaveArchive.unpack(d, new ByteArrayInputStream("not a zip at all".getBytes("UTF-8")));
            fail("not a zip");
        } catch (IOException expected) {
        }
        try {
            SaveArchive.unpack(d, new ByteArrayInputStream(
                zip(SaveArchive.MANIFEST, "{\"format\": 99}", "gba-AXVE/game.sav", "x")));
            fail("a newer format");
        } catch (IOException expected) {
        }
    }

    @Test
    public void hashFollowsContent() throws Exception {
        assertNull("no saves", SaveArchive.hash(d, TID));
        new File(d.saveDir(), "UDATA/4d530004/empty").mkdirs();
        assertNull("an empty folder is no save", SaveArchive.hash(d, TID));
        assertFalse(SaveArchive.hasSaves(d, TID));
        File f = save("UDATA/4d530004/save.dat", "a");
        String h1 = SaveArchive.hash(d, TID);
        assertTrue(SaveArchive.hasSaves(d, TID));
        TestSupport.write(f, "b".getBytes("UTF-8"));
        assertNotEquals(h1, SaveArchive.hash(d, TID));
        TestSupport.write(f, "a".getBytes("UTF-8"));
        assertEquals(h1, SaveArchive.hash(d, TID));
        assertEquals(Collections.singletonList(TID), SaveArchive.titlesWithSaves(d, new Library.Games()));
    }

    @Test
    public void fileNames() {
        String n = SaveArchive.fileName(TID, 0);
        assertTrue(n, n.startsWith("xrecomp-saves-4d530004-") && n.endsWith(".zip"));
        assertTrue(SaveArchive.fileName(null, 0).startsWith("xrecomp-saves-all-"));
    }
}
