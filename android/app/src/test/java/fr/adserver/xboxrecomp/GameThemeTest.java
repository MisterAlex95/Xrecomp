package fr.adserver.xboxrecomp;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotEquals;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;

import org.junit.Before;
import org.junit.Rule;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.RuntimeEnvironment;
import org.robolectric.annotation.Config;
import org.robolectric.annotation.GraphicsMode;

import java.io.File;
import java.nio.file.Files;

/** GameTheme: the per-title cache in files/art, its invalidation, the player's modes, clearing. Synthetic art only. */
@RunWith(RobolectricTestRunner.class)
@Config(sdk = 34)
@GraphicsMode(GraphicsMode.Mode.NATIVE)
public class GameThemeTest {
    @Rule
    public final TestSupport.StockTheme stockTheme = new TestSupport.StockTheme();

    private static final String TID = "4D530004";
    private static final int BLUE = 0xFF2850DC;
    private Context c;
    private File game, xbe;

    @Before
    public void setUp() throws Exception {
        c = RuntimeEnvironment.getApplication();
        game = new File(c.getFilesDir(), "test-game");
        xbe = new File(game, "default.xbe");
        TestSupport.write(xbe, TestSupport.xbeWithImage(0x4D530004, "Sample", TestSupport.xprSolid(128, 128, BLUE)));
    }

    @Test
    public void ensureBakesAndCaches() {
        GameTheme t = GameTheme.ensure(c, game, TID, "Sample");
        assertNotNull(t);
        assertEquals("xbe:$$XTIMAGE", t.source);
        assertEquals(0xFFA7C2FF, t.colors.get(GamePalette.ACCENT));
        assertEquals(265.9, t.accentHue, 0.05);
        assertEquals(GameTheme.Mode.FULL, t.mode);
        assertNotNull(t.heroFile);
        Bitmap hero = BitmapFactory.decodeFile(t.heroFile.getPath());
        assertEquals(48, hero.getWidth());
        assertTrue(t.heroFile.length() < 8192);
        assertTrue(GameTheme.themeFile(c, TID).isFile());
        assertTrue(TitleArt.cacheFile(c.getFilesDir(), TID).isFile());
        assertTrue(t.logLine(), t.logLine().matches(
            "launcher: theme 4D530004: source xbe:\\$\\$XTIMAGE, accent #A7C2FF, hues 265\\.9/265\\.9/-, mode full"));

        GameTheme back = GameTheme.load(c, game, TID);
        assertNotNull(back);
        assertEquals(t.colors, back.colors);
        assertEquals(t.source, back.source);
        assertEquals(t.heroFile, back.heroFile);
        assertTrue(Double.isNaN(back.secondHue));
    }

    @Test
    public void loadNeverDecodes() {
        assertNull(GameTheme.load(c, game, TID));
        assertFalse(GameTheme.themeFile(c, TID).exists());
        assertFalse(TitleArt.cacheFile(c.getFilesDir(), TID).exists());
        assertNull(GameTheme.load(c, game, null));
        assertNull(GameTheme.ensure(c, game, null, null));
    }

    @Test
    public void staleOnVersionOrFingerprint() throws Exception {
        GameTheme.ensure(c, game, TID, "");
        File f = GameTheme.themeFile(c, TID);
        String text = new String(Files.readAllBytes(f.toPath()), "UTF-8");
        TestSupport.write(f, text.replace("v=" + GamePalette.VERSION, "v=0"));
        assertNull(GameTheme.load(c, game, TID));
        assertNotNull(GameTheme.ensure(c, game, TID, ""));   // computed again
        assertNotNull(GameTheme.load(c, game, TID));

        xbe.setLastModified(xbe.lastModified() - 60000);
        assertNull(GameTheme.load(c, game, TID));
        GameTheme t = GameTheme.ensure(c, game, TID, "");
        assertNotNull(t);
        assertNotNull(GameTheme.load(c, game, TID));

        TestSupport.write(f, "v=" + GamePalette.VERSION + "\nxbe=" + GameTheme.fingerprint(xbe) + "\nBG_TOP=zz\n");
        assertNull(GameTheme.load(c, game, TID));            // corrupt: computed again by ensure
        assertNotNull(GameTheme.ensure(c, game, TID, ""));
    }

    @Test
    public void modes() {
        assertEquals(GameTheme.Mode.FULL, GameTheme.mode(c, TID));
        GameTheme.ensure(c, game, TID, "");
        assertTrue(GameTheme.setMode(c, TID, GameTheme.Mode.COLORS));
        GameTheme t = GameTheme.load(c, game, TID);
        assertEquals(GameTheme.Mode.COLORS, t.mode);
        assertNull(t.heroFile);
        assertNotEquals(Theme.Colors.STOCK, t.colors);
        assertTrue(GameTheme.setMode(c, "4d530004", GameTheme.Mode.OFF));
        assertEquals(GameTheme.Mode.OFF, GameTheme.mode(c, TID));
        assertNull(GameTheme.load(c, game, TID));
        assertNull(GameTheme.ensure(c, game, TID, ""));
        assertTrue(GameTheme.themeFile(c, TID).isFile());   // still cached for when it is back on
        assertTrue(GameTheme.setMode(c, TID, GameTheme.Mode.FULL));
        assertNotNull(GameTheme.load(c, game, TID).heroFile);
        assertEquals(GameTheme.Mode.FULL, GameTheme.mode(c, "4D530005"));
        assertFalse(GameTheme.setMode(c, null, GameTheme.Mode.OFF));
    }

    @Test
    public void clearDeletesTheCache() throws Exception {
        GameTheme.ensure(c, game, TID, "");
        TestSupport.write(new File(GameTheme.artDir(c), TID + ".last.jpg"), new byte[] { 1 });
        GameTheme.clear(c, TID);
        File[] left = GameTheme.artDir(c).listFiles();
        assertEquals(0, left == null ? 0 : left.length);
        GameTheme.clear(c, null);
    }

    @Test
    public void noImageFallsBackToTheTitleHue() throws Exception {
        TestSupport.write(xbe, TestSupport.xbe(0x4D530004, "Sample"));
        GameTheme t = GameTheme.ensure(c, game, TID, "");
        assertEquals("none", t.source);
        assertNull(t.heroFile);
        assertEquals(TitleArt.titleHue(TID), t.accentHue, 18.001);   // pushed off ERROR's hue at most
    }

    @Test
    public void greyImageKeepsItsHeroWithTheTitleHue() throws Exception {
        TestSupport.write(xbe, TestSupport.xbeWithImage(0x4D530004, "Sample", TestSupport.xprSolid(64, 64, 0xFF5A5A5C)));
        GameTheme t = GameTheme.ensure(c, game, TID, "");
        assertEquals("xbe:$$XTIMAGE", t.source);
        assertNotNull(t.heroFile);
        assertEquals(TitleArt.titleHue(TID), t.backdropHue, 1e-9);
        assertEquals(t.backdropHue, t.accentHue, 18.001);
    }

    @Test
    public void applyAndReset() {
        GameTheme t = GameTheme.ensure(c, game, TID, "");
        Theme.apply(t.colors);
        assertEquals(0xFFA7C2FF, Theme.ACCENT);
        assertEquals(GamePalette.STOCK[GamePalette.OK], Theme.OK);
        assertFalse(Theme.isStock());
        Theme.reset();
        assertTrue(Theme.isStock());
        assertEquals(0xFF4FE38C, Theme.ACCENT);
    }
}
