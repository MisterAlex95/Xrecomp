import Foundation

/// An image decoded from the game: ARGB pixels, row by row.
public struct ArgbImage: Equatable, Sendable {
    public let width: Int
    public let height: Int
    public var pixels: [UInt32]

    public init(width: Int, height: Int, pixels: [UInt32]) {
        self.width = width
        self.height = height
        self.pixels = pixels
    }

    public subscript(x: Int, y: Int) -> UInt32 { pixels[y * width + x] }
}

/// The XPR0 resource file in an XBE's $$XTIMAGE section, and the NV2A texture formats it uses (a port of the Android
/// launcher's TitleArt.java; public documentation: xboxdevwiki « XPR », src/d3d/d3d8_xbox.h, nv2a_regs.h).
///
/// "XPR0", total size (+4), header size (+8, the data follows it), then 20-byte resource headers (Common, Data, Lock,
/// Format, Size) until 0xFFFFFFFF. A texture has type 4 in Common bits 16..18; Data is its offset from the end of the
/// header. Format: bits 8..15 the colour format, bits 20..23 / 24..27 log2 of the width / height (Size, when not 0,
/// gives width - 1 and height - 1 in 12-bit fields). Colour formats: 0x0C DXT1, 0x0E DXT3, 0x0F DXT5 (4x4 blocks);
/// 0x06 A8R8G8B8, 0x07 X8R8G8B8, 0x05 R5G6B5 swizzled (Morton order); 0x12, 0x1E, 0x11 their linear forms.
public enum Xpr {
    static let maxSide = 1024

    /// The first texture of an XPR0 file, or nil.
    public static func decode(_ b: [UInt8]) -> ArgbImage? {
        guard b.count >= 32, b[0] == 0x58, b[1] == 0x50, b[2] == 0x52, b[3] == 0x30 else { return nil }
        let hdr = Int(Int32(bitPattern: le32(b, 8)))
        guard hdr >= 12, hdr <= b.count else { return nil }
        var o = 12
        while o + 20 <= hdr {
            let common = le32(b, o)
            if common == 0xFFFF_FFFF { break }
            if (common >> 16) & 7 != 4 { o += 20; continue }   // not a texture
            let data = Int64(le32(b, o + 4)), format = le32(b, o + 12), size = le32(b, o + 16)
            let fmt = Int((format >> 8) & 0xFF)
            var w = 1 << Int((format >> 20) & 0xF), h = 1 << Int((format >> 24) & 0xF)
            if size != 0 {
                w = Int(size & 0xFFF) + 1
                h = Int((size >> 12) & 0xFFF) + 1
            }
            guard w <= maxSide, h <= maxSide else { return nil }
            let need = bytesFor(fmt, w, h)
            guard need > 0 else { return nil }
            var at = Int64(hdr) + data
            if at + Int64(need) > Int64(b.count) && data >= Int64(hdr) { at = data }   // an absolute offset
            guard at >= 0, at + Int64(need) <= Int64(b.count) else { return nil }
            guard let px = decode(b, Int(at), fmt, w, h) else { return nil }
            return ArgbImage(width: w, height: h, pixels: px)
        }
        return nil
    }

    public static func bytesFor(_ fmt: Int, _ w: Int, _ h: Int) -> Int {
        let bw = (w + 3) / 4, bh = (h + 3) / 4
        switch fmt {
        case 0x0C: return bw * bh * 8
        case 0x0E, 0x0F: return bw * bh * 16
        case 0x06, 0x07, 0x12, 0x1E: return w * h * 4
        case 0x05, 0x11: return w * h * 2
        default: return -1
        }
    }

    static func decode(_ b: [UInt8], _ at: Int, _ fmt: Int, _ w: Int, _ h: Int) -> [UInt32]? {
        var px = [UInt32](repeating: 0, count: w * h)
        switch fmt {
        case 0x0C, 0x0E, 0x0F:
            Dxt.decode(b, at, fmt, w, h, &px)
            return px
        case 0x06, 0x07, 0x05:
            guard w.nonzeroBitCount == 1, h.nonzeroBitCount == 1 else { return nil }
            let (xm, ym) = swizzleMasks(w, h)
            for y in 0..<h {
                for x in 0..<w { px[y * w + x] = texel(b, at, fmt, deposit(x, xm) | deposit(y, ym)) }
            }
            return px
        case 0x12, 0x1E, 0x11:
            for i in 0..<(w * h) { px[i] = texel(b, at, fmt, i) }
            return px
        default:
            return nil
        }
    }

    static func texel(_ b: [UInt8], _ at: Int, _ fmt: Int, _ i: Int) -> UInt32 {
        if fmt == 0x05 || fmt == 0x11 {
            return rgb565(UInt32(b[at + 2 * i]) | UInt32(b[at + 2 * i + 1]) << 8)
        }
        let v = le32(b, at + 4 * i)
        return fmt == 0x07 || fmt == 0x1E ? v | 0xFF00_0000 : v
    }

    /// The NV2A swizzle masks (xemu's generate_swizzle_masks): x and y bits interleaved, x first.
    public static func swizzleMasks(_ w: Int, _ h: Int) -> (Int, Int) {
        var x = 0, y = 0, bit = 1, i = 1
        while i < w || i < h {
            if i < w { x |= bit; bit <<= 1 }
            if i < h { y |= bit; bit <<= 1 }
            i <<= 1
        }
        return (x, y)
    }

    public static func deposit(_ v: Int, _ maskIn: Int) -> Int {
        var r = 0, bit = 1, mask = maskIn
        while mask != 0 {
            let low = mask & -mask
            if v & bit != 0 { r |= low }
            mask &= mask - 1
            bit <<= 1
        }
        return r
    }

    public static func rgb565(_ v: UInt32) -> UInt32 {
        let r = (v >> 11) & 31, g = (v >> 5) & 63, bl = v & 31
        return 0xFF00_0000 | (r << 3 | r >> 2) << 16 | (g << 2 | g >> 4) << 8 | (bl << 3 | bl >> 2)
    }
}
