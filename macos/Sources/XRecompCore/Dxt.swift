import Foundation

/// DXT1 / DXT3 / DXT5 (BC1 / BC2 / BC3) blocks, as TitleArt.java decodes them.
enum Dxt {
    static func decode(_ b: [UInt8], _ at: Int, _ fmt: Int, _ w: Int, _ h: Int, _ px: inout [UInt32]) {
        let bw = (w + 3) / 4, bh = (h + 3) / 4, bs = fmt == 0x0C ? 8 : 16
        var pal = [UInt32](repeating: 0, count: 4)
        var alpha = [UInt32](repeating: 255, count: 16)
        for by in 0..<bh {
            for bx in 0..<bw {
                let o = at + (by * bw + bx) * bs
                if fmt == 0x0E {
                    for i in 0..<16 { alpha[i] = UInt32((b[o + i / 2] >> UInt8((i & 1) * 4)) & 0xF) * 17 }
                } else if fmt == 0x0F {
                    let a0 = Int(b[o]), a1 = Int(b[o + 1])
                    var bits: UInt64 = 0
                    for k in 0..<6 { bits |= UInt64(b[o + 2 + k]) << UInt64(8 * k) }
                    for i in 0..<16 {
                        let code = Int((bits >> UInt64(3 * i)) & 7)
                        let a: Int
                        if code == 0 { a = a0 }
                        else if code == 1 { a = a1 }
                        else if a0 > a1 { a = ((8 - code) * a0 + (code - 1) * a1) / 7 }
                        else if code == 6 { a = 0 }
                        else if code == 7 { a = 255 }
                        else { a = ((6 - code) * a0 + (code - 1) * a1) / 5 }
                        alpha[i] = UInt32(a)
                    }
                }
                let c = fmt == 0x0C ? o : o + 8
                let c0 = UInt32(b[c]) | UInt32(b[c + 1]) << 8, c1 = UInt32(b[c + 2]) | UInt32(b[c + 3]) << 8
                pal[0] = Xpr.rgb565(c0)
                pal[1] = Xpr.rgb565(c1)
                if c0 > c1 || fmt != 0x0C {
                    pal[2] = mix(pal[0], pal[1], 2, 1, 3)
                    pal[3] = mix(pal[0], pal[1], 1, 2, 3)
                } else {
                    pal[2] = mix(pal[0], pal[1], 1, 1, 2)
                    pal[3] = 0   // transparent black
                }
                let idx = le32(b, c + 4)
                for i in 0..<16 {
                    let x = bx * 4 + (i & 3), y = by * 4 + (i >> 2)
                    if x >= w || y >= h { continue }
                    var col = pal[Int((idx >> UInt32(2 * i)) & 3)]
                    if fmt != 0x0C { col = (col & 0x00FF_FFFF) | alpha[i] << 24 }
                    px[y * w + x] = col
                }
            }
        }
    }

    static func mix(_ a: UInt32, _ b: UInt32, _ wa: UInt32, _ wb: UInt32, _ d: UInt32) -> UInt32 {
        let r = (((a >> 16) & 0xFF) * wa + ((b >> 16) & 0xFF) * wb) / d
        let g = (((a >> 8) & 0xFF) * wa + ((b >> 8) & 0xFF) * wb) / d
        let bl = ((a & 0xFF) * wa + (b & 0xFF) * wb) / d
        return 0xFF00_0000 | r << 16 | g << 8 | bl
    }
}
