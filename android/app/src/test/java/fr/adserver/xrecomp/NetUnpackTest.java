package fr.adserver.xrecomp;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;

import org.junit.Rule;
import org.junit.Test;
import org.junit.rules.TemporaryFolder;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.file.Files;
import java.util.zip.ZipEntry;
import java.util.zip.ZipOutputStream;

/**
 * NetUnpack (XBA-331): an archive unpacked beside itself through a hidden folder, refused when it holds no game or an
 * unsafe path, waiting when it does not fit, cancelled cleanly, and done at once when it was already.
 */
public class NetUnpackTest {
    @Rule
    public TemporaryFolder tmp = new TemporaryFolder();

    /** A zip of name/content pairs. */
    static File zip(File f, String... nameThenContent) throws IOException {
        f.getParentFile().mkdirs();
        try (ZipOutputStream z = new ZipOutputStream(new FileOutputStream(f))) {
            for (int i = 0; i < nameThenContent.length; i += 2) {
                z.putNextEntry(new ZipEntry(nameThenContent[i]));
                z.write(nameThenContent[i + 1].getBytes("UTF-8"));
                z.closeEntry();
            }
        }
        return f;
    }

    @Test
    public void theFolderIsBesideTheArchiveNamedAfterIt() {
        assertEquals("a/b/Game (Disc 1)", NetUnpack.dirPath("a/b/Game (Disc 1).zip"));
        assertEquals("Game", NetUnpack.dirPath("Game.ZIP"));
        assertEquals(".Game.unpacking", NetUnpack.tempDir(new File("/x/Game")).getName());
        assertTrue(NetUnpack.isArchive("A.Zip"));
        assertFalse(NetUnpack.isArchive("A.7z"));
    }

    @Test
    public void unpacksThroughAHiddenFolder() throws IOException {
        File z = zip(new File(tmp.getRoot(), "d/Game.zip"), "Game.iso", "image", "sub/notes.txt", "hi");
        File dir = new File(tmp.getRoot(), "d/Game");
        long[] seen = { 0 };
        assertNull(NetUnpack.unpack(z, dir, Long.MAX_VALUE, () -> false, (d, t) -> seen[0] = d));
        assertEquals("image", new String(Files.readAllBytes(new File(dir, "Game.iso").toPath()), "UTF-8"));
        assertTrue(new File(dir, "sub/notes.txt").isFile());
        assertFalse(NetUnpack.tempDir(dir).exists());
        assertEquals(7, seen[0]);
        assertTrue("the archive is the caller's to delete", z.isFile());
        assertNull("already unpacked", NetUnpack.unpack(z, dir, 0, null, null));
    }

    @Test
    public void noGameInsideOrAnUnsafePathIsRefusedBeforeWriting() throws IOException {
        File dir = new File(tmp.getRoot(), "Docs");
        assertEquals(NetUnpack.E_NO_GAME, NetUnpack.unpack(zip(new File(tmp.getRoot(), "Docs.zip"), "a.txt", "x"),
                                                           dir, Long.MAX_VALUE, null, null));
        assertFalse(dir.exists());
        assertFalse(NetUnpack.tempDir(dir).exists());
        File bad = new File(tmp.getRoot(), "Bad");
        assertEquals(NetUnpack.E_BAD_ZIP, NetUnpack.unpack(zip(new File(tmp.getRoot(), "Bad.zip"), "../Bad.iso", "x"),
                                                           bad, Long.MAX_VALUE, null, null));
        assertEquals(NetUnpack.E_BAD_ZIP, NetUnpack.unpack(zip(new File(tmp.getRoot(), "Deep.zip"),
                                                               "a/b/c/d/e.iso", "x"),
                                                           new File(tmp.getRoot(), "Deep"), Long.MAX_VALUE, null, null));
        assertFalse(new File(tmp.getRoot(), "Bad.iso").exists());
        assertEquals(NetUnpack.E_BAD_ZIP, NetUnpack.unpack(Files.write(new File(tmp.getRoot(), "Junk.zip").toPath(),
                                                                       "not a zip".getBytes()).toFile(),
                                                           new File(tmp.getRoot(), "Junk"), Long.MAX_VALUE, null, null));
    }

    @Test
    public void tooBigWaitsAndACancelLeavesNothing() throws IOException {
        File z = zip(new File(tmp.getRoot(), "G.zip"), "G.iso", "0123456789");
        File dir = new File(tmp.getRoot(), "G");
        assertEquals(NetRunner.E_SPACE, NetUnpack.unpack(z, dir, 5, null, null));
        assertEquals(NetUnpack.CANCELLED, NetUnpack.unpack(z, dir, Long.MAX_VALUE, () -> true, null));
        assertFalse(dir.exists());
        assertFalse(NetUnpack.tempDir(dir).exists());
        // a temporary folder left by a process that died is replaced
        new File(NetUnpack.tempDir(dir), "stale.iso").getParentFile().mkdirs();
        Files.write(new File(NetUnpack.tempDir(dir), "stale.iso").toPath(), new byte[1]);
        assertNull(NetUnpack.unpack(z, dir, Long.MAX_VALUE, null, null));
        assertFalse(new File(dir, "stale.iso").exists());
    }
}
