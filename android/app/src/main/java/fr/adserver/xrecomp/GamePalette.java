package fr.adserver.xrecomp;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.List;

/**
 * The per-game launcher theme's colours (docs/android/app-shell.md « Game theme »): the launcher's tokens derived from
 * the game's title image, and the small blurred, luminance-capped « hero » backdrop baked from it. Pure Java on ARGB
 * int arrays (no android.*), so plain JUnit tests it. A 1:1 port of tools/android/theme_preview/palette.py (the
 * reference the golden vectors come from): change both together, and bump VERSION when the output changes (the
 * launcher's cache, GameTheme, is keyed on it).
 *
 * Colour science: OKLab / OKLCH (Björn Ottosson, public domain), WCAG 2.x relative luminance and contrast. The
 * semantic colours (OK, WARN, ERROR) and the text colours never change: only the brand accent, its glows and the
 * neutrals' hue follow the game.
 */
final class GamePalette {
    private GamePalette() {}

    /** The output's version: part of the cache key (GameTheme), bump when any token or the hero changes. */
    static final int VERSION = 1;

    // ---- Tokens: the same names and order as Theme's colours -------------------------------------------------------
    static final int BG_TOP = 0, BG_BOTTOM = 1, PANEL = 2, PANEL_HI = 3, LINE = 4, LINE_SOFT = 5, TEXT = 6,
                     TEXT_BRIGHT = 7, TEXT_DIM = 8, TEXT_FAINT = 9, ACCENT = 10, ACCENT_DEEP = 11, ACCENT_FACE = 12,
                     TRACK = 13, OK = 14, WARN = 15, ERROR = 16, GLYPH_FACE = 17, GLYPH_INK = 18, GLOW = 19, GLOW2 = 20,
                     COUNT = 21;
    static final String[] NAMES = { "BG_TOP", "BG_BOTTOM", "PANEL", "PANEL_HI", "LINE", "LINE_SOFT", "TEXT",
                                    "TEXT_BRIGHT", "TEXT_DIM", "TEXT_FAINT", "ACCENT", "ACCENT_DEEP", "ACCENT_FACE",
                                    "TRACK", "OK", "WARN", "ERROR", "GLYPH_FACE", "GLYPH_INK", "GLOW", "GLOW2" };
    /**
     * The stock theme (a deep green-black, one vivid green, warm off-white text), ARGB. PANEL is translucent glass,
     * LINE_SOFT a translucent LINE; the stock glows are the accent.
     */
    static final int[] STOCK = { 0xFF0C110F, 0xFF060807, 0xE6111814, 0xFF18211C, 0xFF27332D, 0x6627332D, 0xFFF3EEE4,
                                 0xFFFFFFFF, 0xFFA9AFA6, 0xFF6C746D, 0xFF4FE38C, 0xFF1F7A48, 0xFF143A26, 0xFF1A241F,
                                 0xFF4FE38C, 0xFFF2BA52, 0xFFFF6F61, 0xFFEDE6D8, 0xFF0C110F, 0xFF4FE38C, 0xFF4FE38C };
    /** The tokens that take the game's backdrop hue at the stock token's lightness. */
    static final int[] NEUTRALS = { BG_TOP, BG_BOTTOM, PANEL, PANEL_HI, LINE, TEXT_DIM, TEXT_FAINT, TRACK };

    // ---- Tuning ------------------------------------------------------------------------------------------------------
    /** The image is analysed at most this big (box-downsampled). */
    static final int ANALYSIS_SIDE = 64;
    /** Under this share of hued pixels the image counts as grey: the caller's fallback hue is used. */
    static final double MIN_CHROMATIC = 0.06;
    /** A hue on under this share of the image is noise. */
    static final double MIN_PEAK_AREA = 0.02;
    /** The accent keeps this far (degrees of OKLCH hue) from ERROR's hue: a red accent would read as an error. */
    static final double ERROR_AVOID_DEG = 18;
    /**
     * The same for WARN's amber (OKLCH hue 80): 0 = off, as designed (a warm game keeps its own orange next to the
     * amber « Needs preparing » chip). Set to 15 to push the accent out of +-15 degrees around WARN's hue, should the
     * two read alike on the device.
     */
    static final double WARN_AVOID_DEG = 0;
    /** The hero backdrop: long side in pixels, blur sigma in pixels, and the cap on any pixel's WCAG luminance. */
    static final int HERO_SIDE = 48;
    static final double HERO_SIGMA = 3.0;
    static final double HERO_LUM_CAP = 0.012;

    private static final int BINS = 36;

    // ---- sRGB <-> OKLab / OKLCH ----------------------------------------------------------------------------------------
    private static final double[] LIN = new double[256];
    static {
        for (int i = 0; i < 256; i++)
            LIN[i] = lin(i);
    }

    private static double lin(int c) {
        double v = c / 255.0;
        return v <= 0.04045 ? v / 12.92 : Math.pow((v + 0.055) / 1.055, 2.4);
    }

    private static double gam(double c) {
        return c <= 0.0031308 ? 12.92 * c : 1.055 * Math.pow(c, 1 / 2.4) - 0.055;
    }

    private static int r(int argb) {
        return argb >> 16 & 0xff;
    }

    private static int g(int argb) {
        return argb >> 8 & 0xff;
    }

    private static int b(int argb) {
        return argb & 0xff;
    }

    /** OKLab (L, a, b) of an opaque sRGB colour (alpha ignored). */
    static double[] oklab(int argb) {
        double rl = LIN[r(argb)], gl = LIN[g(argb)], bl = LIN[b(argb)];
        double l = 0.4122214708 * rl + 0.5363325363 * gl + 0.0514459929 * bl;
        double m = 0.2119034982 * rl + 0.6806995451 * gl + 0.1073969566 * bl;
        double s = 0.0883024619 * rl + 0.2817188376 * gl + 0.6299787005 * bl;
        l = Math.cbrt(l);
        m = Math.cbrt(m);
        s = Math.cbrt(s);
        return new double[] { 0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s,
                              1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s,
                              0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s };
    }

    private static double[] oklabToLinear(double lL, double a, double bb) {
        double l = lL + 0.3963377774 * a + 0.2158037573 * bb;
        double m = lL - 0.1055613458 * a - 0.0638541728 * bb;
        double s = lL - 0.0894841775 * a - 1.2914855480 * bb;
        l = l * l * l;
        m = m * m * m;
        s = s * s * s;
        return new double[] { 4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s,
                              -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s,
                              -0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s };
    }

    /** OKLCH (L, C, h in degrees 0..360) of an sRGB colour. */
    static double[] oklch(int argb) {
        double[] lab = oklab(argb);
        return new double[] { lab[0], Math.hypot(lab[1], lab[2]), mod360(Math.toDegrees(Math.atan2(lab[2], lab[1]))) };
    }

    static boolean inGamut(double lL, double c, double h) {
        double hr = Math.toRadians(h);
        double[] rgb = oklabToLinear(lL, c * Math.cos(hr), c * Math.sin(hr));
        double e = 1e-4;
        for (double v : rgb) {
            if (v < -e || v > 1 + e)
                return false;
        }
        return true;
    }

    /** The opaque sRGB colour for OKLCH (L, C, h), chroma reduced (bisection) until it is in the sRGB gamut. */
    static int fromOklch(double lL, double c, double h) {
        if (!inGamut(lL, c, h)) {
            double lo = 0, hi = c;
            for (int i = 0; i < 16; i++) {
                double mid = (lo + hi) / 2;
                if (inGamut(lL, mid, h))
                    lo = mid;
                else
                    hi = mid;
            }
            c = lo;
        }
        double hr = Math.toRadians(h);
        double[] rgb = oklabToLinear(lL, c * Math.cos(hr), c * Math.sin(hr));
        int out = 0xFF000000;
        for (int i = 0; i < 3; i++)
            out |= channel(rgb[i]) << (16 - 8 * i);
        return out;
    }

    /** A linear channel value as an 8-bit sRGB channel; rounds half to even, as the Python reference does. */
    private static int channel(double v) {
        long q = (long)Math.rint(255 * gam(Math.max(0.0, Math.min(1.0, v))));
        return (int)Math.max(0, Math.min(255, q));
    }

    // ---- WCAG 2.x ----------------------------------------------------------------------------------------------------

    /** WCAG relative luminance of an sRGB colour (alpha ignored). */
    static double luminance(int argb) {
        return 0.2126 * LIN[r(argb)] + 0.7152 * LIN[g(argb)] + 0.0722 * LIN[b(argb)];
    }

    static double contrast(int x, int y) {
        double a = luminance(x), b = luminance(y);
        return (Math.max(a, b) + 0.05) / (Math.min(a, b) + 0.05);
    }

    /** fg at opacity a over bg, opaque. */
    static int over(int fg, double a, int bg) {
        int out = 0xFF000000;
        for (int s = 16; s >= 0; s -= 8) {
            double v = (fg >> s & 0xff) * a + (bg >> s & 0xff) * (1 - a);
            out |= (int)Math.rint(v) << s;
        }
        return out;
    }

    static String hex(int argb) {
        return String.format("#%06X", argb & 0xFFFFFF);
    }

    static double hueDistance(double a, double b) {
        double d = Math.abs(a - b) % 360;
        return Math.min(d, 360 - d);
    }

    static double mod360(double d) {
        double m = d % 360;
        return m < 0 ? m + 360 : m;
    }

    // ---- Analysis ----------------------------------------------------------------------------------------------------

    /** A hue the image carries: its share of the vivid weight and of the area, and its pixels' mean chroma. */
    static final class Peak {
        final double hue, vivid, area, meanChroma;

        Peak(double hue, double vivid, double area, double meanChroma) {
            this.hue = hue;
            this.vivid = vivid;
            this.area = area;
            this.meanChroma = meanChroma;
        }
    }

    static final class Analysis {
        /** The share of (opaque) pixels that carry a hue. */
        final double chromatic;
        /** Most vivid first. */
        final List<Peak> peaks;
        /** The opaque pixels counted. */
        final int n;

        Analysis(double chromatic, List<Peak> peaks, int n) {
            this.chromatic = chromatic;
            this.peaks = peaks;
            this.n = n;
        }
    }

    /** analysePixels() of the image box-downsampled to at most ANALYSIS_SIDE on its long side. */
    static Analysis analyse(int[] argb, int w, int h) {
        int[] wh = { w, h };
        int[] px = argb;
        if (Math.max(w, h) > ANALYSIS_SIDE) {
            double s = (double)ANALYSIS_SIDE / Math.max(w, h);
            wh[0] = Math.max(1, (int)Math.round(w * s));
            wh[1] = Math.max(1, (int)Math.round(h * s));
            px = boxResize(argb, w, h, wh[0], wh[1]);
        }
        return analysePixels(px);
    }

    /**
     * The image's hues: pixels with alpha under 128 are skipped; greys (C < 0.04), near-black (L < 0.18) and
     * near-white (L > 0.96) count but carry no hue. Two 36-bin circular hue histograms, « vivid » (weight chroma
     * squared, dark pixels less) and « area » (count), smoothed 1-2-1; their local maxima are the peaks, each owning
     * its bin and both neighbours, its hue the weighted circular mean of those.
     */
    static Analysis analysePixels(int[] px) {
        double[] vivid = new double[BINS], area = new double[BINS], sx = new double[BINS], sy = new double[BINS],
                 sc = new double[BINS];
        int n = 0, chrom = 0;
        for (int p : px) {
            if ((p >>> 24) < 128)
                continue;
            n++;
            double[] lch = oklch(p);
            double lL = lch[0], c = lch[1], h = lch[2];
            if (c < 0.04 || lL < 0.18 || lL > 0.96)
                continue;
            chrom++;
            double wt = c * c * (0.4 + 0.6 * Math.min(1.0, lL / 0.6));
            int i = (int)(h / (360.0 / BINS)) % BINS;
            vivid[i] += wt;
            area[i] += 1;
            sc[i] += c;
            sx[i] += Math.cos(Math.toRadians(h)) * wt;
            sy[i] += Math.sin(Math.toRadians(h)) * wt;
        }
        if (n == 0)
            return new Analysis(0, new ArrayList<>(), 0);
        double[] vs = smooth(vivid);
        double vivTotal = 0;
        for (double v : vivid)
            vivTotal += v;
        if (vivTotal == 0)
            vivTotal = 1;
        List<Peak> peaks = new ArrayList<>();
        for (int i = 0; i < BINS; i++) {
            int prev = (i + BINS - 1) % BINS, next = (i + 1) % BINS;
            if (vs[i] > 0 && vs[i] >= vs[prev] && vs[i] > vs[next]) {
                int[] ks = { prev, i, next };
                double x = 0, y = 0, v = 0, cnt = 0, csum = 0;
                for (int k : ks) {
                    x += sx[k];
                    y += sy[k];
                    v += vivid[k];
                    cnt += area[k];
                    csum += sc[k];
                }
                double hue = (x != 0 || y != 0) ? mod360(Math.toDegrees(Math.atan2(y, x))) : (i + 0.5) * 360.0 / BINS;
                if (cnt == 0)
                    cnt = 1;
                peaks.add(new Peak(hue, v / vivTotal, cnt / n, csum / cnt));
            }
        }
        Collections.sort(peaks, (a, bb) -> Double.compare(bb.vivid, a.vivid));   // stable, as Python's sort
        return new Analysis((double)chrom / n, peaks, n);
    }

    private static double[] smooth(double[] v) {
        double[] o = new double[BINS];
        for (int i = 0; i < BINS; i++)
            o[i] = (v[(i + BINS - 1) % BINS] + 2 * v[i] + v[(i + 1) % BINS]) / 4;
        return o;
    }

    // ---- Tokens ------------------------------------------------------------------------------------------------------

    static final double ERROR_HUE = oklch(STOCK[ERROR])[2];
    static final double WARN_HUE = oklch(STOCK[WARN])[2];

    /** A game's theme: every token (ARGB, NAMES order) and the hues it came from. */
    static final class Palette {
        final int[] colors;
        /** The accent's hue, the neutrals' (backdrop) hue, the second glow's hue (NaN: none, the backdrop's used). */
        final double accentHue, backdropHue, secondHue;
        /** True when the image had no usable colour and the caller's fallback hue was used. */
        final boolean fallback;

        Palette(int[] colors, double accentHue, double backdropHue, double secondHue, boolean fallback) {
            this.colors = colors;
            this.accentHue = accentHue;
            this.backdropHue = backdropHue;
            this.secondHue = secondHue;
            this.fallback = fallback;
        }
    }

    /** A hue pushed out of +-deg around avoid (deg 0: unchanged). */
    static double avoid(double hue, double avoid, double deg) {
        if (deg <= 0)
            return hue;
        double d = mod360(hue - avoid + 540) - 180;
        if (Math.abs(d) < deg)
            return mod360(avoid + (d >= 0 ? deg : -deg));
        return hue;
    }

    /**
     * The theme from an analysis; null when the image has no usable colour and fallbackHue is NaN. Accent: the most
     * vivid hue; neutrals: the hue covering the most area; second glow: the next peak at least 40 degrees from the
     * accent with a vivid share of 0.08 or more.
     */
    static Palette build(Analysis an, double fallbackHue) {
        List<Peak> peaks = new ArrayList<>();
        for (Peak p : an.peaks) {
            if (p.area >= MIN_PEAK_AREA)
                peaks.add(p);
        }
        double ha, hb, h2 = Double.NaN;
        boolean fallback = false;
        if (an.chromatic < MIN_CHROMATIC || peaks.isEmpty()) {
            if (Double.isNaN(fallbackHue))
                return null;
            ha = hb = mod360(fallbackHue);
            fallback = true;
        } else {
            ha = peaks.get(0).hue;
            Peak most = peaks.get(0);
            for (Peak p : peaks) {
                if (p.area > most.area)
                    most = p;
            }
            hb = most.hue;
            for (int i = 1; i < peaks.size(); i++) {
                Peak p = peaks.get(i);
                if (hueDistance(p.hue, ha) >= 40 && p.vivid >= 0.08) {
                    h2 = p.hue;
                    break;
                }
            }
        }
        ha = avoid(ha, WARN_HUE, WARN_AVOID_DEG);
        ha = avoid(ha, ERROR_HUE, ERROR_AVOID_DEG);
        return new Palette(tokens(ha, hb, h2), ha, hb, h2, fallback);
    }

    /** Every token for an accent hue, a backdrop hue and a second glow hue (NaN: the backdrop's). */
    static int[] tokens(double ha, double hb, double h2) {
        int[] t = STOCK.clone();
        for (int k : NEUTRALS) {          // same lightness as the stock token, the game's hue
            double[] s = oklch(STOCK[k]);
            t[k] = fromOklch(s[0], Math.min(s[1] * 1.6, 0.035), hb);
        }
        t[PANEL] = (t[PANEL] & 0x00FFFFFF) | (STOCK[PANEL] & 0xFF000000);
        t[LINE_SOFT] = (t[LINE] & 0x00FFFFFF) | (STOCK[LINE_SOFT] & 0xFF000000);
        t[GLYPH_INK] = t[BG_TOP];
        int panelHi = t[PANEL_HI];
        double[] a = oklch(STOCK[ACCENT]);
        double lL = a[0];
        int acc = fromOklch(lL, a[1], ha);
        while (contrast(acc, panelHi) < 7.0 && lL < 0.93) {   // AAA on the brightest surface it is text on
            lL += 0.01;
            acc = fromOklch(lL, a[1], ha);
        }
        t[ACCENT] = acc;
        double[] d = oklch(STOCK[ACCENT_DEEP]);
        t[ACCENT_DEEP] = fromOklch(d[0], d[1], ha);
        double[] f = oklch(STOCK[ACCENT_FACE]);
        t[ACCENT_FACE] = fromOklch(f[0], f[1], ha);
        t[GLOW] = fromOklch(0.80, 0.16, ha);
        t[GLOW2] = fromOklch(0.74, 0.15, Double.isNaN(h2) ? hb : h2);
        return t;
    }

    /** A contrast pair the UI relies on: its ratio and the minimum it must reach. */
    static final class Check {
        final String name;
        final double ratio, min;

        Check(String name, double ratio, double min) {
            this.name = name;
            this.ratio = ratio;
            this.min = min;
        }

        boolean ok() {
            return ratio >= min;
        }
    }

    /** Every pair the UI relies on (palette.py checks()). */
    static List<Check> checks(int[] t) {
        int bg = t[BG_TOP], hi = t[PANEL_HI];
        int accFill = over(t[ACCENT], 0.16, hi);   // the primary row's tinted fill
        return Arrays.asList(new Check("TEXT on PANEL_HI", contrast(t[TEXT], hi), 7.0),
                             new Check("TEXT_DIM on PANEL_HI", contrast(t[TEXT_DIM], hi), 4.5),
                             new Check("TEXT_FAINT on BG_TOP", contrast(t[TEXT_FAINT], bg), 3.0),
                             new Check("ACCENT on PANEL_HI", contrast(t[ACCENT], hi), 7.0),
                             new Check("ACCENT on BG_TOP", contrast(t[ACCENT], bg), 7.0),
                             new Check("GLYPH_INK on ACCENT", contrast(t[GLYPH_INK], t[ACCENT]), 4.5),
                             new Check("GLYPH_INK on ACCENT_DEEP", contrast(t[GLYPH_INK], t[ACCENT_DEEP]), 3.0),
                             new Check("TEXT_BRIGHT on accent fill", contrast(t[TEXT_BRIGHT], accFill), 7.0),
                             new Check("ACCENT outline on BG_TOP (non-text)", contrast(t[ACCENT], bg), 3.0),
                             new Check("LINE on BG_TOP (decorative)", contrast(t[LINE], bg), 1.0));
    }

    // ---- Hero backdrop -----------------------------------------------------------------------------------------------

    /** An ARGB picture. */
    static final class Image {
        final int[] px;
        final int w, h;

        Image(int[] px, int w, int h) {
            this.px = px;
            this.w = w;
            this.h = h;
        }
    }

    /** bakeHero() with the design's side, blur and cap. */
    static Image bakeHero(int[] argb, int w, int h) {
        return bakeHero(argb, w, h, HERO_SIDE, HERO_SIGMA, HERO_LUM_CAP);
    }

    /**
     * The hero backdrop: box-scaled to side on its long side, Gaussian-blurred (sigma pixels), then a linear-light gain
     * so that the 95th-percentile luminance lands on cap, and any pixel still above cap scaled down to it (hue kept).
     * Opaque; no pixel's WCAG luminance is above cap (rounded down where rounding to 8 bits would cross it).
     */
    static Image bakeHero(int[] argb, int w, int h, int side, double sigma, double cap) {
        double s = (double)side / Math.max(w, h);
        int nw = Math.max(1, (int)Math.round(w * s)), nh = Math.max(1, (int)Math.round(h * s));
        int[] small = boxResize(opaque(argb), w, h, nw, nh);
        double[][] ch = new double[3][nw * nh];
        for (int i = 0; i < small.length; i++) {
            ch[0][i] = r(small[i]);
            ch[1][i] = g(small[i]);
            ch[2][i] = b(small[i]);
        }
        for (double[] c : ch)
            gaussian(c, nw, nh, sigma);
        int n = nw * nh;
        int[] blurred = new int[n];
        double[] lums = new double[n];
        for (int i = 0; i < n; i++) {
            blurred[i] = 0xFF000000 | clamp8(ch[0][i]) << 16 | clamp8(ch[1][i]) << 8 | clamp8(ch[2][i]);
            lums[i] = luminance(blurred[i]);
        }
        double[] sorted = lums.clone();
        Arrays.sort(sorted);
        double hi = sorted[(int)(n * 0.95)];
        if (hi == 0)
            hi = 1e-6;
        double gain = Math.min(1.0, cap / hi);
        int[] out = new int[n];
        for (int i = 0; i < n; i++) {
            int p = blurred[i];
            double rl = LIN[r(p)] * gain, gl = LIN[g(p)] * gain, bl = LIN[b(p)] * gain;
            double lL = 0.2126 * rl + 0.7152 * gl + 0.0722 * bl;
            if (lL > cap) {
                rl *= cap / lL;
                gl *= cap / lL;
                bl *= cap / lL;
            }
            int q = 0xFF000000 | channel(rl) << 16 | channel(gl) << 8 | channel(bl);
            if (luminance(q) > cap)
                q = 0xFF000000 | floorChannel(rl) << 16 | floorChannel(gl) << 8 | floorChannel(bl);
            out[i] = q;
        }
        return new Image(out, nw, nh);
    }

    private static int floorChannel(double v) {
        return (int)Math.max(0, Math.min(255, Math.floor(255 * gam(Math.max(0.0, Math.min(1.0, v))))));
    }

    private static int clamp8(double v) {
        return (int)Math.max(0, Math.min(255, Math.rint(v)));
    }

    private static int[] opaque(int[] px) {
        int[] o = new int[px.length];
        for (int i = 0; i < px.length; i++)
            o[i] = px[i] | 0xFF000000;
        return o;
    }

    /** A separable Gaussian blur in place, edges clamped, kernel radius 3 sigma. */
    private static void gaussian(double[] v, int w, int h, double sigma) {
        if (sigma <= 0)
            return;
        int rad = Math.max(1, (int)Math.ceil(3 * sigma));
        double[] k = new double[2 * rad + 1];
        double sum = 0;
        for (int i = -rad; i <= rad; i++) {
            k[i + rad] = Math.exp(-(i * i) / (2 * sigma * sigma));
            sum += k[i + rad];
        }
        for (int i = 0; i < k.length; i++)
            k[i] /= sum;
        double[] tmp = new double[v.length];
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                double a = 0;
                for (int i = -rad; i <= rad; i++)
                    a += k[i + rad] * v[y * w + Math.max(0, Math.min(w - 1, x + i))];
                tmp[y * w + x] = a;
            }
        }
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                double a = 0;
                for (int i = -rad; i <= rad; i++)
                    a += k[i + rad] * tmp[Math.max(0, Math.min(h - 1, y + i)) * w + x];
                v[y * w + x] = a;
            }
        }
    }

    /**
     * Area-averaging (box) resize, as PIL's Image.BOX: each output pixel the coverage-weighted mean of the source
     * pixels under it, colour premultiplied by alpha.
     */
    static int[] boxResize(int[] px, int w, int h, int nw, int nh) {
        double[] acc = new double[nw * nh * 4];
        double fx = (double)w / nw, fy = (double)h / nh;
        for (int oy = 0; oy < nh; oy++) {
            double y0 = oy * fy, y1 = y0 + fy;
            for (int sy = (int)Math.floor(y0); sy < Math.min(h, (int)Math.ceil(y1)); sy++) {
                double cy = Math.min(y1, sy + 1) - Math.max(y0, sy);
                if (cy <= 0)
                    continue;
                for (int ox = 0; ox < nw; ox++) {
                    double x0 = ox * fx, x1 = x0 + fx;
                    int o = (oy * nw + ox) * 4;
                    for (int sx = (int)Math.floor(x0); sx < Math.min(w, (int)Math.ceil(x1)); sx++) {
                        double cx = Math.min(x1, sx + 1) - Math.max(x0, sx);
                        if (cx <= 0)
                            continue;
                        int p = px[sy * w + sx];
                        double wt = cx * cy, al = (p >>> 24) / 255.0;
                        acc[o] += wt * al;
                        acc[o + 1] += wt * al * r(p);
                        acc[o + 2] += wt * al * g(p);
                        acc[o + 3] += wt * al * b(p);
                    }
                }
            }
        }
        double area = fx * fy;
        int[] out = new int[nw * nh];
        for (int i = 0; i < out.length; i++) {
            double al = acc[i * 4];
            if (al <= 0)
                continue;   // transparent black
            int a8 = clamp8(255 * al / area);
            out[i] = a8 << 24 | clamp8(acc[i * 4 + 1] / al) << 16 | clamp8(acc[i * 4 + 2] / al) << 8
                     | clamp8(acc[i * 4 + 3] / al);
        }
        return out;
    }
}
