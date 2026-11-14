package fr.adserver.xrecomp;

import static org.junit.Assert.assertArrayEquals;
import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;

import android.content.Context;

import org.junit.Before;
import org.junit.Rule;
import org.junit.Test;
import org.junit.rules.TemporaryFolder;
import org.junit.runner.RunWith;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.RuntimeEnvironment;
import org.robolectric.annotation.Config;

import java.io.File;
import java.io.IOException;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

/**
 * Disc images read in place (XBA-126): the names the app takes (the same rules as src/xiso/xiso_source.c), split
 * sets found by name and ordered from a multiple pick, the companion folder's disc.json, what ShellActivity hands
 * the runtime (a path, fds opened from the documents, or nothing when the image is gone), the switch back to
 * extraction (RECOMP_DISC_IMAGE=0), the record's data and the Storage rows of such a game.
 */
@RunWith(RobolectricTestRunner.class)
@Config(sdk = 34)
public class DiscImagesTest {
    @Rule
    public TemporaryFolder tmp = new TemporaryFolder();

    private Context ctx;

    @Before
    public void setUp() {
        ctx = RuntimeEnvironment.getApplication();
    }

    @Test
    public void imageNames() {
        for (String n : new String[] { "Halo.iso", "HALO.XISO", "Fable.cso", "Fable.ciso", "x.zso", "Burnout 3.cci",
                                       "Game.1.iso", "Game.part2.iso", "Game.iso.001", "Game.cso.2" })
            assertTrue(n, DiscImages.isImageName(n));
        for (String n : new String[] { "notes.txt", "Game.iso.txt", "game.7z", "game.chd", "Game.zip", "iso" })
            assertFalse(n, DiscImages.isImageName(n));
    }

    @Test
    public void splitPartNames() {
        DiscImages.Part p = DiscImages.part("Game.2.iso");
        assertEquals(2, p.number);
        assertEquals("Game.1.iso", p.name(1));
        p = DiscImages.part("Game.part3.cso");
        assertEquals(3, p.number);
        assertEquals("Game.part1.cso", p.name(1));
        p = DiscImages.part("Game.iso.002");
        assertEquals(2, p.number);
        assertEquals("Game.iso.001", p.name(1));                 // the zero padding is kept
        assertNull(DiscImages.part("Game.iso"));
        assertNull(DiscImages.part("1.iso"));
    }

    @Test
    public void splitSetsByName() throws Exception {
        File dir = tmp.newFolder("xbox");
        File a = touch(dir, "Fable.1.iso"), b = touch(dir, "Fable.2.iso"), c = touch(dir, "Fable.3.iso");
        assertEquals(Arrays.asList(a, b, c), DiscImages.splitParts(b));         // any part names the set
        File lone = touch(dir, "Halo.2.iso");                                   // no .1: one image
        assertEquals(List.of(lone), DiscImages.splitParts(lone));
        File one = touch(dir, "Solo.1.iso");
        assertEquals(List.of(one), DiscImages.splitParts(one));
        File plain = touch(dir, "Plain.iso");
        assertEquals(List.of(plain), DiscImages.splitParts(plain));
    }

    @Test
    public void orderOfAMultiplePick() {
        assertArrayEquals(new int[] { 0 }, DiscImages.orderParts(List.of("Halo.iso")));
        assertArrayEquals(new int[] { 2, 0, 1 }, DiscImages.orderParts(List.of("G.2.iso", "G.3.iso", "G.1.iso")));
        assertArrayEquals(new int[] { 1, 0 }, DiscImages.orderParts(List.of("G.iso.002", "G.iso.001")));
        assertNull("a missing part", DiscImages.orderParts(List.of("G.1.iso", "G.3.iso")));
        assertNull("not the first", DiscImages.orderParts(List.of("G.2.iso", "G.3.iso")));
        assertNull("two sets", DiscImages.orderParts(List.of("G.1.iso", "H.2.iso")));
        assertNull("two whole images", DiscImages.orderParts(List.of("Halo.iso", "Fable.iso")));
    }

    @Test
    public void discJsonRoundTrip() throws Exception {
        File dir = tmp.newFolder("discs", "4D5300D1");
        DiscImages.Disc d = new DiscImages.Disc();
        d.uris.add("content://x/document/a");
        d.uris.add("content://x/document/b");
        d.names.addAll(List.of("Fable.1.cso", "Fable.2.cso"));
        d.kind = "cso split";
        d.stored = 3_000_000_000L;
        d.files = 7_000_000_000L;
        DiscImages.write(dir, d);
        assertTrue(DiscImages.isCompanion(dir.getPath()));
        DiscImages.Disc back = DiscImages.read(dir);
        assertNotNull(back);
        assertFalse(back.viaPath());
        assertEquals(d.uris, back.uris);
        assertEquals("Fable.1.cso", back.name());
        assertEquals("Fable.1.cso (+1)", DiscImages.label(back));
        assertEquals(3_000_000_000L, back.stored);
        assertNull(DiscImages.read(tmp.newFolder("plain")));
    }

    @Test
    public void runtimeSpecByPath() throws Exception {
        File img = touch(tmp.newFolder("sd"), "Halo.iso");
        DiscImages.Disc d = new DiscImages.Disc();
        d.path = img.getAbsolutePath();
        assertEquals(img.getAbsolutePath(), DiscImages.runtimeSpec(d, null));
        assertTrue(img.delete());                                   // the SD card is out
        assertNull(DiscImages.runtimeSpec(d, null));
    }

    @Test
    public void runtimeSpecByDocuments() throws Exception {
        File dir = tmp.newFolder("docs");
        File a = touch(dir, "G.1.iso"), b = touch(dir, "G.2.iso");
        DiscImages.Disc d = new DiscImages.Disc();
        d.uris.add(a.getAbsolutePath());
        d.uris.add(b.getAbsolutePath());
        List<String> asked = new ArrayList<>();
        String spec = DiscImages.runtimeSpec(d, u -> {
            asked.add(u);
            return 40 + asked.size();
        });
        assertEquals("fd:41,42", spec);                             // in order, handed over to the game process
        assertEquals(d.uris, asked);
        // A part that cannot be opened (permission gone): nothing.
        assertNull(DiscImages.runtimeSpec(d, u -> {
            if (u.endsWith("2.iso"))
                throw new IOException("no permission");
            return 50;
        }));
        assertNull(DiscImages.runtimeSpec(d, u -> -1));
        assertNull(DiscImages.runtimeSpec(new DiscImages.Disc(), u -> 3));
    }

    @Test
    public void extractionSwitch() throws Exception {
        assertTrue(DiscImages.inPlaceAllowed(ctx));
        File ext = GameStore.externalFilesDir(ctx);
        TestSupport.write(new File(ext, "recomp.cfg"), "RECOMP_FB_WINDOW=1\nRECOMP_DISC_IMAGE=0\n");
        assertFalse(DiscImages.inPlaceAllowed(ctx));
        assertEquals("1", GameStore.cfgValue(ctx, "RECOMP_FB_WINDOW"));
        assertNull(GameStore.cfgGameDir(ctx));
        assertTrue(new File(ext, "recomp.cfg").delete());
    }

    /** The record of a game read from an image: data while the image is there, the Storage row is the companion. */
    @Test
    public void recordAndStorage() throws Exception {
        File files = tmp.newFolder("files"), ext = tmp.newFolder("ext"), sd = tmp.newFolder("sd");
        File img = new File(sd, "Fable.iso");
        TestSupport.write(img, new byte[5000]);
        File comp = new File(new File(files, DiscImages.DISCS_SUBDIR), "4D5300D1");
        TestSupport.write(new File(comp, "default.xbe"), TestSupport.xbe(0x4D5300D1, "Fable"));
        TestSupport.write(new File(comp, "sub/menu.xbe"), new byte[100]);
        DiscImages.Disc d = new DiscImages.Disc();
        d.path = img.getAbsolutePath();
        d.stored = 5000;
        DiscImages.write(comp, d);
        Library.Record r = new Library.Record();
        r.titleId = "4D5300D1";
        r.dataDir = comp.getAbsolutePath();
        r.dataKind = Library.KIND_IMAGE;
        assertTrue(r.hasData());

        Storage.Dirs dirs = new Storage.Dirs(files, ext);
        assertTrue(Storage.companion(dirs, comp.getPath()));
        assertFalse("not an owned copy", Storage.ownedCopy(dirs, comp.getPath()));
        List<Storage.Item> items = Storage.items(dirs, "4D5300D1", r);
        assertEquals(Storage.Kind.IMAGE, items.get(0).kind);
        assertEquals(5000, Storage.disc(items.get(0)).stored);
        assertTrue(Storage.measure(items.get(0)) < 5000);           // only the XBEs and disc.json are the app's

        Library.Games g = new Library.Games();
        g.put(r);
        Storage.delete(dirs, items.get(0), g);
        assertFalse(comp.exists());
        assertTrue("the image is never touched", img.isFile());
        assertEquals("", r.dataDir);

        // The image gone (SD card out): no data, Home says the source is gone.
        TestSupport.write(new File(comp, "default.xbe"), TestSupport.xbe(0x4D5300D1, "Fable"));
        DiscImages.write(comp, d);
        r.dataDir = comp.getAbsolutePath();
        r.dataKind = Library.KIND_IMAGE;
        assertTrue(img.delete());
        assertFalse(r.hasData());
        GameState.Facts f = new GameState.Facts();
        f.titleId = "4D5300D1";
        f.dataKind = Library.KIND_IMAGE;
        assertEquals(GameState.Chip.SOURCE_GONE, GameState.chip(f));
    }

    private static File touch(File dir, String name) throws IOException {
        File f = new File(dir, name);
        TestSupport.write(f, new byte[] { 1 });
        return f;
    }
}
