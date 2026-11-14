package fr.adserver.xrecomp;

import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertTrue;

import org.junit.Test;

import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.Map;

/**
 * The « Calm » look's contrast promises (docs/android/app-shell.md « Look and motion »), for the stock colours and
 * for game themes of every hue: the focus plate (ACCENT at Widgets.Glide.PLATE_ALPHA over the backdrop) keeps the
 * focused row's label, value and description readable, the outline and the Play button stand out. Pure JVM: the
 * plate is composited here the way Canvas does it.
 */
public class CalmContrastTest {
    private static int[] tokens(String name, Map<String, int[]> out, int[] colors) {
        out.put(name, colors);
        return colors;
    }

    private static Map<String, int[]> palettes() {
        Map<String, int[]> m = new LinkedHashMap<>();
        tokens("stock", m, GamePalette.STOCK);
        for (int hue = 0; hue < 360; hue += 15) {
            GamePalette.Palette p = GamePalette.build(new GamePalette.Analysis(0, new ArrayList<>(), 1), hue);
            assertNotNull(p);
            tokens("hue " + hue, m, p.colors);
        }
        for (String img : new String[] { "split", "flame", "stripes" }) {
            GamePalette.Palette p = GamePalette.build(GamePalette.analysePixels(GamePaletteTest.image(img)), Double.NaN);
            if (p != null)
                tokens(img, m, p.colors);
        }
        return m;
    }

    /** fg at alpha a over the opaque bg. */
    private static int over(int fg, float a, int bg) {
        int out = 0xFF000000;
        for (int sh = 16; sh >= 0; sh -= 8)
            out |= Math.round((fg >> sh & 0xff) * a + (bg >> sh & 0xff) * (1 - a)) << sh;
        return out;
    }

    private static void atLeast(String what, double min, int fg, int bg) {
        double c = GamePalette.contrast(fg, bg);
        assertTrue(what + ": " + String.format("%.2f", c) + " < " + min, c >= min);
    }

    @Test
    public void focusPlateKeepsTextReadable() {
        for (Map.Entry<String, int[]> e : palettes().entrySet()) {
            int[] c = e.getValue();
            for (int bg : new int[] { c[GamePalette.BG_TOP], c[GamePalette.BG_BOTTOM] }) {
                int plate = over(c[GamePalette.ACCENT], Widgets.Glide.PLATE_ALPHA, bg);
                String n = e.getKey() + String.format(" on %06x", bg & 0xFFFFFF);
                atLeast(n + ": TEXT_DIM on the backdrop", 4.5, c[GamePalette.TEXT_DIM], bg);
                atLeast(n + ": TEXT_DIM on the focus plate", 4.5, c[GamePalette.TEXT_DIM], plate);
                atLeast(n + ": TEXT_BRIGHT on the focus plate", 7.0, c[GamePalette.TEXT_BRIGHT], plate);
                atLeast(n + ": ACCENT (a focused value) on the plate", 4.5, c[GamePalette.ACCENT], plate);
                atLeast(n + ": the focus outline on the backdrop", 3.0, c[GamePalette.ACCENT], bg);
            }
            atLeast(e.getKey() + ": ink on the focused Play", 4.5, c[GamePalette.GLYPH_INK], c[GamePalette.ACCENT]);
            atLeast(e.getKey() + ": ACCENT on ACCENT_FACE (Play unfocused)", 4.5, c[GamePalette.ACCENT],
                    c[GamePalette.ACCENT_FACE]);
        }
    }
}
