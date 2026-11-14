import CoreGraphics
import Foundation
import ImageIO
import XRecompCore

/// « Create an app for this game » (TitleApp): names, the Info.plist transform, single-game detection, the ICNS
/// writer, the icon tile and the bundle copy, on a fake XRecomp.app made here (no signing: that needs a real binary).

/// Max per-channel distance of two ARGB colours (alpha ignored).
func colourDistance(_ a: UInt32, _ b: UInt32) -> Int {
    let s = [16, 8, 0].map { sh in abs(Int((a >> UInt32(sh)) & 0xFF) - Int((b >> UInt32(sh)) & 0xFF)) }
    return s.max() ?? 0
}

/// A fake app bundle: Info.plist, an executable script, an old icon.
func fakeApp(_ url: URL, info: [String: Any]) {
    let data = try! PropertyListSerialization.data(fromPropertyList: info, format: .xml, options: 0)
    write(url.appendingPathComponent("Contents/Info.plist"), data)
    write(url.appendingPathComponent("Contents/MacOS/XRecomp"), "#!/bin/sh\nexit 0\n", executable: true)
    write(url.appendingPathComponent("Contents/Resources/AppIcon.icns"), Data("old icon".utf8))
}

let mainInfo: [String: Any] = [
    "CFBundleName": "XRecomp", "CFBundleDisplayName": "XRecomp", "CFBundleIdentifier": "fr.adserver.xrecomp.mac",
    "CFBundleExecutable": "XRecomp", "CFBundleIconFile": "AppIcon", "CFBundleIconName": "AppIcon",
    "CFBundleShortVersionString": "0.1.0", "LSMinimumSystemVersion": "14.0", "XBAToolkitPath": "/Users/you/tk",
]

func registerTitleAppChecks() {
    check("title app: title ID, bundle ID and file name") {
        expectEq(TitleApp.normalizedTitleId(" 4d530004\n"), "4D530004")
        expectNil(TitleApp.normalizedTitleId("4D53000"))
        expectNil(TitleApp.normalizedTitleId("4D53000G"))
        expectNil(TitleApp.normalizedTitleId("4D5300041"))
        expectNil(TitleApp.normalizedTitleId(nil))
        expectEq(TitleApp.bundleIdentifier(titleId: "4D530004"), "fr.adserver.xrecomp.mac.title.4d530004")
        expectEq(TitleApp.bundleIdentifier(titleId: "4D/53 0004"), "fr.adserver.xrecomp.mac.title.4d530004")
        expectEq(TitleApp.bundleIdentifier(titleId: "é"), "fr.adserver.xrecomp.mac.title.unknown")
        expectEq(TitleApp.appName(title: "Burnout 3: Takedown"), "Burnout 3 - Takedown")
        expectEq(TitleApp.appName(title: "AC/DC:Live"), "AC-DC-Live")
        expectEq(TitleApp.appName(title: "  .hidden\tgame\n"), "hiddengame")
        expectEq(TitleApp.appName(title: "Égalité 2"), "Égalité 2")
        expectEq(TitleApp.appName(title: ""), "Game")
        expectEq(TitleApp.appName(title: "..."), "Game")
        expectEq(TitleApp.appName(title: String(repeating: "x", count: 300)).count, 120)
        expectEq(TitleApp.fileName(title: "Sample Game"), "Sample Game.app")
        for t in ["a/b", "a:b", "x: y/z", "\n/:"] {
            let n = TitleApp.fileName(title: t)
            expect(!n.contains("/") && !n.contains(":"), "\(n) has / or :")
        }
    }

    check("title app: Info.plist transform") {
        let p = TitleApp.infoPlist(mainInfo, title: "Sample: Game", titleId: "4d530004",
                                   mainAppPath: "/Applications/XRecomp.app", toolkitPath: nil)
        expectEq(p["CFBundleName"] as? String, "Sample - Game")
        expectEq(p["CFBundleDisplayName"] as? String, "Sample - Game")
        expectEq(p["CFBundleIdentifier"] as? String, "fr.adserver.xrecomp.mac.title.4d530004")
        expectEq(p["CFBundleIconFile"] as? String, "TitleIcon")
        expect(p["CFBundleIconName"] == nil, "the asset-catalog icon name is dropped")
        expectEq(p[TitleApp.singleTitleKey] as? String, "4D530004")
        expectEq(p[TitleApp.mainBundleIdKey] as? String, "fr.adserver.xrecomp.mac")
        expectEq(p[TitleApp.mainAppPathKey] as? String, "/Applications/XRecomp.app")
        expectEq(p["XBAToolkitPath"] as? String, "/Users/you/tk", "kept")
        expectEq(p["CFBundleExecutable"] as? String, "XRecomp")
        expectEq(p["CFBundleShortVersionString"] as? String, "0.1.0")
        expectEq(p["LSMinimumSystemVersion"] as? String, "14.0")
        expectEq(mainInfo["CFBundleIdentifier"] as? String, "fr.adserver.xrecomp.mac", "the input is untouched")
        let q = TitleApp.infoPlist(mainInfo, title: "G", titleId: "4D530004", mainAppPath: nil, toolkitPath: "/src/tk")
        expectEq(q["XBAToolkitPath"] as? String, "/src/tk", "the toolkit in use wins")
        expect(q[TitleApp.mainAppPathKey] == nil)
        // From a game app's own plist: the main app's bundle ID is carried, not the game app's.
        let r = TitleApp.infoPlist(p, title: "Other", titleId: "41420001", mainAppPath: nil, toolkitPath: nil)
        expectEq(r[TitleApp.mainBundleIdKey] as? String, "fr.adserver.xrecomp.mac")
        expectEq(r[TitleApp.singleTitleKey] as? String, "41420001")
        expectNotNil(try? PropertyListSerialization.data(fromPropertyList: p, format: .xml, options: 0), "serialisable")
    }

    check("title app: single-game detection") {
        expectEq(TitleApp.singleTitle(info: [TitleApp.singleTitleKey: "4d530004"]), "4D530004")
        expectNil(TitleApp.singleTitle(info: nil))
        expectNil(TitleApp.singleTitle(info: mainInfo), "the main app")
        expectNil(TitleApp.singleTitle(info: [TitleApp.singleTitleKey: "bogus"]))
        expectNil(TitleApp.singleTitle(info: [TitleApp.singleTitleKey: 0x4D530004]), "not a string")
        expectEq(TitleApp.singleTitle(info: [TitleApp.singleTitleKey: "4D530004"],
                                      environment: [TitleApp.environmentKey: "12345678"]), "12345678")
        expectEq(TitleApp.singleTitle(info: [TitleApp.singleTitleKey: "4D530004"],
                                      environment: [TitleApp.environmentKey: ""]), "4D530004")
        expectEq(TitleApp.singleTitle(info: nil, environment: [TitleApp.environmentKey: "abcdef01"]), "ABCDEF01")
    }

    check("title app: ICNS layout and parse") {
        let one = TitleApp.icns([TitleApp.IcnsEntry(type: "ic07", data: Data([1, 2, 3]))])
        expectEq([UInt8](one), Array("icns".utf8) + [0, 0, 0, 19] + Array("ic07".utf8) + [0, 0, 0, 11, 1, 2, 3])
        let entries = [TitleApp.IcnsEntry(type: "ic10", data: Data(repeating: 7, count: 300)),
                       TitleApp.IcnsEntry(type: "ic11", data: Data())]
        let two = TitleApp.icns(entries)
        expectEq(two.count, 8 + 8 + 300 + 8)
        expectEq(TitleApp.icnsEntries(two), entries)
        var bad = [UInt8](two)
        bad[7] &+= 1   // total length off by one
        expectNil(TitleApp.icnsEntries(Data(bad)))
        var badEntry = [UInt8](two)
        badEntry[15] = 4   // an entry shorter than its header
        badEntry[14] = 0
        expectNil(TitleApp.icnsEntries(Data(badEntry)))
        expectNil(TitleApp.icnsEntries(Data("nope".utf8)))
        expectEq(TitleApp.icnsEntries(TitleApp.icns([]))?.count, 0)
    }

    check("title app: tile background from the image border, else the accent") {
        var px = [UInt32](repeating: 0xFF20_40C0, count: 16 * 16)
        for y in 4..<12 { for x in 4..<12 { px[y * 16 + x] = 0xFFC0_3020 } }
        let framed = ArgbImage(width: 16, height: 16, pixels: px)
        expectEq(TitleApp.tileBackground(framed, accent: 0xFF4F_E38C), 0xFF20_40C0)
        let clear = ArgbImage(width: 16, height: 16, pixels: [UInt32](repeating: 0x0000_0000, count: 256))
        let fromAccent = TitleApp.tileBackground(clear, accent: 0xFF4F_E38C)
        expectEq(fromAccent, TitleApp.tileBackground(nil, accent: 0xFF4F_E38C))
        let (h, _, v) = Palette.toHsv(fromAccent)
        expect(abs(h - Palette.toHsv(0xFF4F_E38C).0) < 6, "the accent's hue, got \(h)")
        expect(v < 0.3, "a deep shade, got v \(v)")
    }

    check("title app: icon tile with the title image") {
        var px = [UInt32](repeating: 0xFF20_40C0, count: 16 * 16)
        for y in 4..<12 { for x in 4..<12 { px[y * 16 + x] = 0xFFC0_3020 } }
        guard let art = expectNotNil(TitleArt.cgImage(ArgbImage(width: 16, height: 16, pixels: px))),
              let icon = expectNotNil(TitleApp.iconImage(art: art, title: "Sample Game", titleId: "4D530004",
                                                         accent: 0xFF4F_E38C, side: 256)),
              let a = expectNotNil(TitleArt.argb(icon, side: 256)) else { return }
        expectEq(icon.width, 256)
        expectEq(icon.height, 256)
        expect(a[2, 2] >> 24 < 16, "outside the tile: transparent, got \(String(a[2, 2], radix: 16))")
        expectEq(a[128, 128] >> 24, 255)
        expect(colourDistance(a[128, 128], 0xFFC0_3020) < 24, "the image's centre, got \(String(a[128, 128], radix: 16))")
        // Inside the tile, outside the image (80 % of the tile): the border colour.
        let tile = TitleApp.tileRect(side: 256)
        let y = Int(tile.minY) + 6
        expectEq(a[128, y] >> 24, 255)
        expect(colourDistance(a[128, y], 0xFF20_40C0) < 24, "the tile, got \(String(a[128, y], radix: 16))")
    }

    check("title app: monogram tile without an image") {
        guard let icon = expectNotNil(TitleApp.iconImage(art: nil, title: "Sample Game", titleId: "4D530004",
                                                         accent: 0xFF4F_E38C, side: 256)),
              let a = expectNotNil(TitleArt.argb(icon, side: 256)) else { return }
        expect(a[2, 2] >> 24 < 16, "outside the tile: transparent")
        let tile = TitleApp.tileRect(side: 256)
        expectEq(a[128, Int(tile.minY) + 6] >> 24, 255, "the tile is opaque")
        let white = a.pixels.filter { p in p >> 24 == 255 && colourDistance(p, 0xFFFF_FFFF) < 25 }.count
        expect(white > 200, "the initials are drawn (\(white) white pixels)")
        // Another title ID, another hue.
        guard let other = TitleApp.iconImage(art: nil, title: "Sample Game", titleId: "41420001", accent: 0xFF4F_E38C,
                                             side: 256),
              let b = TitleArt.argb(other, side: 256) else { return expect(false, "no second icon") }
        let y = Int(tile.minY) + 6
        expect(colourDistance(a[128, y], b[128, y]) > 4, "the tile follows the title's hue")
    }

    check("title app: ICNS of every size from the 1024 master") {
        guard let icon = expectNotNil(TitleApp.iconImage(art: nil, title: "Sample Game", titleId: "4D530004",
                                                         accent: 0xFF4F_E38C)),
              let data = expectNotNil(TitleApp.icns(master: icon)),
              let entries = expectNotNil(TitleApp.icnsEntries(data)) else { return }
        expectEq(icon.width, 1024)
        expectEq(entries.map(\.type), ["ic10", "ic09", "ic08", "ic07", "ic14", "ic13", "ic12", "ic11"])
        for (e, want) in zip(entries, TitleApp.icnsTypes) {
            expectEq(Array(e.data.prefix(8)), [0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A], "\(e.type) is a PNG")
            let src = CGImageSourceCreateWithData(e.data as CFData, nil)
            let img = src.flatMap { CGImageSourceCreateImageAtIndex($0, 0, nil) }
            expectEq(img?.width, want.pixels, e.type)
            expectEq(img?.height, want.pixels, e.type)
        }
    }

    check("title app: bundle copy, plist, icon; replace ours only; never into the source") {
        let d = tempDir("titleapp")
        let src = d.appendingPathComponent("XRecomp.app", isDirectory: true)
        fakeApp(src, info: mainInfo)
        let icns = TitleApp.icns([TitleApp.IcnsEntry(type: "ic07", data: Data([9, 9]))])
        let dest = d.appendingPathComponent("Apps/Sample - Game.app", isDirectory: true)
        try TitleApp.build(source: src, dest: dest, title: "Sample: Game", titleId: "4d530004", icns: icns,
                           toolkitPath: "/src/tk")
        let info = TitleApp.readInfo(dest) ?? [:]
        expectEq(info["CFBundleName"] as? String, "Sample - Game")
        expectEq(info["CFBundleIdentifier"] as? String, "fr.adserver.xrecomp.mac.title.4d530004")
        expectEq(info[TitleApp.singleTitleKey] as? String, "4D530004")
        expectEq(info[TitleApp.mainAppPathKey] as? String, src.standardizedFileURL.path)
        expectEq(info["XBAToolkitPath"] as? String, "/src/tk")
        expect(TitleApp.isTitleApp(dest))
        expect(!TitleApp.isTitleApp(src), "the source keeps its plist")
        expectEq(try? Data(contentsOf: dest.appendingPathComponent("Contents/Resources/TitleIcon.icns")), icns)
        expect(FileManager.default.isExecutableFile(atPath: dest.appendingPathComponent("Contents/MacOS/XRecomp").path))
        expect(!FileManager.default.fileExists(atPath: src.appendingPathComponent("Contents/Resources/TitleIcon.icns").path))

        // Made again over itself: replaced.
        try TitleApp.build(source: src, dest: dest, title: "Renamed", titleId: "4D530004", icns: icns, toolkitPath: nil)
        expectEq(TitleApp.readInfo(dest)?["CFBundleName"] as? String, "Renamed")
        expectEq(TitleApp.readInfo(dest)?["XBAToolkitPath"] as? String, "/Users/you/tk")

        func refused(_ to: URL, _ want: TitleApp.BuildError, line: Int = #line) {
            do {
                try TitleApp.build(source: src, dest: to, title: "X", titleId: "4D530004", icns: icns, toolkitPath: nil)
                expect(false, "\(to.lastPathComponent) should be refused", line: line)
            } catch let e as TitleApp.BuildError {
                expectEq(e, want, line: line)
            } catch {
                expect(false, "unexpected \(error)", line: line)
            }
        }
        // Someone else's app of that name stays.
        let other = d.appendingPathComponent("Apps/Other.app", isDirectory: true)
        fakeApp(other, info: ["CFBundleIdentifier": "com.example.other"])
        refused(other, .notOurs(other.standardizedFileURL.path))
        expect(FileManager.default.fileExists(atPath: other.appendingPathComponent("Contents/Info.plist").path))
        // Never over or inside the running app.
        refused(src, .insideSource)
        refused(src.appendingPathComponent("Contents/Inner.app"), .insideSource)
        expect(!TitleApp.isTitleApp(src), "the source is intact")
        let noExt = d.appendingPathComponent("Apps/NoExtension")
        refused(noExt, .notAnApp(noExt.standardizedFileURL.path))
        do {
            try TitleApp.build(source: d.appendingPathComponent("nothing.app"), dest: d.appendingPathComponent("y.app"),
                               title: "X", titleId: "4D530004", icns: icns, toolkitPath: nil)
            expect(false, "a missing source is refused")
        } catch {}
    }
}
