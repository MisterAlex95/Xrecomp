import Foundation

/// The launcher's colour tokens: the Android launcher's stock theme (GamePalette.STOCK, ours), ARGB, same names and
/// order. The app's Theme takes every colour from here; the checks hold it equal to the Java array and check the
/// text contrast pairs.
public enum Palette {
    public static let names = ["BG_TOP", "BG_BOTTOM", "PANEL", "PANEL_HI", "LINE", "LINE_SOFT", "TEXT", "TEXT_BRIGHT",
                               "TEXT_DIM", "TEXT_FAINT", "ACCENT", "ACCENT_DEEP", "ACCENT_FACE", "TRACK", "OK", "WARN",
                               "ERROR", "GLYPH_FACE", "GLYPH_INK", "GLOW", "GLOW2"]
    public static let stock: [UInt32] = [
        0xFF0C110F, 0xFF060807, 0xE6111814, 0xFF18211C, 0xFF27332D, 0x6627332D, 0xFFF3EEE4,
        0xFFFFFFFF, 0xFFA9AFA6, 0xFF6C746D, 0xFF4FE38C, 0xFF1F7A48, 0xFF143A26, 0xFF1A241F,
        0xFF4FE38C, 0xFFF2BA52, 0xFFFF6F61, 0xFFEDE6D8, 0xFF0C110F, 0xFF4FE38C, 0xFF4FE38C,
    ]

    public static func token(_ name: String) -> UInt32 { stock[names.firstIndex(of: name)!] }

    public static var bgTop: UInt32 { stock[0] }
    public static var bgBottom: UInt32 { stock[1] }
    public static var panel: UInt32 { stock[2] }
    public static var panelHi: UInt32 { stock[3] }
    public static var line: UInt32 { stock[4] }
    public static var lineSoft: UInt32 { stock[5] }
    public static var text: UInt32 { stock[6] }
    public static var textBright: UInt32 { stock[7] }
    public static var textDim: UInt32 { stock[8] }
    public static var textFaint: UInt32 { stock[9] }
    public static var accent: UInt32 { stock[10] }
    public static var accentDeep: UInt32 { stock[11] }
    public static var accentFace: UInt32 { stock[12] }
    public static var track: UInt32 { stock[13] }
    public static var ok: UInt32 { stock[14] }
    public static var warn: UInt32 { stock[15] }
    public static var error: UInt32 { stock[16] }
    public static var glyphFace: UInt32 { stock[17] }
    /// The ink on the accent (the Play button's label).
    public static var glyphInk: UInt32 { stock[18] }

    // ---- colour arithmetic -----------------------------------------------------------------------------------------

    public static func rgb(_ c: UInt32) -> (Double, Double, Double) {
        (Double((c >> 16) & 0xFF) / 255, Double((c >> 8) & 0xFF) / 255, Double(c & 0xFF) / 255)
    }

    public static func alpha(_ c: UInt32) -> Double { Double(c >> 24) / 255 }

    /// WCAG 2.x relative luminance.
    public static func luminance(_ c: UInt32) -> Double {
        func lin(_ v: Double) -> Double { v <= 0.04045 ? v / 12.92 : pow((v + 0.055) / 1.055, 2.4) }
        let (r, g, b) = rgb(c)
        return 0.2126 * lin(r) + 0.7152 * lin(g) + 0.0722 * lin(b)
    }

    /// WCAG contrast ratio of two opaque colours.
    public static func contrast(_ a: UInt32, _ b: UInt32) -> Double {
        let la = luminance(a), lb = luminance(b)
        return (max(la, lb) + 0.05) / (min(la, lb) + 0.05)
    }

    /// HSV (hue in degrees, s and v 0...1) to opaque ARGB.
    public static func hsv(_ h: Double, _ s: Double, _ v: Double) -> UInt32 {
        let hh = (h.truncatingRemainder(dividingBy: 360) + 360).truncatingRemainder(dividingBy: 360) / 60
        let i = Int(hh), f = hh - Double(i)
        let p = v * (1 - s), q = v * (1 - s * f), t = v * (1 - s * (1 - f))
        let (r, g, b): (Double, Double, Double)
        switch i {
        case 0: (r, g, b) = (v, t, p)
        case 1: (r, g, b) = (q, v, p)
        case 2: (r, g, b) = (p, v, t)
        case 3: (r, g, b) = (p, q, v)
        case 4: (r, g, b) = (t, p, v)
        default: (r, g, b) = (v, p, q)
        }
        return argb(r, g, b)
    }

    public static func argb(_ r: Double, _ g: Double, _ b: Double, alpha: Double = 1) -> UInt32 {
        func c(_ v: Double) -> UInt32 { UInt32((max(0, min(1, v)) * 255).rounded()) }
        return c(alpha) << 24 | c(r) << 16 | c(g) << 8 | c(b)
    }

    /// Hue (degrees), saturation and value of an ARGB colour.
    public static func toHsv(_ c: UInt32) -> (Double, Double, Double) {
        let (r, g, b) = rgb(c)
        let mx = max(r, g, b), mn = min(r, g, b), d = mx - mn
        var h = 0.0
        if d > 0 {
            if mx == r { h = 60 * ((g - b) / d).truncatingRemainder(dividingBy: 6) }
            else if mx == g { h = 60 * ((b - r) / d + 2) }
            else { h = 60 * ((r - g) / d + 4) }
        }
        if h < 0 { h += 360 }
        return (h, mx == 0 ? 0 : d / mx, mx)
    }
}

/// A game's accent from its title image (v1: the dominant hue, then a contrast clamp; the full OKLCH GamePalette
/// port is later work). The accent must keep the Play label (GLYPH_INK) readable on it, at least 4.5:1, and stand
/// out of the dark background, at least 3:1.
public enum Accent {
    public static let minInkContrast = 4.5
    public static let minBackgroundContrast = 3.0
    /// The accent keeps this far from ERROR's hue: a red Play button would read as an error.
    public static let errorAvoidDegrees = 18.0

    /// A hue pushed out of +-deg around `center`.
    public static func avoid(_ hue: Double, _ center: Double, _ deg: Double) -> Double {
        let d = (hue - center + 540).truncatingRemainder(dividingBy: 360) - 180
        guard abs(d) < deg else { return hue }
        return (center + (d >= 0 ? deg : -deg) + 360).truncatingRemainder(dividingBy: 360)
    }

    /// The dominant hue of the image's coloured pixels (36 bins, weighted by saturation), or nil when the image is
    /// mostly grey.
    public static func dominantHue(_ img: ArgbImage) -> Double? {
        var bins = [Double](repeating: 0, count: 36)
        var hueSum = [Double](repeating: 0, count: 36)
        var chromatic = 0.0
        for p in img.pixels where p >> 24 >= 128 {
            let (h, s, v) = Palette.toHsv(p)
            guard s >= 0.25, v >= 0.2 else { continue }
            let w = s * v
            let k = min(35, Int(h / 10))
            bins[k] += w
            hueSum[k] += h * w
            chromatic += 1
        }
        guard chromatic >= Double(img.pixels.count) * 0.06 else { return nil }
        // The best window of three neighbouring bins, so a hue on a bin edge is not split.
        var best = 0, bestW = -1.0
        for k in 0..<36 {
            let w = bins[(k + 35) % 36] + bins[k] + bins[(k + 1) % 36]
            if w > bestW { bestW = w; best = k }
        }
        // The weighted mean hue of the window (bins across 0/360 unwrapped around the centre).
        var sum = 0.0, wsum = 0.0
        for dk in -1...1 {
            let k = (best + dk + 36) % 36
            guard bins[k] > 0 else { continue }
            var h = hueSum[k] / bins[k]
            if dk == -1 && best == 0 { h -= 360 }
            if dk == 1 && best == 35 { h += 360 }
            sum += h * bins[k]
            wsum += bins[k]
        }
        return ((sum / wsum) + 360).truncatingRemainder(dividingBy: 360)
    }

    /// The accent for a hue: vivid, then lightened until the ink and the background contrast hold.
    public static func color(hue hueIn: Double) -> UInt32 {
        let hue = avoid(hueIn, Palette.toHsv(Palette.error).0, errorAvoidDegrees)
        var s = 0.62, v = 0.86
        var c = Palette.hsv(hue, s, v)
        var guardSteps = 0
        while (Palette.contrast(c, Palette.glyphInk) < minInkContrast
               || Palette.contrast(c, Palette.bgTop) < minBackgroundContrast) && guardSteps < 40 {
            // Brighter first, then less saturated (blue and violet need both).
            if v < 1 { v = min(1, v + 0.03) } else { s = max(0.2, s - 0.03) }
            c = Palette.hsv(hue, s, v)
            guardSteps += 1
        }
        return c
    }

    /// The accent for a title: from its image when it has colour, else from the title ID's hue.
    public static func forTitle(image: ArgbImage?, titleId: String?) -> UInt32 {
        if let image, let h = dominantHue(image) { return color(hue: h) }
        return color(hue: Double(TitleArt.titleHue(titleId)))
    }
}
