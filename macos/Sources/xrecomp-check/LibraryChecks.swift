import Foundation
import XRecompCore

// ---- fixtures: TestSupport.java's, built here (never game data) -----------------------------------------------

func putLe32(_ b: inout [UInt8], _ o: Int, _ v: UInt32) {
    b[o] = UInt8(v & 0xFF)
    b[o + 1] = UInt8((v >> 8) & 0xFF)
    b[o + 2] = UInt8((v >> 16) & 0xFF)
    b[o + 3] = UInt8((v >> 24) & 0xFF)
}

/// A minimal XBE: "XBEH", base 0x10000, certificate at 0x10200 with the title ID and a UTF-16 name.
func synthXbe(_ titleId: UInt32, _ name: String) -> [UInt8] {
    var b = [UInt8](repeating: 0, count: 0x400)
    b[0] = 0x58; b[1] = 0x42; b[2] = 0x45; b[3] = 0x48
    putLe32(&b, 0x104, 0x10000)
    putLe32(&b, 0x118, 0x10200)
    putLe32(&b, 0x200 + 0x08, titleId)
    let n = Array(name.utf16.prefix(40))
    for (i, u) in n.enumerated() {
        b[0x20C + 2 * i] = UInt8(u & 0xFF)
        b[0x20C + 2 * i + 1] = UInt8(u >> 8)
    }
    return b
}

/// synthXbe with one section, `section`, holding `image`: headers up to 0x400 (section header at 0x300, its name at
/// 0x340), the section's bytes from 0x400.
func synthXbeWithSection(_ titleId: UInt32, _ name: String, _ section: String = "$$XTIMAGE", _ image: [UInt8]) -> [UInt8] {
    var b = synthXbe(titleId, name) + [UInt8](repeating: 0, count: image.count)
    putLe32(&b, 0x108, 0x400)
    putLe32(&b, 0x11C, 1)
    putLe32(&b, 0x120, 0x10300)
    putLe32(&b, 0x300 + 0x0C, 0x400)
    putLe32(&b, 0x300 + 0x10, UInt32(image.count))
    putLe32(&b, 0x300 + 0x14, 0x10340)
    for (i, c) in section.utf8.enumerated() { b[0x340 + i] = c }
    b.replaceSubrange(0x400..<0x400 + image.count, with: image)
    return b
}

/// An XPR0 with one texture: format fmt, w x h (powers of two), data after a 2048-byte header.
func synthXpr0(_ fmt: UInt32, _ w: Int, _ h: Int, _ data: [UInt8]) -> [UInt8] {
    let hdr = 0x800
    var b = [UInt8](repeating: 0, count: hdr + data.count)
    for i in 12..<hdr { b[i] = 0xAD }
    b[0] = 0x58; b[1] = 0x50; b[2] = 0x52; b[3] = 0x30
    putLe32(&b, 4, UInt32(b.count))
    putLe32(&b, 8, UInt32(hdr))
    putLe32(&b, 12, 0x0004_0001)
    putLe32(&b, 16, 0)
    putLe32(&b, 20, 0)
    putLe32(&b, 24, 0x29 | fmt << 8 | 1 << 16 | UInt32(w.trailingZeroBitCount) << 20 | UInt32(h.trailingZeroBitCount) << 24)
    putLe32(&b, 28, 0)
    putLe32(&b, 32, 0xFFFF_FFFF)
    b.replaceSubrange(hdr..<hdr + data.count, with: data)
    return b
}

let RED: UInt32 = 0xF800, BLUE: UInt32 = 0x001F

func dxtBlock(_ d: inout [UInt8], _ o: Int, _ c0: UInt32, _ c1: UInt32, _ idx: UInt32) {
    d[o] = UInt8(c0 & 0xFF); d[o + 1] = UInt8(c0 >> 8)
    d[o + 2] = UInt8(c1 & 0xFF); d[o + 3] = UInt8(c1 >> 8)
    putLe32(&d, o + 4, idx)
}

/// An 8 x 4 DXT1 image: a four-colour block, then a three-colour block with its transparent index.
func dxt1Two() -> [UInt8] {
    var d = [UInt8](repeating: 0, count: 16)
    var idx: UInt32 = 0
    for i in 0..<16 { idx |= UInt32(i % 4) << UInt32(2 * i) }
    dxtBlock(&d, 0, RED, BLUE, idx)
    dxtBlock(&d, 8, BLUE, RED, 3 | 2 << 2)
    return d
}

/// A 128 x 128 DXT1 picture: rings over a hue sweep (TestSupport.dxt1Picture's idea).
func dxt1Picture() -> [UInt8] {
    let n = 32
    var d = [UInt8](repeating: 0, count: n * n * 8)
    func r565(_ c: UInt32) -> UInt32 { ((c >> 19) & 31) << 11 | ((c >> 10) & 63) << 5 | ((c >> 3) & 31) }
    for by in 0..<n {
        for bx in 0..<n {
            let hue = 190 + Double(bx + by) * 1.6
            var a = r565(Palette.hsv(hue, 0.75, 0.95)), z = r565(Palette.hsv(hue + 50, 0.85, 0.22))
            if a <= z { swap(&a, &z) }
            var idx: UInt32 = 0
            for i in 0..<16 {
                let x = Double(bx * 4 + (i & 3) - 64), y = Double(by * 4 + (i >> 2) - 64)
                let band = Int((x * x + y * y).squareRoot() / 6) % 4
                let code: UInt32 = band == 0 ? 0 : band == 1 ? 2 : band == 2 ? 3 : 1
                idx |= code << UInt32(2 * i)
            }
            dxtBlock(&d, (by * n + bx) * 8, a, z, idx)
        }
    }
    return d
}

func registerLibraryChecks() {
    // ---- TitleArtTest.java's cases ------------------------------------------------------------------------------
    check("art: DXT1 from an XPR0") {
        guard let img = expectNotNil(Xpr.decode(synthXpr0(0x0C, 8, 4, dxt1Two()))) else { return }
        expectEq(img.width, 8)
        expectEq(img.height, 4)
        let px = img.pixels
        expectEq(px[0], 0xFFFF0000)
        expectEq(px[1], 0xFF0000FF)
        expectEq((px[2] >> 16) & 0xFF, 170)
        expectEq((px[3] >> 16) & 0xFF, 85)
        expectEq(px[8], 0xFFFF0000)
        expectEq(px[4], 0, "block 1 texel 0: transparent")
        expectEq(px[5] & 0xFF, 127)
        expectEq((px[5] >> 16) & 0xFF, 127)
        expectEq(px[5] >> 24, 0xFF)
    }

    check("art: DXT5 alpha") {
        var d = [UInt8](repeating: 0, count: 16)
        d[0] = 255
        d[1] = 0
        let bits: UInt64 = 1 | 7 << 3
        for k in 0..<6 { d[2 + k] = UInt8((bits >> UInt64(8 * k)) & 0xFF) }
        dxtBlock(&d, 8, RED, BLUE, 0)
        guard let img = expectNotNil(Xpr.decode(synthXpr0(0x0F, 4, 4, d))) else { return }
        expectEq(img.pixels[0] >> 24, 0)
        expectEq(img.pixels[1] >> 24, 255 / 7)
        expectEq(img.pixels[2] >> 24, 255)
        expectEq(img.pixels[2] & 0xFFFFFF, 0xFF0000)
    }

    check("art: swizzled A8R8G8B8") {
        var d = [UInt8](repeating: 0, count: 64)
        for k in 0..<16 { putLe32(&d, 4 * k, 0xFF00_0000 | UInt32(k)) }
        guard let img = expectNotNil(Xpr.decode(synthXpr0(0x06, 4, 4, d))) else { return }
        expectEq(img.pixels[0] & 0xFF, 0)
        expectEq(img.pixels[1] & 0xFF, 1)
        expectEq(img.pixels[2] & 0xFF, 4)
        expectEq(img.pixels[4] & 0xFF, 2)
        expectEq(img.pixels[15] & 0xFF, 15)
        let (xm, ym) = Xpr.swizzleMasks(4, 4)
        expectEq(xm, 0b0101)
        expectEq(ym, 0b1010)
    }

    check("art: linear X8R8G8B8 and R5G6B5") {
        var d = [UInt8](repeating: 0, count: 16)
        putLe32(&d, 0, 0x0012_3456)
        let x = Xpr.decode(synthXpr0(0x1E, 2, 2, d))
        expectEq(x?.pixels[0], 0xFF12_3456, "X8: alpha forced")
        var e = [UInt8](repeating: 0, count: 8)
        e[0] = 0x00; e[1] = 0xF8   // pure red 565
        expectEq(Xpr.decode(synthXpr0(0x11, 2, 2, e))?.pixels[0], 0xFFFF_0000)
    }

    check("art: not an XPR, unknown format, data cut short") {
        expectNil(Xpr.decode([UInt8](repeating: 0, count: 64)))
        var xpr = synthXpr0(0x0C, 8, 4, dxt1Two())
        putLe32(&xpr, 24, 0x29 | 0x33 << 8 | 3 << 20 | 2 << 24)
        expectNil(Xpr.decode(xpr))
        let cut = Array(synthXpr0(0x0C, 8, 4, dxt1Two()).prefix(0x800 + 8))
        expectNil(Xpr.decode(cut))
        expectNil(Xpr.decode([]))
    }

    check("art: from the XBE, cached as PNG") {
        let d = tempDir("art")
        let xpr = synthXpr0(0x0C, 128, 128, dxt1Picture())
        let xbe = d.appendingPathComponent("default.xbe")
        write(xbe, Data(synthXbeWithSection(0x4D53_0004, "Sample Game", "$$XTIMAGE", xpr)))
        expectEq(XbeInfo.section(xbe, named: TitleArt.section), xpr)
        expectNil(XbeInfo.section(xbe, named: ".text"))
        let cache = d.appendingPathComponent("titles/4D530004/art.png")
        guard let img = expectNotNil(TitleArt.load(cache: cache, xbe: xbe)) else { return }
        expectEq(img.width, 128)
        expect(FileManager.default.fileExists(atPath: cache.path))
        let again = TitleArt.load(cache: cache, xbe: nil)
        expectEq(again?.width, 128, "from the cache, without the XBE")
        let a1 = TitleArt.argb(img, side: 128), a2 = again.flatMap { TitleArt.argb($0, side: 128) }
        expectEq(a1?[64, 10], a2?[64, 10])
        // Without the section: no image (the monogram).
        let plain = d.appendingPathComponent("plain.xbe")
        write(plain, Data(synthXbe(0x4142_0001, "Plain")))
        expectNil(TitleArt.load(cache: d.appendingPathComponent("p.png"), xbe: plain))
    }

    check("art: monogram helpers") {
        expectEq(TitleArt.initials("Sample Game"), "SG")
        expectEq(TitleArt.initials("Halo"), "HA")
        expectEq(TitleArt.initials("4x4 Evo 2"), "4E")
        expectEq(TitleArt.initials("Burnout 3: Takedown"), "B3")
        expectEq(TitleArt.monogramColor("4D530004"), TitleArt.monogramColor("4d530004"))
        expect(TitleArt.monogramColor("4D530004") != TitleArt.monogramColor("41420001"))
        let h = TitleArt.titleHue("41420001")
        expect(h >= 0 && h < 360)
    }

    // ---- XBE info ----------------------------------------------------------------------------------------------
    check("xbe: title ID and name; corrupt input is nil") {
        let d = tempDir("xbe")
        let f = d.appendingPathComponent("default.xbe")
        write(f, Data(synthXbe(0x4D53_0004, "Sample Game")))
        let info = XbeInfo.read(f)
        expectEq(info?.titleId, "4D530004")
        expectEq(info?.title, "Sample Game")
        expectEq(info?.size, 0x400)
        // Truncated, wrong magic, certificate outside the file, random bytes, empty: nil, never a crash.
        var bad = synthXbe(1, "x")
        for (name, bytes) in [("short", Array(bad.prefix(0x100))), ("magic", { var b = bad; b[0] = 0; return b }()),
                              ("cert", { putLe32(&bad, 0x118, 0x7FFF_FFFF); return bad }()),
                              ("random", (0..<5000).map { _ in UInt8.random(in: 0...255) }), ("empty", [])] {
            let u = d.appendingPathComponent("\(name).xbe")
            write(u, Data(bytes))
            expectNil(XbeInfo.read(u), name)
            expectNil(XbeInfo.section(u, named: "$$XTIMAGE"), name)
            expectNil(TitleArt.decode(xbe: u), name)
        }
        // A section table pointing past the file.
        var sec = synthXbeWithSection(2, "y", "$$XTIMAGE", [1, 2, 3, 4])
        putLe32(&sec, 0x300 + 0x10, 0x7FFF_FFFF)
        let u = d.appendingPathComponent("sec.xbe")
        write(u, Data(sec))
        expectNil(XbeInfo.section(u, named: "$$XTIMAGE"))
        expectNil(XbeInfo.read(d.appendingPathComponent("missing.xbe")))
    }

    check("xbe: the repo's synthetic test.xbe") {
        let repo = URL(fileURLWithPath: #filePath).deletingLastPathComponent()   // Sources/xrecomp-check
            .deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent()
        let info = XbeInfo.read(repo.appendingPathComponent("tools/conformance/test.xbe"))
        expectEq(info?.titleId, "12345678")
        expectEq(info?.title, "SYNTH")
    }

    // ---- the scanner -------------------------------------------------------------------------------------------
    check("scan: folders with default.xbe in any case, images through identify") {
        let lib = tempDir("lib")
        write(lib.appendingPathComponent("Sample Game/default.xbe"), Data(synthXbe(0x4D53_0004, "Sample Game")))
        write(lib.appendingPathComponent("Other/game/DEFAULT.XBE"), Data(synthXbe(0x4142_0001, "Other")))
        write(lib.appendingPathComponent("Burnout (Europe)/Burnout (Europe) (Fr,De).iso"), "iso")
        write(lib.appendingPathComponent("Partial/Copying.xiso"), "half")
        write(lib.appendingPathComponent(".hidden/default.xbe"), Data(synthXbe(0x1, "Hidden")))
        write(lib.appendingPathComponent("notes.txt"), "x")
        var calls: [String] = []
        var s = LibraryScanner { url in
            calls.append(url.lastPathComponent)
            if url.pathExtension == "xiso" { return .failure(.init("Not readable yet (still being copied?)")) }
            return .success(Identity(titleId: "4541005B", title: "Burnout 3", xbe: nil))
        }
        var index = LibraryIndex()
        let entries = s.scan([lib], index: &index)
        let ids = Set(entries.compactMap(\.titleId))
        expectEq(ids, ["4D530004", "41420001", "4541005B"])
        expectEq(entries.count, 4, "the partial image is listed with its problem")
        let partial = entries.first { $0.path.hasSuffix("Copying.xiso") }
        expectEq(partial?.problem, "Not readable yet (still being copied?)")
        expectEq(partial?.displayName, "Copying")
        expectEq(entries.first { $0.titleId == "41420001" }?.kind, .folder)
        expect(entries.first { $0.titleId == "41420001" }?.xbe?.hasSuffix("DEFAULT.XBE") == true)
        expectEq(calls.sorted(), ["Burnout (Europe) (Fr,De).iso", "Copying.xiso"])
        expectNil(entries.first { $0.title == "Hidden" })

        // Rescan: the identified image comes from the cache, the partial one is asked again.
        calls = []
        _ = s.scan([lib], index: &index)
        expectEq(calls, ["Copying.xiso"])
        // A changed image (size and mtime) is identified again.
        write(lib.appendingPathComponent("Burnout (Europe)/Burnout (Europe) (Fr,De).iso"), "iso, now complete")
        calls = []
        _ = s.scan([lib], index: &index)
        expectEq(calls.sorted(), ["Burnout (Europe) (Fr,De).iso", "Copying.xiso"])
        // The cache survives a save and load, keyed by path.
        let f = lib.appendingPathComponent("library.json")
        try index.save(to: f)
        let back = LibraryIndex.load(from: f)
        expectEq(back, index)
        expectEq(back.entries.count, 3)
        write(f, "{broken")
        expectEq(LibraryIndex.load(from: f).entries.count, 0)
    }

    check("scan: depth and entry limits, a game folder is not descended") {
        let lib = tempDir("deep")
        write(lib.appendingPathComponent("a/b/c/default.xbe"), Data(synthXbe(0x3, "Depth3")))   // folder at depth 3
        write(lib.appendingPathComponent("a/b/c/d/default.xbe"), Data(synthXbe(0x4, "Depth4")))
        write(lib.appendingPathComponent("a/b/c/d/e/x.iso"), "x")
        write(lib.appendingPathComponent("g/default.xbe"), Data(synthXbe(0x5, "Game")))
        write(lib.appendingPathComponent("g/inner/default.xbe"), Data(synthXbe(0x6, "Inner")))
        var s = LibraryScanner { _ in .failure(.init("no")) }
        let found = s.findSources(in: [lib]).map { $0.0.lastPathComponent }
        expectEq(Set(found), ["c", "g"])
        var small = LibraryScanner { _ in .failure(.init("no")) }
        small.maxEntries = 3
        for i in 0..<10 { write(lib.appendingPathComponent("many/\(i).iso"), "x") }
        _ = small.findSources(in: [lib.appendingPathComponent("many")])
        expect(small.truncated)
        expectEq(small.findSources(in: [lib.appendingPathComponent("many")]).count, 3)
        // A missing library folder is not an error.
        expectEq(s.findSources(in: [lib.appendingPathComponent("nope")]).count, 0)
        // The library folder itself can be a game folder.
        expectEq(s.findSources(in: [lib.appendingPathComponent("g")]).count, 1)
    }

    check("scan: one entry per title ID, the first found wins") {
        let a = tempDir("libA"), b = tempDir("libB")
        write(a.appendingPathComponent("Black/default.xbe"), Data(synthXbe(0x4541_0058, "Black")))
        write(b.appendingPathComponent("black/game/default.xbe"), Data(synthXbe(0x4541_0058, "Black")))
        var s = LibraryScanner { _ in .failure(.init("no")) }
        var index = LibraryIndex()
        let e = s.scan([a, b], index: &index)
        expectEq(e.count, 1)
        expect(e.first?.path.hasPrefix(a.path) == true)
        expectEq(e.first?.duplicates.count, 1)
    }

    check("identify: JSON and error lines") {
        let j = """
        {"source": "/x/B.iso", "kind": "image", "size": 2966814720, "title_id": "4541005B", "name": "Burnout 3",
         "profile_name": "Burnout 3: Takedown", "version": 3, "xbe": "/s/a.xbe", "xbe_size": 4308992, "has_title_image": true}
        """
        let id = try JSONDecoder().decode(Identity.self, from: Data(j.utf8))
        expectEq(id.titleId, "4541005B")
        expectEq(id.title, "Burnout 3: Takedown", "the profile's name first")
        expectEq(id.size, 2966814720)
        expectEq(id.xbe, "/s/a.xbe")
        let k = try JSONDecoder().decode(Identity.self, from: Data(#"{"title_id": "0x4d530004", "name": "S", "profile_name": null}"#.utf8))
        expectEq(k.titleId, "4D530004")
        expectEq(k.title, "S")
        expectEq(LibraryScanner.identifyErrorMessage(#"{"error": {"code": "BAD_IMAGE", "msg": "ends inside default.xbe"}}"#,
                                                     status: 1), "Not readable yet (still being copied?)")
        expectEq(LibraryScanner.identifyErrorMessage(#"{"error": {"code": "NO_XBE", "msg": "no default.xbe"}}"#,
                                                     status: 1), "NO_XBE: no default.xbe")
        expectEq(LibraryScanner.stableHash("abc"), LibraryScanner.stableHash("abc"))
    }

    // ---- colours ------------------------------------------------------------------------------------------------
    check("theme: tokens equal GamePalette.STOCK") {
        // Copied from android/.../GamePalette.java STOCK; a change there must come here too.
        let java: [UInt32] = [0xFF0C110F, 0xFF060807, 0xE6111814, 0xFF18211C, 0xFF27332D, 0x6627332D, 0xFFF3EEE4,
                              0xFFFFFFFF, 0xFFA9AFA6, 0xFF6C746D, 0xFF4FE38C, 0xFF1F7A48, 0xFF143A26, 0xFF1A241F,
                              0xFF4FE38C, 0xFFF2BA52, 0xFFFF6F61, 0xFFEDE6D8, 0xFF0C110F, 0xFF4FE38C, 0xFF4FE38C]
        expectEq(Palette.stock, java)
        expectEq(Palette.names.count, java.count)
        expectEq(Palette.token("WARN"), 0xFFF2BA52)
        // The Java source itself, when the checkout has it.
        let repo = URL(fileURLWithPath: #filePath).deletingLastPathComponent().deletingLastPathComponent()
            .deletingLastPathComponent().deletingLastPathComponent()
        let src = read(repo.appendingPathComponent("android/app/src/main/java/fr/adserver/xrecomp/GamePalette.java"))
        if let r = src.range(of: #"STOCK = \{[^}]*\}"#, options: .regularExpression) {
            let vals = src[r].matches(of: #/0x([0-9A-Fa-f]{8})/#).map { UInt32($0.1, radix: 16)! }
            expectEq(vals, java, "GamePalette.java STOCK")
        }
    }

    check("theme: text contrast pairs") {
        for bg in [Palette.bgTop, Palette.bgBottom, Palette.panelHi] {
            expect(Palette.contrast(Palette.textDim, bg) >= 4.5, "dim text on \(String(bg, radix: 16))")
            expect(Palette.contrast(Palette.text, bg) >= 7)
            expect(Palette.contrast(Palette.warn, bg) >= 4.5)
            expect(Palette.contrast(Palette.error, bg) >= 4.5)
        }
        expect(Palette.contrast(Palette.glyphInk, Palette.accent) >= 4.5, "ink on Play")
        expect(Palette.contrast(Palette.accent, Palette.accentFace) >= 4.5, "accent text on its tinted face")
    }

    check("accent: every hue keeps the Play label readable") {
        for h in stride(from: 0.0, to: 360, by: 5) {
            let c = Accent.color(hue: h)
            expect(Palette.contrast(c, Palette.glyphInk) >= Accent.minInkContrast, "hue \(h)")
            expect(Palette.contrast(c, Palette.bgTop) >= Accent.minBackgroundContrast, "hue \(h)")
            let hue = Palette.toHsv(c).0
            let err = Palette.toHsv(Palette.error).0
            let d = abs((hue - err + 540).truncatingRemainder(dividingBy: 360) - 180)
            expect(d >= Accent.errorAvoidDegrees - 1.5, "hue \(h) -> \(hue) too close to ERROR")
        }
        // A blue image gives a blue accent; a grey one falls back to the title's hue.
        let blue = ArgbImage(width: 4, height: 4, pixels: [UInt32](repeating: 0xFF2050E0, count: 16))
        let hb = Accent.dominantHue(blue) ?? -1
        expect(hb > 215 && hb < 235, "blue hue \(hb)")
        let grey = ArgbImage(width: 4, height: 4, pixels: [UInt32](repeating: 0xFF808080, count: 16))
        expectNil(Accent.dominantHue(grey))
        expectEq(Accent.forTitle(image: grey, titleId: "4D530004"),
                 Accent.color(hue: Double(TitleArt.titleHue("4D530004"))))
    }
}
