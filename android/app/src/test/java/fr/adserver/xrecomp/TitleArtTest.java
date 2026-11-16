package fr.adserver.xrecomp;

import static org.junit.Assert.assertArrayEquals;
import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertNotEquals;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;

import android.graphics.Bitmap;

import org.junit.Rule;
import org.junit.Test;
import org.junit.rules.TemporaryFolder;
import org.junit.runner.RunWith;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.annotation.Config;
import org.robolectric.annotation.GraphicsMode;

import java.io.File;

/**
 * TitleArt: the XBE's $$XTIMAGE section, its XPR0 container and the DXT / swizzled texels, on blobs built in the test
 * (never game data), and the monogram helpers (the launcher's colours from the image: GameThemeTest).
 */
@RunWith(RobolectricTestRunner.class)
@Config(sdk = 34)
@GraphicsMode(GraphicsMode.Mode.NATIVE)
public class TitleArtTest {
    @Rule
    public TemporaryFolder tmp = new TemporaryFolder();

    private static final int RED = 0xF800, BLUE = 0x001F;

    private static void block(byte[] d, int o, int c0, int c1, int idx) {
        d[o] = (byte)c0;
        d[o + 1] = (byte)(c0 >> 8);
        d[o + 2] = (byte)c1;
        d[o + 3] = (byte)(c1 >> 8);
        TestSupport.le32(d, o + 4, idx);
    }

    /** An 8 x 4 DXT1 image: a four-colour block, then a three-colour block with its transparent index. */
    private static byte[] dxt1Two() {
        byte[] d = new byte[16];
        // block 0: red > blue, texel i -> index i % 4: red, blue, 2/3 red, 1/3 red
        int idx = 0;
        for (int i = 0; i < 16; i++)
            idx |= (i % 4) << (2 * i);
        block(d, 0, RED, BLUE, idx);
        // block 1: blue < red (three-colour mode): texel 0 index 3 = transparent, texel 1 index 2 = midpoint
        block(d, 8, BLUE, RED, 3 | 2 << 2);
        return d;
    }

    @Test
    public void dxt1FromAnXpr0() {
        byte[] xpr = TestSupport.xpr0(0x0C, 8, 4, dxt1Two());
        int[] wh = new int[2];
        int[] px = TitleArt.decodeXpr(xpr, wh);
        assertNotNull(px);
        assertArrayEquals(new int[] { 8, 4 }, wh);
        assertEquals(0xFFFF0000, px[0]);
        assertEquals(0xFF0000FF, px[1]);
        assertEquals(170, px[2] >> 16 & 0xff);   // (2 * 255 + 0) / 3
        assertEquals(85, px[3] >> 16 & 0xff);
        assertEquals(0xFFFF0000, px[8 * 1 + 0]);  // row 1, texel 4 of block 0: index 0
        assertEquals(0, px[4]);                    // block 1, texel 0: transparent
        assertEquals(127, px[5] & 0xff);           // block 1, texel 1: midpoint, blue half
        assertEquals(127, px[5] >> 16 & 0xff);
        assertEquals(0xFF, px[5] >>> 24);
    }

    @Test
    public void dxt5Alpha() {
        byte[] d = new byte[16];
        d[0] = (byte)255;   // a0
        d[1] = 0;           // a1: eight-step mode; code 1 = a1, code 7 = 1/7 a0
        long bits = 1L | 7L << 3;
        for (int k = 0; k < 6; k++)
            d[2 + k] = (byte)(bits >> (8 * k));
        block(d, 8, RED, BLUE, 0);
        int[] wh = new int[2];
        int[] px = TitleArt.decodeXpr(TestSupport.xpr0(0x0F, 4, 4, d), wh);
        assertNotNull(px);
        assertEquals(0, px[0] >>> 24);
        assertEquals(255 / 7, px[1] >>> 24);
        assertEquals(255, px[2] >>> 24);
        assertEquals(0xFF0000, px[2] & 0xFFFFFF);
    }

    @Test
    public void swizzledArgb() {
        // 4 x 4 A8R8G8B8, swizzled: the texel at swizzled offset k holds the value k. Masks: x = 0b0101, y = 0b1010.
        byte[] d = new byte[64];
        for (int k = 0; k < 16; k++)
            TestSupport.le32(d, 4 * k, 0xFF000000 | k);
        int[] wh = new int[2];
        int[] px = TitleArt.decodeXpr(TestSupport.xpr0(0x06, 4, 4, d), wh);
        assertNotNull(px);
        assertEquals(0, px[0] & 0xff);        // (0, 0)
        assertEquals(1, px[1] & 0xff);        // (1, 0) -> x bit 0
        assertEquals(4, px[2] & 0xff);        // (2, 0) -> x bit 1 at position 2
        assertEquals(2, px[4] & 0xff);        // (0, 1) -> y bit 0 at position 1
        assertEquals(15, px[15] & 0xff);      // (3, 3)
    }

    @Test
    public void notAnXpr() {
        int[] wh = new int[2];
        assertNull(TitleArt.decodeXpr(new byte[64], wh));
        byte[] xpr = TestSupport.xpr0(0x0C, 8, 4, dxt1Two());
        TestSupport.le32(xpr, 24, 0x29 | 0x33 << 8 | 3 << 20 | 2 << 24);   // an unknown colour format
        assertNull(TitleArt.decodeXpr(xpr, wh));
        byte[] cut = java.util.Arrays.copyOf(TestSupport.xpr0(0x0C, 8, 4, dxt1Two()), 0x800 + 8);   // data cut short
        assertNull(TitleArt.decodeXpr(cut, wh));
    }

    @Test
    public void fromTheXbeAndCached() throws Exception {
        byte[] xpr = TestSupport.xpr0(0x0C, 128, 128, TestSupport.dxt1Picture());
        File xbe = tmp.newFile("default.xbe");
        TestSupport.write(xbe, TestSupport.xbeWithImage(0x4D530004, "Sample Game", xpr));
        byte[] sec = TitleArt.xbeSection(xbe, TitleArt.SECTION);
        assertArrayEquals(xpr, sec);
        assertNull(TitleArt.xbeSection(xbe, ".text"));

        File files = tmp.newFolder("files");
        Bitmap b = TitleArt.load(files, xbe, "4D530004");
        assertNotNull(b);
        assertEquals(128, b.getWidth());
        assertTrue(TitleArt.cacheFile(files, "4D530004").isFile());
        // From the cache next time, even without the XBE.
        Bitmap again = TitleArt.load(files, null, "4D530004");
        assertNotNull(again);
        assertEquals(b.getPixel(64, 10), again.getPixel(64, 10));

        // An XBE without the section (TestSupport.xbe): no image, the launcher shows the monogram.
        File plain = tmp.newFile("plain.xbe");
        TestSupport.write(plain, TestSupport.xbe(0x41420001, "Plain"));
        assertNull(TitleArt.load(files, plain, "41420001"));
    }

    /** XBA-242: no $$XTIMAGE, the save image is the cover; both present, the title image wins. */
    @Test
    public void saveImageWhenNoTitleImage() throws Exception {
        File files = tmp.newFolder("files");
        File xbe = tmp.newFile("save.xbe");
        TestSupport.write(xbe, TestSupport.xbeWithSection(0x41420002, "Saves", GameArt.SAVE_SECTION,
                                                          TestSupport.xprSolid(4, 4, 0xFF00FF00)));
        Bitmap b = TitleArt.load(files, xbe, "41420002");
        assertNotNull(b);
        assertEquals(4, b.getWidth());
        assertEquals(0xFF00FF00, b.getPixel(1, 1));
        assertTrue(TitleArt.cacheFile(files, "41420002").isFile());

        File both = tmp.newFile("both.xbe");
        TestSupport.write(both, TestSupport.xbeWithSections(0x41420003, "Both",
                GameArt.SAVE_SECTION, TestSupport.xprSolid(4, 4, 0xFF00FF00),
                TitleArt.SECTION, TestSupport.xprSolid(8, 8, 0xFFFF0000)));
        Bitmap t = TitleArt.load(files, both, "41420003");
        assertNotNull(t);
        assertEquals(8, t.getWidth());
        assertEquals(0xFFFF0000, t.getPixel(2, 2));

        // A title image that does not decode and no save image: none, the monogram.
        File bad = tmp.newFile("bad.xbe");
        TestSupport.write(bad, TestSupport.xbeWithImage(0x41420004, "Bad", new byte[64]));
        assertNull(TitleArt.load(files, bad, "41420004"));
    }

    @Test
    public void monogram() {
        assertEquals("SG", TitleArt.initials("Sample Game"));
        assertEquals("HA", TitleArt.initials("Halo"));
        assertEquals("4E", TitleArt.initials("4x4 Evo 2"));
        assertEquals(TitleArt.monogramColor("4D530004"), TitleArt.monogramColor("4d530004"));
        assertNotEquals(TitleArt.monogramColor("4D530004"), TitleArt.monogramColor("41420001"));
        assertEquals(TitleArt.titleHue("4D530004"), TitleArt.titleHue("4d530004"));
        assertTrue(TitleArt.titleHue("41420001") >= 0 && TitleArt.titleHue("41420001") < 360);
    }
}
