package fr.adserver.xrecomp;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;

import java.io.BufferedReader;
import java.io.IOException;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

import org.junit.Test;

/**
 * GamePalette (plain JUnit, no android.*): the port of tools/android/theme_preview/palette.py, checked against its
 * golden vectors (src/test/resources/theme/golden*.txt, made by golden.py), plus the contrast and hero guarantees.
 * Synthetic pixels only, no game art.
 */
public class GamePaletteTest {
    private static int rgb(int r, int g, int b) {
        return 0xFF000000 | r << 16 | g << 8 | b;
    }

    private static int[] solid(int argb, int n) {
        int[] px = new int[n];
        Arrays.fill(px, argb);
        return px;
    }

    private static GamePalette.Palette solidPalette(int r, int g, int b) {
        return GamePalette.build(GamePalette.analysePixels(solid(rgb(r, g, b), 64)), Double.NaN);
    }

    private static GamePalette.Palette fallback(double hue) {
        return GamePalette.build(new GamePalette.Analysis(0, new ArrayList<>(), 1), hue);
    }

    private static int token(String name) {
        int i = Arrays.asList(GamePalette.NAMES).indexOf(name);
        assertTrue("unknown token " + name, i >= 0);
        return i;
    }

    private static void assertNear(String what, int want, int got) {
        for (int s = 16; s >= 0; s -= 8) {
            int d = Math.abs((want >> s & 0xff) - (got >> s & 0xff));
            assertTrue(what + ": want " + GamePalette.hex(want) + ", got " + GamePalette.hex(got), d <= 1);
        }
    }

    private static List<String> lines(String resource) throws IOException {
        InputStream in = GamePaletteTest.class.getClassLoader().getResourceAsStream(resource);
        assertNotNull(resource, in);
        List<String> out = new ArrayList<>();
        try (BufferedReader r = new BufferedReader(new InputStreamReader(in, StandardCharsets.UTF_8))) {
            for (String l; (l = r.readLine()) != null;) {
                if (!l.trim().isEmpty())
                    out.add(l);
            }
        }
        return out;
    }

    // ---- Colour science ---------------------------------------------------------------------------------------------

    @Test
    public void oklchRoundTrip() {
        for (int r = 0; r < 256; r += 17) {
            for (int g = 0; g < 256; g += 17) {
                for (int b = 0; b < 256; b += 17) {
                    double[] lch = GamePalette.oklch(rgb(r, g, b));
                    assertNear("round trip", rgb(r, g, b), GamePalette.fromOklch(lch[0], lch[1], lch[2]));
                }
            }
        }
    }

    @Test
    public void outOfGamutChromaIsReduced() {
        int c = GamePalette.fromOklch(0.9, 0.4, 265);
        double[] lch = GamePalette.oklch(c);
        assertEquals(0.9, lch[0], 0.01);
        assertTrue(lch[1] < 0.4);
    }

    @Test
    public void wcag() {
        assertEquals(21.0, GamePalette.contrast(0xFFFFFFFF, 0xFF000000), 1e-9);
        assertEquals(1.0, GamePalette.contrast(0xFF808080, 0xFF808080), 1e-9);
    }

    // ---- Design examples --------------------------------------------------------------------------------------------

    @Test
    public void blueImageGivesPeriwinkle() {
        GamePalette.Palette p = solidPalette(40, 80, 220);
        assertEquals(265.9, p.accentHue, 0.05);
        assertEquals(0xFFA7C2FF, p.colors[GamePalette.ACCENT]);
        assertEquals(0xFF4864A9, p.colors[GamePalette.ACCENT_DEEP]);
        assertEquals(0xFF24304E, p.colors[GamePalette.ACCENT_FACE]);
        assertEquals(0xFF0D1016, p.colors[GamePalette.BG_TOP]);
        assertEquals(0xE6121620, p.colors[GamePalette.PANEL]);
        assertEquals(0x66283040, p.colors[GamePalette.LINE_SOFT]);
        assertEquals(p.colors[GamePalette.BG_TOP], p.colors[GamePalette.GLYPH_INK]);
        assertFalse(p.fallback);
        assertTrue(Double.isNaN(p.secondHue));
    }

    @Test
    public void orangeImageKeepsItsOrange() {
        GamePalette.Palette p = solidPalette(240, 130, 20);
        assertEquals(56.4, p.accentHue, 0.05);
        assertEquals(0xFFFFAE72, p.colors[GamePalette.ACCENT]);
    }

    @Test
    public void redAccentIsPushedAwayFromError() {
        GamePalette.Palette p = solidPalette(230, 30, 30);
        assertEquals(46.0, p.accentHue, 0.05);
        assertEquals(0xFFFFAC85, p.colors[GamePalette.ACCENT]);
        assertTrue(GamePalette.hueDistance(p.accentHue, GamePalette.ERROR_HUE) >= GamePalette.ERROR_AVOID_DEG - 1e-9);
    }

    @Test
    public void warnAvoidanceIsOffAsDesignedAndWorksWhenEnabled() {
        assertEquals(0, GamePalette.WARN_AVOID_DEG, 0);
        assertEquals(80.0, GamePalette.avoid(80.0, GamePalette.WARN_HUE, 0), 0);
        double pushed = GamePalette.avoid(78.0, GamePalette.WARN_HUE, 15);
        assertEquals(15, GamePalette.hueDistance(pushed, GamePalette.WARN_HUE), 1e-9);
        assertEquals(120.0, GamePalette.avoid(120.0, GamePalette.WARN_HUE, 15), 0);
    }

    @Test
    public void semanticColoursNeverChange() {
        for (double h = 0; h < 360; h += 7) {
            int[] t = fallback(h).colors;
            for (int k : new int[] { GamePalette.OK, GamePalette.WARN, GamePalette.ERROR, GamePalette.TEXT,
                                     GamePalette.TEXT_BRIGHT, GamePalette.GLYPH_FACE })
                assertEquals(GamePalette.NAMES[k], GamePalette.STOCK[k], t[k]);
        }
    }

    @Test
    public void noColourMeansNoTheme() {
        assertNull(solidPalette(128, 128, 128));
        assertNull(solidPalette(255, 255, 255));
        assertNull(solidPalette(8, 10, 40));
        assertNull(GamePalette.build(GamePalette.analysePixels(new int[0]), Double.NaN));
        assertNull(GamePalette.build(GamePalette.analysePixels(solid(0x00FF0000, 64)), Double.NaN));   // transparent
        GamePalette.Palette p = GamePalette.build(GamePalette.analysePixels(solid(rgb(128, 128, 128), 64)), 212);
        assertNotNull(p);
        assertTrue(p.fallback);
        assertEquals(212, p.accentHue, 1e-9);
    }

    @Test
    public void analyseDownsamplesBigImages() {
        int[] px = solid(rgb(40, 80, 220), 256 * 128);
        GamePalette.Analysis a = GamePalette.analyse(px, 256, 128);
        assertEquals(64 * 32, a.n);
        assertEquals(1.0, a.chromatic, 1e-9);
        assertEquals(265.9, a.peaks.get(0).hue, 0.05);
    }

    // ---- The 360-degree sweep ---------------------------------------------------------------------------------------

    @Test
    public void everyHueMeetsEveryContrastPair() {
        for (int h = 0; h < 360; h++) {
            int[] t = fallback(h).colors;
            for (GamePalette.Check c : GamePalette.checks(t))
                assertTrue("hue " + h + ": " + c.name + " " + c.ratio + " < " + c.min, c.ok());
        }
        for (GamePalette.Check c : GamePalette.checks(GamePalette.STOCK))
            assertTrue("stock: " + c.name, c.ok());
    }

    // ---- Golden vectors from palette.py -----------------------------------------------------------------------------

    /** The design's sweep: « hue H -> accent hue A ACCENT #... NAME #... ». */
    @Test
    public void goldenSweep() throws IOException {
        List<String> ls = lines("theme/golden-sweep.txt");
        assertEquals(12, ls.size());
        for (String l : ls) {
            String[] w = l.trim().split("\\s+");
            double hue = Double.parseDouble(w[1]);
            GamePalette.Palette p = fallback(hue);
            assertEquals(l, Double.parseDouble(w[5]), p.accentHue, 0.05);
            for (int i = 6; i + 1 < w.length; i += 2)
                assertNear(l + " " + w[i], 0xFF000000 | Integer.parseInt(w[i + 1].substring(1), 16),
                           p.colors[token(w[i])]);
        }
    }

    @Test
    public void goldenFull() throws IOException {
        List<String> ls = lines("theme/golden.txt");
        assertTrue(ls.size() > 30);
        for (String l : ls) {
            String[] parts = l.split("\\|");
            String[] c = parts[0].trim().split(" ");
            GamePalette.Palette p;
            switch (c[0]) {
            case "fallback":
                p = fallback(Double.parseDouble(c[1]));
                break;
            case "solid": {
                String[] v = c[1].split(",");
                p = solidPalette(Integer.parseInt(v[0]), Integer.parseInt(v[1]), Integer.parseInt(v[2]));
                break;
            }
            case "image":
                p = GamePalette.build(GamePalette.analysePixels(image(c[1])), Double.NaN);
                break;
            default:
                throw new AssertionError(l);
            }
            if (parts[1].trim().equals("none")) {
                assertNull(l, p);
                continue;
            }
            assertNotNull(l, p);
            String[] hues = parts[1].trim().substring("hues ".length()).split("/");
            assertEquals(l, Double.parseDouble(hues[0]), p.accentHue, 0.05);
            assertEquals(l, Double.parseDouble(hues[1]), p.backdropHue, 0.05);
            if (hues[2].equals("-"))
                assertTrue(l, Double.isNaN(p.secondHue));
            else
                assertEquals(l, Double.parseDouble(hues[2]), p.secondHue, 0.05);
            for (String kv : parts[2].trim().split(" ")) {
                String[] e = kv.split("=");
                assertNear(l + " " + e[0], 0xFF000000 | Integer.parseInt(e[1], 16), p.colors[token(e[0])]);
            }
        }
    }

    /** The synthetic pictures of golden.py (same integer formulas). */
    static int[] image(String name) {
        int[] px = new int[64 * 64];
        for (int y = 0; y < 64; y++) {
            for (int x = 0; x < 64; x++) {
                int p;
                switch (name) {
                case "split":
                    p = y < 8 ? rgb(30, 30, 32) : x < 45 ? rgb(40, 80, 220) : rgb(220, 60, 150);
                    break;
                case "flame": {
                    int d = Math.abs(2 * y - 80 + (x - 32)) * 100 / 40;
                    int k = Math.max(0, 100 - d);
                    p = rgb(8 + 247 * k / 100, 8 + 150 * k * k / 10000, 10 + 30 * k * k * k / 1000000);
                    break;
                }
                case "stripes": {
                    int a = x % 8 == 7 ? 0 : 255;
                    p = (x % 16 < 4 ? rgb(230, 180, 40) : rgb(20, 170, 160)) & 0x00FFFFFF | a << 24;
                    break;
                }
                default:
                    throw new AssertionError(name);
                }
                px[y * 64 + x] = p;
            }
        }
        return px;
    }

    // ---- Hero --------------------------------------------------------------------------------------------------------

    @Test
    public void heroIsSmallOpaqueAndCapped() {
        int[] src = new int[128 * 128];
        for (int y = 0; y < 128; y++) {
            for (int x = 0; x < 128; x++)
                src[y * 128 + x] = (x / 16 + y / 16) % 2 == 0 ? 0xFFFFFFFF : rgb(40, 80, 220);
        }
        src[0] = 0;   // a transparent pixel
        GamePalette.Image h = GamePalette.bakeHero(src, 128, 128);
        assertEquals(48, h.w);
        assertEquals(48, h.h);
        double max = 0;
        for (int p : h.px) {
            assertEquals(0xFF, p >>> 24);
            max = Math.max(max, GamePalette.luminance(p));
        }
        assertTrue("brightest " + max, max <= GamePalette.HERO_LUM_CAP);
        assertTrue("not all black: " + max, max > GamePalette.HERO_LUM_CAP * 0.5);
    }

    @Test
    public void heroKeepsAspectAndDarkImagesStayDark() {
        int[] src = solid(rgb(20, 4, 4), 64 * 32);
        GamePalette.Image h = GamePalette.bakeHero(src, 64, 32);
        assertEquals(48, h.w);
        assertEquals(24, h.h);
        for (int p : h.px)
            assertEquals(GamePalette.luminance(rgb(20, 4, 4)), GamePalette.luminance(p), 1e-3);   // no gain up
        for (int cap10 = 5; cap10 <= 30; cap10 += 5) {
            double cap = cap10 / 1000.0;
            GamePalette.Image k = GamePalette.bakeHero(solid(0xFFFFFFFF, 32 * 32), 32, 32, 48, 3, cap);
            for (int p : k.px)
                assertTrue(GamePalette.luminance(p) <= cap);
        }
    }

    @Test
    public void boxResizeAverages() {
        int[] px = { rgb(0, 0, 0), rgb(200, 100, 50), rgb(0, 0, 0), rgb(200, 100, 50) };
        int[] o = GamePalette.boxResize(px, 2, 2, 1, 1);
        assertEquals(rgb(100, 50, 25), o[0]);
    }
}
