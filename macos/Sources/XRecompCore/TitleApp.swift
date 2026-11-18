import CoreGraphics
import CoreText
import Foundation
import ImageIO
import UniformTypeIdentifiers

/// « Create an app for this game » (docs/macos/launcher.md « Game apps »): `<Game title>.app`, a copy of the running
/// XRecomp.app with its own name, bundle ID and icon (the title image on a macOS tile, or the monogram) and
/// `XBASingleTitle` in its Info.plist, which opens the launcher on that one game. The copy holds no game code or
/// data: it points at the library entry by title ID and shares the main app's Application Support tree.
///
/// Everything here is pure or plain file work, so `xrecomp-check` covers it; the app adds the save panel and
/// `codesign`.
public enum TitleApp {
    /// Info.plist key: the title ID the app is for (8 upper-case hex digits). Present = single-game mode.
    public static let singleTitleKey = "XBASingleTitle"
    /// Info.plist keys written into a game app: the XRecomp.app it was made from, and that app's bundle ID, for
    /// « Open XRecomp ».
    public static let mainAppPathKey = "XBAMainAppPath"
    public static let mainBundleIdKey = "XBAMainBundleIdentifier"
    public static let toolkitKey = "XBAToolkitPath"
    /// The main app's bundle ID (macos/Info.plist.in).
    public static let mainBundleId = "fr.adserver.xrecomp.mac"
    public static let bundleIdPrefix = "fr.adserver.xrecomp.mac.title."
    /// The icon in Contents/Resources (`TitleIcon.icns`), named by CFBundleIconFile.
    public static let iconBaseName = "TitleIcon"
    /// `XRECOMP_SINGLE_TITLE=TID` gives single-game mode to any launcher run (`swift run XRecomp`, tests).
    public static let environmentKey = "XRECOMP_SINGLE_TITLE"

    // ---- names -----------------------------------------------------------------------------------------------------

    /// « 4d530004 » -> « 4D530004 »; nil unless 8 hex digits.
    public static func normalizedTitleId(_ s: String?) -> String? {
        if let game = Console.normalizedGameId(s) { return game } // a console game's app (XBA-278)
        guard let t = s?.trimmingCharacters(in: .whitespacesAndNewlines).uppercased(), t.utf8.count == 8 else {
            return nil
        }
        let hex = Set("0123456789ABCDEF".utf8)
        return t.utf8.allSatisfy { hex.contains($0) } ? t : nil
    }

    /// `fr.adserver.xrecomp.mac.title.<titleid lower case>`; anything but [a-z0-9-] is dropped.
    public static func bundleIdentifier(titleId: String) -> String {
        let allowed = Set("abcdefghijklmnopqrstuvwxyz0123456789-".unicodeScalars)
        var safe = ""
        for u in titleId.lowercased().unicodeScalars where allowed.contains(u) { safe.unicodeScalars.append(u) }
        return bundleIdPrefix + (safe.isEmpty ? "unknown" : safe)
    }

    /// The game title as a Finder name: « A: B » becomes « A - B », any other `/` and `:` a `-`, control characters
    /// go, no leading dot, at most 120 characters; « Game » when nothing is left.
    public static func appName(title: String) -> String {
        var s = ""
        for ch in title.replacingOccurrences(of: ": ", with: " - ") {
            if ch == "/" || ch == ":" {
                s.append("-")
            } else if ch.unicodeScalars.allSatisfy({ !CharacterSet.controlCharacters.contains($0) }) {
                s.append(ch)
            }
        }
        s = s.trimmingCharacters(in: .whitespacesAndNewlines)
        while s.hasPrefix(".") { s.removeFirst() }
        s = s.trimmingCharacters(in: .whitespaces)
        if s.count > 120 { s = String(s.prefix(120)).trimmingCharacters(in: .whitespaces) }
        return s.isEmpty ? "Game" : s
    }

    /// `<appName>.app`, the save panel's default name.
    public static func fileName(title: String) -> String { appName(title: title) + ".app" }

    // ---- Info.plist ------------------------------------------------------------------------------------------------

    /// The game app's Info.plist from the main app's: name and display name = the title, its own bundle ID, the new
    /// icon, `XBASingleTitle`, where the main app is. `XBAToolkitPath` is kept, or set to `toolkitPath` (the toolkit
    /// the main app uses right now) when one is given, so the copy outside the checkout still finds it. The rest
    /// (executable, version, minimum system) is unchanged.
    public static func infoPlist(_ base: [String: Any], title: String, titleId: String, mainAppPath: String?,
                                 toolkitPath: String?) -> [String: Any] {
        var p = base
        let tid = normalizedTitleId(titleId) ?? titleId.uppercased()
        let name = appName(title: title)
        p["CFBundleName"] = name
        p["CFBundleDisplayName"] = name
        p["CFBundleIdentifier"] = bundleIdentifier(titleId: tid)
        p["CFBundleIconFile"] = iconBaseName
        p.removeValue(forKey: "CFBundleIconName")   // an asset-catalog icon would win over the file
        p[singleTitleKey] = tid
        p[mainBundleIdKey] = (base[mainBundleIdKey] as? String) ?? (base["CFBundleIdentifier"] as? String) ?? mainBundleId
        if let mainAppPath, !mainAppPath.isEmpty { p[mainAppPathKey] = mainAppPath }
        if let toolkitPath, !toolkitPath.isEmpty { p[toolkitKey] = toolkitPath }
        return p
    }

    /// The title ID this launcher run is for, or nil (the whole library): `$XRECOMP_SINGLE_TITLE`, else the
    /// Info.plist's `XBASingleTitle`. A value that is not a title ID counts as none.
    public static func singleTitle(info: [String: Any]?, environment: [String: String] = [:]) -> String? {
        if let e = normalizedTitleId(environment[environmentKey]) { return e }
        return normalizedTitleId(info?[singleTitleKey] as? String)
    }

    /// The Info.plist of an .app, or nil.
    public static func readInfo(_ app: URL) -> [String: Any]? {
        guard let d = try? Data(contentsOf: app.appendingPathComponent("Contents/Info.plist")) else { return nil }
        return (try? PropertyListSerialization.propertyList(from: d, options: [], format: nil)) as? [String: Any]
    }

    /// True for a game app this launcher made (its Info.plist has `XBASingleTitle`).
    public static func isTitleApp(_ app: URL) -> Bool { singleTitle(info: readInfo(app)) != nil }

    // ---- icon --------------------------------------------------------------------------------------------------

    public static let iconSide = 1024

    /// The tile on the macOS icon grid: 824 of 1024 pixels, centred.
    public static func tileRect(side: Int) -> CGRect {
        let s = CGFloat(side), inset = s * 100 / 1024
        return CGRect(x: inset, y: inset, width: s - 2 * inset, height: s - 2 * inset)
    }

    /// The tile's colour behind a title image: the average of its border when at least half of the border is
    /// opaque (the image then melts into the tile), else a deep shade of the game's accent.
    public static func tileBackground(_ img: ArgbImage?, accent: UInt32) -> UInt32 {
        if let img, img.width > 1, img.height > 1, img.pixels.count == img.width * img.height {
            var r = 0.0, g = 0.0, b = 0.0, n = 0.0, total = 0.0
            func add(_ p: UInt32) {
                total += 1
                guard p >> 24 >= 128 else { return }
                let (pr, pg, pb) = Palette.rgb(p)
                r += pr
                g += pg
                b += pb
                n += 1
            }
            for x in 0..<img.width {
                add(img[x, 0])
                add(img[x, img.height - 1])
            }
            for y in 1..<(img.height - 1) {
                add(img[0, y])
                add(img[img.width - 1, y])
            }
            if n > 0 && n >= total * 0.5 { return Palette.argb(r / n, g / n, b / n) }
        }
        let (h, s, _) = Palette.toHsv(accent)
        return Palette.hsv(h, min(s, 0.6), 0.22)
    }

    static func cgColor(_ c: UInt32, alpha: Double? = nil) -> CGColor {
        let (r, g, b) = Palette.rgb(c)
        return CGColor(srgbRed: r, green: g, blue: b, alpha: alpha ?? Palette.alpha(c))
    }

    static func bitmap(_ side: Int) -> CGContext? {
        CGContext(data: nil, width: side, height: side, bitsPerComponent: 8, bytesPerRow: 0,
                  space: CGColorSpace(name: CGColorSpace.sRGB)!,
                  bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)
    }

    /// The game app's icon, `side` pixels square: a rounded macOS tile with a soft shadow; on it the title image
    /// scaled (high-quality interpolation) to 80 % of the tile over `tileBackground`, or, without an image, the
    /// launcher's monogram (the initials on the title ID's hue, as the Library cards draw it).
    public static func iconImage(art: CGImage?, title: String, titleId: String?, accent: UInt32,
                                 side: Int = iconSide) -> CGImage? {
        guard side >= 16, let ctx = bitmap(side) else { return nil }
        let s = CGFloat(side)
        let tile = tileRect(side: side)
        let radius = tile.width * 0.2245
        let shape = CGPath(roundedRect: tile, cornerWidth: radius, cornerHeight: radius, transform: nil)
        let base = art.map { tileBackground(TitleArt.argb($0, side: 128), accent: accent) } ?? Palette.bgTop

        ctx.saveGState()
        ctx.setShadow(offset: CGSize(width: 0, height: -s * 0.01), blur: s * 0.03,
                      color: CGColor(srgbRed: 0, green: 0, blue: 0, alpha: 0.35))
        ctx.addPath(shape)
        ctx.setFillColor(cgColor(base, alpha: 1))
        ctx.fillPath()
        ctx.restoreGState()

        ctx.saveGState()
        ctx.addPath(shape)
        ctx.clip()
        if let art, art.width > 0, art.height > 0 {
            ctx.interpolationQuality = .high
            let box = tile.width * 0.8
            let aspect = CGFloat(art.width) / CGFloat(art.height)
            let w = aspect >= 1 ? box : box * aspect, h = aspect >= 1 ? box / aspect : box
            ctx.draw(art, in: CGRect(x: tile.midX - w / 2, y: tile.midY - h / 2, width: w, height: h))
        } else {
            let hue = TitleArt.monogramColor(titleId)
            let space = CGColorSpace(name: CGColorSpace.sRGB)!
            if let grad = CGGradient(colorsSpace: space,
                                     colors: [cgColor(hue, alpha: 0.55), cgColor(hue, alpha: 0.18)] as CFArray,
                                     locations: [0, 1]) {
                // Top left to bottom right (CoreGraphics' y goes up).
                ctx.drawLinearGradient(grad, start: CGPoint(x: tile.minX, y: tile.maxY),
                                       end: CGPoint(x: tile.maxX, y: tile.minY), options: [])
            }
            drawInitials(TitleArt.initials(title), in: tile, ctx: ctx)
        }
        ctx.restoreGState()
        return ctx.makeImage()
    }

    static func drawInitials(_ text: String, in tile: CGRect, ctx: CGContext) {
        guard !text.isEmpty else { return }
        let size = tile.width * 0.38
        let plain = CTFontCreateUIFontForLanguage(.system, size, nil) ?? CTFontCreateWithName("Helvetica" as CFString, size, nil)
        let font = CTFontCreateCopyWithSymbolicTraits(plain, size, nil, .traitBold, .traitBold) ?? plain
        let attrs: [NSAttributedString.Key: Any] = [
            NSAttributedString.Key(kCTFontAttributeName as String): font,
            NSAttributedString.Key(kCTForegroundColorAttributeName as String): cgColor(Palette.textBright),
        ]
        let line = CTLineCreateWithAttributedString(NSAttributedString(string: text, attributes: attrs) as CFAttributedString)
        let b = CTLineGetBoundsWithOptions(line, .useGlyphPathBounds)
        ctx.textMatrix = .identity
        ctx.textPosition = CGPoint(x: tile.midX - b.width / 2 - b.minX, y: tile.midY - b.height / 2 - b.minY)
        CTLineDraw(line, ctx)
    }

    /// img redrawn at side x side with high-quality interpolation.
    public static func resized(_ img: CGImage, side: Int) -> CGImage? {
        if img.width == side && img.height == side { return img }
        guard let ctx = bitmap(side) else { return nil }
        ctx.interpolationQuality = .high
        ctx.draw(img, in: CGRect(x: 0, y: 0, width: side, height: side))
        return ctx.makeImage()
    }

    public static func pngData(_ img: CGImage) -> Data? {
        let out = NSMutableData()
        guard let dest = CGImageDestinationCreateWithData(out as CFMutableData, UTType.png.identifier as CFString, 1,
                                                          nil) else { return nil }
        CGImageDestinationAddImage(dest, img, nil)
        guard CGImageDestinationFinalize(dest) else { return nil }
        return out as Data
    }

    // ---- ICNS ----------------------------------------------------------------------------------------------------

    /// One element of an .icns file: a 4-character type and its data (a PNG for every type written here).
    public struct IcnsEntry: Equatable, Sendable {
        public let type: String
        public let data: Data

        public init(type: String, data: Data) {
            self.type = type
            self.data = data
        }
    }

    /// The PNG types written, with their pixel size: ic10 1024 (512@2x), ic09 512, ic08 256, ic07 128,
    /// ic14 512 (256@2x), ic13 256 (128@2x), ic12 64 (32@2x), ic11 32 (16@2x).
    public static let icnsTypes: [(type: String, pixels: Int)] = [
        ("ic10", 1024), ("ic09", 512), ("ic08", 256), ("ic07", 128),
        ("ic14", 512), ("ic13", 256), ("ic12", 64), ("ic11", 32),
    ]

    /// The .icns bytes: « icns », the total length (big-endian, header included), then per entry its type, its length
    /// (big-endian, its 8-byte header included) and its data. Types must be 4 ASCII characters.
    public static func icns(_ entries: [IcnsEntry]) -> Data {
        var out = Data()
        func be32(_ v: Int) {
            let u = UInt32(truncatingIfNeeded: v).bigEndian
            withUnsafeBytes(of: u) { out.append(contentsOf: $0) }
        }
        out.append(contentsOf: Array("icns".utf8))
        be32(8 + entries.reduce(0) { $0 + 8 + $1.data.count })
        for e in entries {
            var t = Array(e.type.utf8.prefix(4))
            while t.count < 4 { t.append(0x20) }
            out.append(contentsOf: t)
            be32(8 + e.data.count)
            out.append(e.data)
        }
        return out
    }

    /// The entries of .icns bytes, or nil when the file is malformed (a length that does not add up).
    public static func icnsEntries(_ data: Data) -> [IcnsEntry]? {
        let b = [UInt8](data)
        guard b.count >= 8, Array(b[0..<4]) == Array("icns".utf8) else { return nil }
        func be(_ o: Int) -> Int { Int(b[o]) << 24 | Int(b[o + 1]) << 16 | Int(b[o + 2]) << 8 | Int(b[o + 3]) }
        guard be(4) == b.count else { return nil }
        var out: [IcnsEntry] = []
        var o = 8
        while o < b.count {
            guard o + 8 <= b.count else { return nil }
            let len = be(o + 4)
            guard len >= 8, o + len <= b.count else { return nil }
            out.append(IcnsEntry(type: String(decoding: b[o..<(o + 4)], as: UTF8.self),
                                 data: Data(b[(o + 8)..<(o + len)])))
            o += len
        }
        return out
    }

    /// The .icns of `master` (ideally 1024 square) at every size of `icnsTypes`, or nil when a PNG cannot be made.
    public static func icns(master: CGImage) -> Data? {
        var pngs: [Int: Data] = [:]
        var entries: [IcnsEntry] = []
        for (type, px) in icnsTypes {
            if pngs[px] == nil {
                guard let img = resized(master, side: px), let png = pngData(img) else { return nil }
                pngs[px] = png
            }
            entries.append(IcnsEntry(type: type, data: pngs[px]!))
        }
        return icns(entries)
    }

    // ---- the bundle ------------------------------------------------------------------------------------------------

    public enum BuildError: Error, CustomStringConvertible, Equatable {
        case notAnApp(String)
        case insideSource
        case notOurs(String)
        case badInfoPlist
        case icon

        public var description: String {
            switch self {
            case .notAnApp(let p): return "\(p) is not an application bundle (.app)"
            case .insideSource: return "the game app cannot replace XRecomp.app or go inside it"
            case .notOurs(let p): return "\(p) already exists and is not a game app made by XRecomp; choose another name"
            case .badInfoPlist: return "the Info.plist of XRecomp.app cannot be read"
            case .icon: return "the icon could not be drawn"
            }
        }
    }

    /// Copies the app bundle `source` to `dest` (an existing game app there is replaced; anything else is refused),
    /// rewrites its Info.plist (`infoPlist`) and writes `Contents/Resources/TitleIcon.icns`. It does not sign: the
    /// edit breaks the copied signature, `sign` makes a new one. On an error nothing is left at `dest`.
    public static func build(source: URL, dest: URL, title: String, titleId: String, icns: Data,
                             toolkitPath: String?) throws {
        let fm = FileManager.default
        let src = source.standardizedFileURL
        let dst = dest.standardizedFileURL
        guard src.pathExtension == "app", fm.fileExists(atPath: src.appendingPathComponent("Contents/Info.plist").path)
        else { throw BuildError.notAnApp(src.path) }
        guard dst.pathExtension == "app" else { throw BuildError.notAnApp(dst.path) }
        let srcReal = src.resolvingSymlinksInPath().path + "/"
        let dstReal = dst.deletingLastPathComponent().resolvingSymlinksInPath()
            .appendingPathComponent(dst.lastPathComponent).path + "/"
        if dstReal.hasPrefix(srcReal) || srcReal.hasPrefix(dstReal) { throw BuildError.insideSource }
        if fm.fileExists(atPath: dst.path) {
            guard isTitleApp(dst) else { throw BuildError.notOurs(dst.path) }
            try fm.removeItem(at: dst)
        }
        try fm.ensureDirectory(dst.deletingLastPathComponent())
        try fm.copyItem(at: src, to: dst)
        do {
            guard let base = readInfo(dst) else { throw BuildError.badInfoPlist }
            let info = infoPlist(base, title: title, titleId: titleId, mainAppPath: src.path, toolkitPath: toolkitPath)
            let data = try PropertyListSerialization.data(fromPropertyList: info, format: .xml, options: 0)
            try data.write(to: dst.appendingPathComponent("Contents/Info.plist"), options: .atomic)
            let res = dst.appendingPathComponent("Contents/Resources", isDirectory: true)
            try fm.ensureDirectory(res)
            try icns.write(to: res.appendingPathComponent(iconBaseName + ".icns"), options: .atomic)
            // A fresh date, so Finder and the Dock read the new icon rather than one cached for this path.
            try? fm.setAttributes([.modificationDate: Date()], ofItemAtPath: dst.path)
        } catch {
            try? fm.removeItem(at: dst)
            throw error
        }
    }

    /// Ad-hoc signature of the whole bundle: `codesign --force --deep --sign - <app>`.
    public static func sign(_ app: URL) -> Shell.Result {
        Shell.run("/usr/bin/codesign", ["--force", "--deep", "--sign", "-", app.path], timeout: 120)
    }
}
