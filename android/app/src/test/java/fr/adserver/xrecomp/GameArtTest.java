package fr.adserver.xrecomp;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertNull;

import java.io.File;
import java.io.IOException;

import org.junit.Rule;
import org.junit.Test;
import org.junit.rules.TemporaryFolder;

/**
 * GameArt: the theme picture's sources and their order, on XBEs and XPR0 files built here (plain JUnit; no game
 * data).
 */
public class GameArtTest {
    @Rule
    public TemporaryFolder tmp = new TemporaryFolder();

    private static final int TID = 0x4D530004;
    private static final int BLUE = 0xFF2850DC, RED = 0xFFDC2828, GREEN = 0xFF28DC50, GOLD = 0xFFE6B428;

    private File game() throws IOException {
        return tmp.newFolder("game");
    }

    private File save() throws IOException {
        return tmp.newFolder("save");
    }

    @Test
    public void titleImageSectionFirst() throws IOException {
        File g = game();
        TestSupport.write(new File(g, "default.xbe"), TestSupport.xbeWithImage(TID, "Sample", TestSupport.xprSolid(8, 8, BLUE)));
        TestSupport.write(new File(g, "TitleImage.xbx"), TestSupport.xprSolid(4, 4, RED));
        GameArt.Found f = GameArt.titleImage(g, save(), "4D530004");
        assertNotNull(f);
        assertEquals("xbe:$$XTIMAGE", f.source);
        assertEquals(8, f.w);
        assertEquals(BLUE, f.argb[0]);
    }

    @Test
    public void saveImageSectionSecond() throws IOException {
        File g = game();
        TestSupport.write(new File(g, "default.xbe"),
                          TestSupport.xbeWithSection(TID, "Sample", GameArt.SAVE_SECTION, TestSupport.xprSolid(4, 4, GREEN)));
        TestSupport.write(new File(g, "TitleImage.xbx"), TestSupport.xprSolid(4, 4, RED));
        GameArt.Found f = GameArt.titleImage(g, save(), "4D530004");
        assertEquals("xbe:$$XSIMAGE", f.source);
        assertEquals(GREEN, f.argb[0]);
        assertEquals("xbe:$$XSIMAGE", GameArt.saveImage(g, null, null).source);
    }

    @Test
    public void looseFilesAnyCaseRootFirstThenDownToDepthThree() throws IOException {
        File g = game();
        TestSupport.write(new File(g, "default.xbe"), TestSupport.xbe(TID, "Sample"));
        TestSupport.write(new File(g, "a/b/c/TITLEIMAGE.XBX"), TestSupport.xprSolid(4, 4, BLUE));
        GameArt.Found f = GameArt.titleImage(g, save(), "4D530004");
        assertEquals("file:a/b/c/TITLEIMAGE.XBX", f.source);
        TestSupport.write(new File(g, "Media/titleimage.xbx"), TestSupport.xprSolid(4, 4, RED));
        assertEquals("file:Media/titleimage.xbx", GameArt.titleImage(g, null, null).source);
        TestSupport.write(new File(g, "TitleImage.xbx"), TestSupport.xprSolid(4, 4, GREEN));
        assertEquals("file:TitleImage.xbx", GameArt.titleImage(g, null, null).source);
    }

    @Test
    public void depthFourIsNotWalked() throws IOException {
        File g = game();
        TestSupport.write(new File(g, "a/b/c/d/TitleImage.xbx"), TestSupport.xprSolid(4, 4, BLUE));
        assertNull(GameArt.titleImage(g, save(), "4D530004"));
    }

    @Test
    public void titleImageFileBeforeSaveImageFile() throws IOException {
        File g = game();
        TestSupport.write(new File(g, "SaveImage.xbx"), TestSupport.xprSolid(4, 4, GOLD));
        TestSupport.write(new File(g, "Data/Misc/TitleImage.xbx"), TestSupport.xprSolid(4, 4, BLUE));
        assertEquals("file:Data/Misc/TitleImage.xbx", GameArt.titleImage(g, null, null).source);
        new File(g, "Data/Misc/TitleImage.xbx").delete();
        assertEquals("file:SaveImage.xbx", GameArt.titleImage(g, null, null).source);
    }

    @Test
    public void udataTitleImageLast() throws IOException {
        File g = game(), s = save();
        TestSupport.write(new File(g, "default.xbe"), TestSupport.xbe(TID, "Sample"));
        TestSupport.write(new File(s, "UDATA/4d530004/TitleImage.xbx"), TestSupport.xprSolid(4, 4, GOLD));
        GameArt.Found f = GameArt.titleImage(g, s, "4D530004");
        assertEquals("save:UDATA/4d530004/TitleImage.xbx", f.source);
        assertEquals(GOLD, f.argb[5]);
        assertNull(GameArt.titleImage(g, s, "4D530005"));
    }

    @Test
    public void walkIsBounded() throws IOException {
        File g = game();
        for (int i = 0; i < GameArt.MAX_ENTRIES + 10; i++)
            TestSupport.write(new File(g, String.format("f%05d", i)), new byte[0]);
        TestSupport.write(new File(g, "zz/TitleImage.xbx"), TestSupport.xprSolid(4, 4, BLUE));
        assertNull(GameArt.titleImage(g, null, null));
    }

    @Test
    public void onlyDefaultXbeIsASource() throws IOException {
        File g = game();
        TestSupport.write(new File(g, "default.xbe"), TestSupport.xbe(TID, "Sample"));
        TestSupport.write(new File(g, "dashupdate.xbe"), TestSupport.xbeWithImage(TID, "Dash", TestSupport.xprSolid(4, 4, RED)));
        assertNull(GameArt.titleImage(g, save(), "4D530004"));
    }

    @Test
    public void unreadableFilesAreSkipped() throws IOException {
        File g = game();
        TestSupport.write(new File(g, "TitleImage.xbx"), new byte[] { 'X', 'P', 'R', '0', 1, 2, 3 });
        TestSupport.write(new File(g, "SaveImage.xbx"), TestSupport.xprSolid(4, 4, GOLD));
        assertNull(GameArt.loose(g, GameArt.TITLE_FILE));
        assertEquals("file:SaveImage.xbx", GameArt.titleImage(g, null, null).source);
        assertNull(GameArt.titleImage(null, null, null));
    }
}
