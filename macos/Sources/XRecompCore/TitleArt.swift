import CoreGraphics
import Foundation
import ImageIO
import UniformTypeIdentifiers

/// The game's title image: the XBE's « $$XTIMAGE » section, decoded on the Mac and cached as a PNG per title
/// (`titles/<TID>/art.png`), and the monogram fallback. Nothing of the game is in the app or the repo: the image comes
/// from the player's own default.xbe at run time. A port of the Android launcher's TitleArt.java.
public enum TitleArt {
    public static let section = "$$XTIMAGE"

    /// The image from an XBE file, or nil (no section, or a format this cannot read).
    public static func decode(xbe: URL) -> ArgbImage? {
        guard let sec = XbeInfo.section(xbe, named: section) else { return nil }
        return Xpr.decode(sec)
    }

    /// The cached PNG, else decoded from `xbe` and cached at `cache`. nil: show the monogram.
    public static func load(cache: URL, xbe: URL?) -> CGImage? {
        if let img = readPNG(cache) { return img }
        guard let xbe, let argb = decode(xbe: xbe), let img = cgImage(argb) else { return nil }
        try? writePNG(img, to: cache)
        return img
    }

    /// ARGB pixels as a CGImage (straight alpha).
    public static func cgImage(_ a: ArgbImage) -> CGImage? {
        guard a.width > 0, a.height > 0, a.pixels.count == a.width * a.height else { return nil }
        // ARGB in host (little-endian) UInt32 = BGRA bytes: byteOrder32Little + alpha first.
        let data = a.pixels.withUnsafeBufferPointer { Data(buffer: $0) }
        guard let provider = CGDataProvider(data: data as CFData) else { return nil }
        let info = CGBitmapInfo(rawValue: CGImageAlphaInfo.first.rawValue | CGBitmapInfo.byteOrder32Little.rawValue)
        return CGImage(width: a.width, height: a.height, bitsPerComponent: 8, bitsPerPixel: 32,
                       bytesPerRow: a.width * 4, space: CGColorSpace(name: CGColorSpace.sRGB)!, bitmapInfo: info,
                       provider: provider, decode: nil, shouldInterpolate: true, intent: .defaultIntent)
    }

    public static func writePNG(_ img: CGImage, to url: URL) throws {
        try FileManager.default.ensureDirectory(url.deletingLastPathComponent())
        let tmp = url.deletingLastPathComponent().appendingPathComponent(url.lastPathComponent + ".tmp")
        guard let dest = CGImageDestinationCreateWithURL(tmp as CFURL, UTType.png.identifier as CFString, 1, nil) else {
            throw CocoaError(.fileWriteUnknown)
        }
        CGImageDestinationAddImage(dest, img, nil)
        guard CGImageDestinationFinalize(dest) else { throw CocoaError(.fileWriteUnknown) }
        guard rename(tmp.path, url.path) == 0 else { throw CocoaError(.fileWriteUnknown) }
    }

    public static func readPNG(_ url: URL) -> CGImage? {
        guard FileManager.default.fileExists(atPath: url.path),
              let src = CGImageSourceCreateWithURL(url as CFURL, nil) else { return nil }
        return CGImageSourceCreateImageAtIndex(src, 0, nil)
    }

    /// The pixels of a CGImage as ARGB (for the accent), drawn into an sRGB bitmap of at most `side` pixels.
    public static func argb(_ img: CGImage, side: Int = 64) -> ArgbImage? {
        let scale = min(1, Double(side) / Double(max(img.width, img.height)))
        let w = max(1, Int(Double(img.width) * scale)), h = max(1, Int(Double(img.height) * scale))
        var px = [UInt32](repeating: 0, count: w * h)
        let info = CGImageAlphaInfo.premultipliedFirst.rawValue | CGBitmapInfo.byteOrder32Little.rawValue
        let ok = px.withUnsafeMutableBytes { buf -> Bool in
            guard let ctx = CGContext(data: buf.baseAddress, width: w, height: h, bitsPerComponent: 8,
                                      bytesPerRow: w * 4, space: CGColorSpace(name: CGColorSpace.sRGB)!,
                                      bitmapInfo: info) else { return false }
            ctx.interpolationQuality = .medium
            ctx.draw(img, in: CGRect(x: 0, y: 0, width: w, height: h))
            return true
        }
        return ok ? ArgbImage(width: w, height: h, pixels: px) : nil
    }

    // ---- monogram ------------------------------------------------------------------------------------------------

    /// Java's String.hashCode over UTF-16 units, so a title keeps the Android launcher's monogram hue.
    static func javaHash(_ s: String) -> Int32 {
        var h: Int32 = 0
        for u in s.utf16 { h = h &* 31 &+ Int32(u) }
        return h
    }

    /// A hue (degrees, 0..359) from the title ID, stable for a title: the monogram's, and the accent's fallback.
    public static func titleHue(_ titleId: String?) -> Int {
        var h = titleId.map { javaHash($0.uppercased()) } ?? 0
        h ^= Int32(bitPattern: UInt32(bitPattern: h) >> 16)
        h = h &* 0x45d9f3b
        h ^= Int32(bitPattern: UInt32(bitPattern: h) >> 16)
        return Int(h & 0x7fff_ffff) % 360
    }

    /// The monogram's colour: a vivid hue from the title ID.
    public static func monogramColor(_ titleId: String?) -> UInt32 {
        Palette.hsv(Double(titleHue(titleId)), 0.62, 0.9)
    }

    /// « Sample Game » -> « SG », « Halo » -> « HA », « 4x4 Evo 2 » -> « 4E ».
    public static func initials(_ title: String?) -> String {
        guard let title else { return "" }
        let words = title.trimmingCharacters(in: .whitespaces)
            .components(separatedBy: CharacterSet(charactersIn: " \t\n:-_")).filter { !$0.isEmpty }
        var out = ""
        for wd in words {
            if let c = wd.first, c.isLetter || c.isNumber { out += c.uppercased() }
            if out.count == 2 { break }
        }
        if out.count == 1, let w0 = words.first, w0.count > 1 {
            let c = w0[w0.index(after: w0.startIndex)]
            if c.isLetter || c.isNumber { out += c.uppercased() }
        }
        return out
    }
}
