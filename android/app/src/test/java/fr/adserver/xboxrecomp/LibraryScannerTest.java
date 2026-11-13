package fr.adserver.xboxrecomp;

import static org.junit.Assert.assertArrayEquals;
import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;

import org.junit.Before;
import org.junit.Rule;
import org.junit.Test;
import org.junit.rules.TemporaryFolder;

import java.io.ByteArrayInputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

/**
 * LibraryScanner over an in-memory tree and a fake ImagePeek (plain JUnit): what is found, what is skipped, what a
 * rescan reopens, variants, bad images, the caps and cancel; and PathTree over a real temporary folder.
 *
 * A fake image's content says what the peek finds: "XISO" + the XBE bytes, "NOXBE", "JUNK" (not an Xbox image),
 * "HUGE" (default.xbe over the cap), "BROKEN" (an I/O error).
 */
public class LibraryScannerTest {
    @Rule
    public TemporaryFolder tmp = new TemporaryFolder();

    private File files, cache;

    @Before
    public void setUp() {
        files = new File(tmp.getRoot(), "files");
        cache = Library.xbeCacheDir(files);
    }

    static byte[] iso(int tid, String name) {
        return iso(TestSupport.xbe(tid, name));
    }

    static byte[] iso(byte[] xbe) {
        byte[] b = new byte[4 + xbe.length];
        System.arraycopy("XISO".getBytes(StandardCharsets.US_ASCII), 0, b, 0, 4);
        System.arraycopy(xbe, 0, b, 4, xbe.length);
        return b;
    }

    static byte[] text(String s) {
        return s.getBytes(StandardCharsets.US_ASCII);
    }

    /** An in-memory folder. Locations « fake:/path ». */
    static class FakeTree implements LibraryScanner.Tree {
        final Map<String, List<LibraryScanner.Node>> dirs = new HashMap<>();
        final Map<String, byte[]> data = new HashMap<>();
        final List<byte[]> opened = new ArrayList<>();
        int lists;

        FakeTree() {
            dirs.put("fake:", new ArrayList<>());
        }

        private String dirOf(String path) {
            int s = path.lastIndexOf('/');
            String parent = s < 0 ? "fake:" : "fake:/" + path.substring(0, s);
            if (!dirs.containsKey(parent))
                dir(path.substring(0, s));
            return parent;
        }

        FakeTree dir(String path) {
            String parent = dirOf(path);
            String loc = "fake:/" + path;
            dirs.put(loc, new ArrayList<>());
            dirs.get(parent).add(new LibraryScanner.Node(loc, path.substring(path.lastIndexOf('/') + 1), true, 0, 0));
            return this;
        }

        FakeTree file(String path, byte[] content, long mtime) {
            remove(path);
            String parent = dirOf(path);
            String loc = "fake:/" + path;
            data.put(loc, content);
            dirs.get(parent).add(new LibraryScanner.Node(loc, path.substring(path.lastIndexOf('/') + 1), false,
                                                         content.length, mtime));
            return this;
        }

        void remove(String path) {
            String loc = "fake:/" + path;
            for (List<LibraryScanner.Node> l : dirs.values())
                l.removeIf(n -> n.loc.equals(loc));
            data.remove(loc);
        }

        @Override
        public LibraryScanner.Node root() {
            return new LibraryScanner.Node("fake:", "xbox", true, 0, 0);
        }

        @Override
        public List<LibraryScanner.Node> list(LibraryScanner.Node dir) throws IOException {
            lists++;
            List<LibraryScanner.Node> l = dirs.get(dir.loc);
            if (l == null)
                throw new IOException("no " + dir.loc);
            return new ArrayList<>(l);
        }

        @Override
        public LibraryScanner.Opened open(LibraryScanner.Node file) throws IOException {
            byte[] b = data.get(file.loc);
            if (b == null || new String(b, 0, Math.min(6, b.length), StandardCharsets.US_ASCII).equals("BROKEN"))
                throw new IOException("cannot open " + file.loc);
            int fd = opened.size();
            opened.add(b);
            return new LibraryScanner.Opened() {
                @Override
                public int fd() {
                    return fd;
                }

                @Override
                public InputStream stream() {
                    return new ByteArrayInputStream(b);
                }

                @Override
                public void close() {
                }
            };
        }
    }

    /** Reads what FakeTree.open handed out under that fd. */
    static final class FakePeek implements LibraryScanner.ImagePeek {
        final FakeTree t;
        int calls;

        FakePeek(FakeTree t) {
            this.t = t;
        }

        int lastParts;

        @Override
        public int extractFile(int[] fds, String pathInImage, File out, long maxBytes) {
            calls++;
            lastParts = fds.length;
            assertEquals("default.xbe", pathInImage);
            assertEquals(LibraryScanner.MAX_XBE, maxBytes);
            java.io.ByteArrayOutputStream cat = new java.io.ByteArrayOutputStream();   // the parts, in order
            for (int fd : fds)
                cat.write(t.opened.get(fd), 0, t.opened.get(fd).length);
            byte[] b = cat.toByteArray();
            String head = new String(b, 0, Math.min(5, b.length), StandardCharsets.US_ASCII);
            if (head.startsWith("NOXBE"))
                return XisoNative.ENOENT;
            if (head.startsWith("HUGE"))
                return XisoNative.EINVAL;
            if (!head.startsWith("XISO"))
                return XisoNative.ENOTXISO;
            if (out.exists())
                return -6;   // O_EXCL
            try {
                Files.write(out.toPath(), Arrays.copyOfRange(b, 4, b.length));
            } catch (IOException e) {
                return -6;
            }
            return 0;
        }
    }

    private LibraryScanner.Result scan(FakeTree t, LibraryIndex prev) {
        return LibraryScanner.scan(t, prev, new FakePeek(t), cache, null);
    }

    private static List<String> names(LibraryIndex x) {
        List<String> l = new ArrayList<>();
        for (LibraryIndex.Item i : x.items)
            l.add(i.name);
        return l;
    }

    /** XBA-126: a split image is one item (its first part, the parts in order, their sizes summed), compressed
     * names are images, a lone later part is read as one file. */
    @Test
    public void splitAndCompressedImages() throws Exception {
        byte[] whole = iso(0x4D5300D1, "Fable");
        int cut = whole.length / 3;
        FakeTree t = new FakeTree()
                .file("Fable.2.iso", Arrays.copyOfRange(whole, cut, 2 * cut), 11)
                .file("Fable.1.iso", Arrays.copyOfRange(whole, 0, cut), 10)
                .file("Fable.3.iso", Arrays.copyOfRange(whole, 2 * cut, whole.length), 12)
                .file("Halo.cso", iso(0x4D530004, "Halo"), 20)
                .file("Burnout.iso.002", iso(0x4541000D, "Burnout 3"), 30);   // no .001: one file
        FakePeek peek = new FakePeek(t);
        LibraryScanner.Result r = LibraryScanner.scan(t, null, peek, cache, null);
        assertEquals(3, r.index.items.size());
        LibraryIndex.Item fable = r.index.find("fake:/Fable.1.iso");
        assertTrue(fable.ok());
        assertEquals("4D5300D1", fable.tid);
        assertEquals(Arrays.asList("fake:/Fable.1.iso", "fake:/Fable.2.iso", "fake:/Fable.3.iso"), fable.parts);
        assertEquals(whole.length, fable.size);
        assertNull(r.index.find("fake:/Fable.2.iso"));
        assertTrue(r.index.find("fake:/Halo.cso").ok());
        assertTrue(r.index.find("fake:/Burnout.iso.002").parts.isEmpty());
        // An unchanged set is reused without being opened (LibraryStoreTest: the parts in the index's JSON).
        int peeks = peek.calls;
        LibraryScanner.Result again = LibraryScanner.scan(t, r.index, peek, cache, null);
        assertEquals(peeks, peek.calls);
        assertTrue(again.index.find("fake:/Fable.1.iso").ok());
    }

    @Test
    public void findsImagesAndFolders() throws Exception {
        FakeTree t = new FakeTree()
                .file("Fable.iso", iso(0x4D5300D1, "Fable"), 10)
                .file("racing/Burnout 3.XISO", iso(TestSupport.xbe(0x4541000D, "Burnout 3", 1, 0, 2)), 20)
                .file("Halo/default.XBE", TestSupport.xbe(0x4D530004, "Halo"), 30)
                .file("Halo/media/intro.iso", iso(1, "not a game of its own"), 30)   // inside a game folder
                .file("readme.txt", text("hello"), 1);
        List<LibraryIndex.Item> seen = new ArrayList<>();
        List<String> art = new ArrayList<>();
        LibraryScanner.Result r = LibraryScanner.scan(t, null, new FakePeek(t), cache,
                (xbe, tid, title) -> {
                    assertTrue(xbe.isFile());
                    art.add(tid + " " + title);
                }, seen::add, null);
        assertFalse(r.cancelled);
        assertEquals(3, r.index.items.size());
        assertEquals(3, seen.size());
        assertEquals(3, art.size());
        assertEquals("fake:", r.index.root);
        assertTrue(r.index.scannedAt > 0);
        assertEquals(2, r.peeks);   // the two images; the folder's XBE is copied, not peeked
        assertEquals(3, r.games());

        LibraryIndex.Item fable = r.index.find("fake:/Fable.iso");
        assertEquals(LibraryIndex.OK, fable.status);
        assertEquals(LibraryIndex.KIND_ISO, fable.kind);
        assertEquals("4D5300D1", fable.tid);
        assertEquals("Fable", fable.title);
        assertEquals(10, fable.mtime);
        File xbe = fable.xbeFile(files);
        assertNotNull(xbe);
        assertArrayEquals(TestSupport.xbe(0x4D5300D1, "Fable"), Files.readAllBytes(xbe.toPath()));
        assertEquals(GameStore.sha256(xbe), fable.xbeSha256);
        assertEquals(new File(cache, "4D5300D1-" + fable.xbeSha256.substring(0, 8) + "/default.xbe"), xbe);

        LibraryIndex.Item burnout = r.index.find("fake:/racing/Burnout 3.XISO");
        assertEquals("4541000D", burnout.tid);
        assertEquals(1, burnout.region);
        assertEquals(2, burnout.version);

        LibraryIndex.Item halo = r.index.find("fake:/Halo");
        assertEquals(LibraryIndex.KIND_FOLDER, halo.kind);
        assertEquals("4D530004", halo.tid);
        assertEquals("Halo", halo.name);
        assertTrue(halo.xbeFile(files).isFile());
        assertNull("a game folder is not descended", r.index.find("fake:/Halo/media/intro.iso"));
        String[] left = cache.list((d, n) -> n.startsWith("."));
        assertEquals("no temporary file left", 0, left == null ? 0 : left.length);
    }

    @Test
    public void depthLimitAndSkippedNames() {
        FakeTree t = new FakeTree()
                .file("a/b/c/deep4.iso", iso(1, "four"), 1)            // 4 levels below the folder: found
                .file("a/b/c/d/deep5.iso", iso(2, "five"), 1)          // 5: not
                .file(".hidden/x.iso", iso(3, "hidden"), 1)
                .file(".y.iso", iso(4, "dot file"), 1)
                .file("Android/data/z.iso", iso(5, "android"), 1)
                .file("android/obb/w.iso", iso(6, "android, any case"), 1)
                .file("top.iso", iso(7, "top"), 1);
        LibraryScanner.Result r = scan(t, null);
        assertEquals(Arrays.asList("deep4.iso", "top.iso"), names(r.index));
    }

    @Test
    public void theFolderItselfCanBeAGame() {
        FakeTree t = new FakeTree().file("DEFAULT.xbe", TestSupport.xbe(0x4D530004, "Halo"), 3)
                .file("other.iso", iso(1, "x"), 1);
        LibraryScanner.Result r = scan(t, null);
        assertEquals(1, r.index.items.size());
        assertEquals("fake:", r.index.items.get(0).loc);
        assertEquals(LibraryIndex.KIND_FOLDER, r.index.items.get(0).kind);
    }

    @Test
    public void rescanReusesUnchangedItems() {
        FakeTree t = new FakeTree().file("a.iso", iso(0x11111111, "A"), 1).file("b.iso", iso(0x22222222, "B"), 1)
                .file("bad.iso", text("JUNK"), 1).file("G/default.xbe", TestSupport.xbe(0x33333333, "G"), 1);
        LibraryScanner.Result first = scan(t, null);
        assertEquals(3, first.peeks);
        assertEquals(0, first.reused);

        FakePeek peek = new FakePeek(t);
        int opens = t.opened.size();
        LibraryScanner.Result again = LibraryScanner.scan(t, first.index, peek, cache, null);
        assertEquals("a stat-only rescan", 0, peek.calls);
        assertEquals("nothing opened, the folder's XBE neither", opens, t.opened.size());
        assertEquals(4, again.reused);
        assertEquals(4, again.index.items.size());

        // b.iso changes (another mtime), a.iso is removed, c.iso is new.
        t.file("b.iso", iso(0x22222222, "B v2"), 2);
        t.remove("a.iso");
        t.file("c.iso", iso(0x44444444, "C"), 1);
        peek = new FakePeek(t);
        LibraryScanner.Result third = LibraryScanner.scan(t, again.index, peek, cache, null);
        assertEquals(2, peek.calls);
        assertEquals(2, third.reused);
        assertNull(third.index.find("fake:/a.iso"));
        assertEquals("B v2", third.index.find("fake:/b.iso").title);
        assertEquals("C", third.index.find("fake:/c.iso").title);

        // A cached XBE deleted behind the scanner's back: that item is opened again.
        GameStore.deleteTree(third.index.find("fake:/c.iso").xbeFile(files).getParentFile());
        peek = new FakePeek(t);
        LibraryIndex.Item c = LibraryScanner.scan(t, third.index, peek, cache, null).index.find("fake:/c.iso");
        assertEquals(1, peek.calls);
        assertTrue(c.xbeFile(files).isFile());
    }

    @Test
    public void variantsAndDedupe() {
        byte[] same = TestSupport.xbe(0x4D5300D1, "Fable");
        FakeTree t = new FakeTree().file("Fable.iso", iso(same), 5).file("backup/Fable copy.iso", iso(same), 7)
                .file("Fable v2.iso", iso(TestSupport.xbe(0x4D5300D1, "Fable", 4, 0, 2)), 9)
                .file("Fable disc 2.iso", iso(TestSupport.xbe(0x4D5300D1, "Fable", 4, 2, 0)), 3);
        LibraryScanner.Result r = scan(t, null);
        Map<String, List<LibraryIndex.Item>> g = r.index.games();
        assertEquals(1, g.size());
        List<LibraryIndex.Item> v = g.get("4D5300D1");
        assertEquals(4, v.size());
        assertEquals("Fable v2.iso", v.get(0).name);   // most recently modified first
        assertEquals(2, v.get(0).version);
        assertEquals(2, r.index.find("fake:/Fable disc 2.iso").disc);
        // Identical XBEs share one cached copy: three different XBEs, three directories.
        assertEquals(r.index.find("fake:/Fable.iso").xbe, r.index.find("fake:/backup/Fable copy.iso").xbe);
        assertEquals(3, cache.list((d, n) -> !n.startsWith(".")).length);
    }

    @Test
    public void badImagesAreListed() {
        byte[] notXbe = new byte[0x400];
        FakeTree t = new FakeTree().file("linux.iso", text("JUNK"), 1).file("empty.iso", text("NOXBE"), 1)
                .file("huge.iso", text("HUGE"), 1).file("broken.iso", text("BROKEN"), 1)
                .file("garbage.iso", iso(notXbe), 1).file("Folder/default.xbe", text("not an xbe"), 1)
                .file("good.iso", iso(1, "Good"), 1);
        LibraryScanner.Result r = scan(t, null);
        assertEquals(7, r.index.items.size());
        assertEquals(LibraryIndex.NOT_XISO, r.index.find("fake:/linux.iso").status);
        assertEquals(LibraryIndex.NO_XBE, r.index.find("fake:/empty.iso").status);
        assertEquals(LibraryIndex.BAD_XBE, r.index.find("fake:/huge.iso").status);
        assertEquals(LibraryIndex.UNREADABLE, r.index.find("fake:/broken.iso").status);
        assertEquals(LibraryIndex.BAD_XBE, r.index.find("fake:/garbage.iso").status);
        assertEquals(LibraryIndex.BAD_XBE, r.index.find("fake:/Folder").status);
        assertEquals(6, r.index.others().size());
        assertEquals(1, r.games());
        // Their fingerprints are kept too: a rescan does not open them again.
        FakePeek peek = new FakePeek(t);
        LibraryScanner.scan(t, r.index, peek, cache, null);
        assertEquals(0, peek.calls);
    }

    @Test
    public void entryCap() {
        FakeTree t = new FakeTree();
        for (int i = 0; i < LibraryScanner.MAX_ENTRIES + 50; i++)
            t.file(String.format("f%05d.bin", i), text("x"), 1);
        t.file("zz.iso", iso(1, "after the cap"), 1);
        LibraryScanner.Result r = scan(t, null);
        assertTrue(r.index.truncated);
        assertTrue(r.index.items.isEmpty());
        assertEquals(LibraryScanner.MAX_ENTRIES + 1, r.entries);
    }

    @Test
    public void cancel() {
        FakeTree t = new FakeTree();
        for (int i = 0; i < 10; i++)
            t.file("g" + i + ".iso", iso(0x100 + i, "G" + i), 1);
        int[] seen = { 0 };
        LibraryScanner.Result r = LibraryScanner.scan(t, null, new FakePeek(t), cache, null, i -> seen[0]++,
                                                      () -> seen[0] >= 3);
        assertTrue(r.cancelled);
        assertEquals(3, r.index.items.size());
    }

    @Test
    public void unreadableFolder() {
        LibraryScanner.Tree t = new FakeTree() {
            @Override
            public LibraryScanner.Node root() {
                return new LibraryScanner.Node("fake:/gone", "gone", true, 0, 0);
            }
        };
        LibraryScanner.Result r = LibraryScanner.scan(t, null, (fd, p, o, m) -> 0, cache, null);
        assertTrue(r.index.items.isEmpty());
        assertFalse(r.cancelled);
    }

    @Test
    public void sourceStates() {
        LibraryIndex.Item iso = new LibraryIndex.Item();
        iso.loc = "file:/sdcard/xbox/a.iso";
        assertEquals(GameState.Source.PRESENT, LibraryScanner.source(iso, true));
        assertEquals(GameState.Source.NO_ACCESS, LibraryScanner.source(iso, false));
        assertEquals(GameState.Source.MISSING, LibraryScanner.source(null, true));
        LibraryIndex.Item folder = new LibraryIndex.Item();
        folder.kind = LibraryIndex.KIND_FOLDER;
        folder.loc = "file:/sdcard/xbox/Halo";
        assertEquals(GameState.Source.PRESENT, LibraryScanner.source(folder, true));
        folder.loc = "doc:primary:xbox/Halo";
        assertEquals(GameState.Source.NEEDS_COPY, LibraryScanner.source(folder, true));
        LibraryIndex.Item docIso = new LibraryIndex.Item();
        docIso.loc = "doc:primary:xbox/a.iso";
        assertEquals(GameState.Source.PRESENT, LibraryScanner.source(docIso, true));
    }

    // ---- PathTree over a real folder ----------------------------------------------------------------------------

    /** PathTree with stream-only opens (no ParcelFileDescriptor here); the fd is an index into opened. */
    static final class TestPathTree extends LibraryScanner.PathTree {
        final List<File> opened = new ArrayList<>();

        TestPathTree(File root) {
            super(root);
        }

        @Override
        LibraryScanner.Opened openFile(File f) {
            int fd = opened.size();
            opened.add(f);
            return new LibraryScanner.Opened() {
                @Override
                public int fd() {
                    return fd;
                }

                @Override
                public InputStream stream() throws IOException {
                    return new FileInputStream(f);
                }

                @Override
                public void close() {
                }
            };
        }
    }

    @Test
    public void pathTree() throws Exception {
        File root = new File(tmp.getRoot(), "sdcard/xbox");
        TestSupport.write(new File(root, "Fable.iso"), iso(0x4D5300D1, "Fable"));
        TestSupport.write(new File(root, "Halo/default.xbe"), TestSupport.xbe(0x4D530004, "Halo"));
        TestSupport.write(new File(root, "Android/data/x.iso"), iso(1, "no"));
        TestSupport.write(new File(root, "notes.txt"), "x");
        TestPathTree t = new TestPathTree(root);
        LibraryScanner.ImagePeek peek = (fds, path, out, max) -> {
            try {
                byte[] b = Files.readAllBytes(t.opened.get(fds[0]).toPath());
                Files.write(out.toPath(), Arrays.copyOfRange(b, 4, b.length));
                return 0;
            } catch (IOException e) {
                return -1;
            }
        };
        LibraryScanner.Result r = LibraryScanner.scan(t, null, peek, cache, null);
        assertEquals(LibraryIndex.fileLoc(root), r.index.root);
        assertEquals(2, r.index.items.size());
        LibraryIndex.Item fable = r.index.find(LibraryIndex.fileLoc(new File(root, "Fable.iso")));
        assertEquals("4D5300D1", fable.tid);
        assertEquals(new File(root, "Fable.iso").length(), fable.size);
        assertEquals(new File(root, "Fable.iso").lastModified(), fable.mtime);
        LibraryIndex.Item halo = r.index.find(LibraryIndex.fileLoc(new File(root, "Halo")));
        assertEquals("4D530004", halo.tid);
        assertEquals(GameState.Source.PRESENT, LibraryScanner.source(halo, true));

        // Unchanged: nothing opened. Touched: opened again.
        int opens = t.opened.size();
        LibraryScanner.Result again = LibraryScanner.scan(t, r.index, peek, cache, null);
        assertEquals(opens, t.opened.size());
        assertEquals(2, again.reused);
        assertTrue(new File(root, "Fable.iso").setLastModified(new File(root, "Fable.iso").lastModified() + 5000));
        LibraryScanner.scan(t, again.index, peek, cache, null);
        assertEquals(opens + 1, t.opened.size());

        // The folder renamed away: an empty result, no crash.
        assertTrue(root.renameTo(new File(root.getParentFile(), "xbox2")));
        assertTrue(LibraryScanner.scan(t, again.index, peek, cache, null).index.items.isEmpty());
    }

    @Test
    public void imageNames() {
        assertTrue(LibraryScanner.isImage("a.iso"));
        assertTrue(LibraryScanner.isImage("A.ISO"));
        assertTrue(LibraryScanner.isImage("b.xiso"));
        assertFalse(LibraryScanner.isImage("c.iso.part"));
        assertFalse(LibraryScanner.isImage("iso"));
        assertEquals("Fable", LibraryScanner.stripExtension("Fable.iso"));
        assertEquals(".iso", LibraryScanner.stripExtension(".iso"));
    }
}
