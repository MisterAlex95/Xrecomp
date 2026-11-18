import Foundation

/// One game the scan found.
public struct LibraryEntry: Codable, Equatable, Sendable, Identifiable {
    public enum Kind: String, Codable, Sendable {
        case folder   // a folder holding default.xbe (an extracted disc)
        case image    // a disc image (.iso, .xiso, .cso, .ciso, .zso, .cci, or a split part)
        /// A GBA / PS1 / N64 / GameCube game's file (a ROM, a disc or a DOL), identified by the xrecomp command line
        /// (ConsoleGames.swift, XBA-278).
        case console
    }

    /// The source path, which prepare takes.
    public var path: String
    public var kind: Kind
    public var size: Int64
    public var mtime: Double
    public var titleId: String?
    public var title: String?
    /// A default.xbe to read the title image from: the folder's own, or the one identify pulled out of the image.
    public var xbe: String?
    /// Why the source could not be identified (a partial copy, a broken image); retried at the next scan.
    public var problem: String?
    /// Other sources of the same title ID (the first one found is the entry).
    public var duplicates: [String] = []
    /// A console game's console (« gba », « ps1 », « n64 », « gc »); nil for an Xbox title. Its titleId is then the
    /// game ID (« gba-AXVE-1A2B3C4D »).
    public var console: String?

    public var id: String { titleId ?? path }
    /// The game's console (Xbox when none is recorded).
    public var consoleKind: Console { console.flatMap(Console.init(rawValue:)) ?? .xbox }
    public var isConsole: Bool { consoleKind != .xbox }

    /// The name to show: the XBE's, else the file or folder name.
    public var displayName: String {
        if let t = title, !t.isEmpty { return t }
        let u = URL(fileURLWithPath: path)
        return kind == .folder ? u.lastPathComponent : u.deletingPathExtension().lastPathComponent
    }

    enum CodingKeys: String, CodingKey {
        case path, kind, size, mtime, titleId = "title_id", title, xbe, problem, duplicates, console
    }

    public init(path: String, kind: Kind, size: Int64, mtime: Double, titleId: String? = nil, title: String? = nil,
                xbe: String? = nil, problem: String? = nil, console: String? = nil) {
        self.console = console
        self.path = path
        self.kind = kind
        self.size = size
        self.mtime = mtime
        self.titleId = titleId
        self.title = title
        self.xbe = xbe
        self.problem = problem
    }

    public init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        path = try c.decode(String.self, forKey: .path)
        kind = try c.decode(Kind.self, forKey: .kind)
        size = try c.decodeIfPresent(Int64.self, forKey: .size) ?? 0
        mtime = try c.decodeIfPresent(Double.self, forKey: .mtime) ?? 0
        titleId = try c.decodeIfPresent(String.self, forKey: .titleId)
        title = try c.decodeIfPresent(String.self, forKey: .title)
        xbe = try c.decodeIfPresent(String.self, forKey: .xbe)
        problem = try c.decodeIfPresent(String.self, forKey: .problem)
        duplicates = try c.decodeIfPresent([String].self, forKey: .duplicates) ?? []
        console = try c.decodeIfPresent(String.self, forKey: .console)
    }
}

/// What identify says about a disc image (`python -m tools.prepare.identify SOURCE --json --xbe-out FILE`).
public struct Identity: Codable, Equatable, Sendable {
    public var titleId: String
    public var title: String
    public var size: Int64?
    /// The extracted default.xbe, when asked for.
    public var xbe: String?

    public init(titleId: String, title: String, size: Int64? = nil, xbe: String? = nil) {
        self.titleId = titleId
        self.title = title
        self.size = size
        self.xbe = xbe
    }

    enum CodingKeys: String, CodingKey {
        case titleId = "title_id", title, name, profileName = "profile_name", size, xbe, xbeOut = "xbe_out"
    }

    public init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        let raw = try c.decode(String.self, forKey: .titleId)
        titleId = Identity.normalise(raw)
        // The title profile's name first (identify: « show it in preference to name »).
        let names = [CodingKeys.profileName, .title, .name].compactMap { k in
            (try? c.decodeIfPresent(String.self, forKey: k)) ?? nil
        }
        title = names.first { !$0.isEmpty } ?? ""
        size = try? c.decodeIfPresent(Int64.self, forKey: .size)
        xbe = (try? c.decodeIfPresent(String.self, forKey: .xbe)) ?? (try? c.decodeIfPresent(String.self, forKey: .xbeOut))
    }

    public func encode(to encoder: Encoder) throws {
        var c = encoder.container(keyedBy: CodingKeys.self)
        try c.encode(titleId, forKey: .titleId)
        try c.encode(title, forKey: .title)
        try c.encodeIfPresent(size, forKey: .size)
        try c.encodeIfPresent(xbe, forKey: .xbe)
    }

    /// "0x4d530004" / "4d530004" -> "4D530004".
    public static func normalise(_ s: String) -> String {
        var t = s.trimmingCharacters(in: .whitespaces).uppercased()
        if t.hasPrefix("0X") { t.removeFirst(2) }
        return t
    }
}

/// Finds games in the library folders, by content rather than by name: a folder holding default.xbe (any case) is a
/// game, so is a disc image the pipeline reads (isImageName). Hidden entries and packages are skipped; the walk stops at `maxDepth` levels
/// below each library folder and after `maxEntries` directory entries in all.
public struct LibraryScanner {
    public typealias Identify = (URL) -> Result<Identity, IdentifyError>

    public struct IdentifyError: Error, Equatable, Sendable {
        public let message: String
        public init(_ message: String) { self.message = message }
    }

    public var maxDepth = 3
    public var maxEntries = 20000
    /// Identifies a disc image (the toolkit's tools.prepare.identify); a folder is read here.
    public var identifyImage: Identify
    /// Also asks identify about folders (for the title profile's name); the XBE read here is the fallback.
    public var identifyFolders = false
    /// Identifies console games, a batch of sources at once (ConsoleCLI.identify, XBA-278). nil: the scan lists
    /// Xbox titles only (ROMs and console discs are not even looked at).
    public var identifyConsoles: (([URL]) -> [String: ConsoleIdentity])?
    public private(set) var truncated = false
    private var visited = 0

    public init(identifyImage: @escaping Identify) {
        self.identifyImage = identifyImage
    }

    /// The disc image extensions the pipeline reads (tools/xiso/source.py, EXTS).
    public static let imageExtensions: Set<String> = ["iso", "xiso", "cso", "ciso", "zso", "cci"]

    /// A name the pipeline takes for a disc image (tools/xiso/source.py, is_image_name): `Game.iso` with any of
    /// imageExtensions, or a split image's first part named `Game.iso.1` / `Game.iso.001` / `Game.iso.0` (prepare
    /// finds the other parts from it; later ones would only repeat the title). Parts named `Game.part2.iso` end in an
    /// image extension and are taken: the scan keeps one entry per title ID.
    public static func isImageName(_ name: String) -> Bool {
        let low = name.lowercased() as NSString
        let ext = low.pathExtension
        if imageExtensions.contains(ext) { return true }
        guard !ext.isEmpty, ext.count <= 4, ext.allSatisfy({ $0 >= "0" && $0 <= "9" }), let n = Int(ext), n <= 1 else {
            return false
        }
        return imageExtensions.contains((low.deletingPathExtension as NSString).pathExtension)
    }

    /// Every source under the folders (identified or not), in walk order; no dedupe.
    public mutating func findSources(in folders: [URL]) -> [(URL, LibraryEntry.Kind)] {
        truncated = false
        visited = 0
        var out: [(URL, LibraryEntry.Kind)] = []
        var seen = Set<String>()
        for f in folders { walk(f, depth: 0, &out, &seen) }
        return out
    }

    private mutating func walk(_ dir: URL, depth: Int, _ out: inout [(URL, LibraryEntry.Kind)], _ seen: inout Set<String>) {
        let fm = FileManager.default
        guard visited < maxEntries,
              let names = try? fm.contentsOfDirectory(atPath: dir.path) else { return }
        let real = dir.resolvingSymlinksInPath().path
        guard seen.insert(real).inserted else { return }
        if names.contains(where: { $0.lowercased() == "default.xbe" }) {
            out.append((dir, .folder))
            return
        }
        let consoles = identifyConsoles != nil
        let hasCue = consoles && names.contains { ($0.lowercased() as NSString).pathExtension == "cue" }
        for name in names.sorted(by: { $0.localizedStandardCompare($1) == .orderedAscending }) {
            visited += 1
            if visited > maxEntries { truncated = true; return }
            if name.hasPrefix(".") { continue }
            let u = dir.appendingPathComponent(name)
            var isDir: ObjCBool = false
            guard fm.fileExists(atPath: u.path, isDirectory: &isDir) else { continue }
            if isDir.boolValue {
                if name.hasSuffix(".app") || name.hasSuffix(".bundle") { continue }
                if depth < maxDepth - 1 || (depth < maxDepth && hasDefaultXbe(u)) {
                    walk(u, depth: depth + 1, &out, &seen)
                }
            } else if LibraryScanner.isImageName(name) {
                out.append((u, .image))
            } else if consoles {
                switch ConsoleSources.claim(name) {
                case .console: out.append((u, .console))
                case .track where !hasCue: out.append((u, .console))
                default: break
                }
            }
        }
    }

    private func hasDefaultXbe(_ dir: URL) -> Bool {
        ((try? FileManager.default.contentsOfDirectory(atPath: dir.path)) ?? [])
            .contains { $0.lowercased() == "default.xbe" }
    }

    /// The default.xbe of a game folder, in whatever case it has.
    public static func defaultXbe(in dir: URL) -> URL? {
        let names = (try? FileManager.default.contentsOfDirectory(atPath: dir.path)) ?? []
        return names.first { $0.lowercased() == "default.xbe" }.map { dir.appendingPathComponent($0) }
    }

    /// Size and modification time: the image's, or the folder's default.xbe's.
    static func stamp(_ url: URL, _ kind: LibraryEntry.Kind) -> (Int64, Double) {
        let target = kind == .folder ? (defaultXbe(in: url) ?? url) : url
        let a = (try? FileManager.default.attributesOfItem(atPath: target.path)) ?? [:]
        let size = (a[.size] as? NSNumber)?.int64Value ?? 0
        let mtime = (a[.modificationDate] as? Date)?.timeIntervalSince1970 ?? 0
        return (size, mtime)
    }

    /// Scans: sources found, identified (from the index when path, size and mtime match), then one entry per title
    /// ID (the first source found wins, the others are its duplicates). Sources that could not be identified are
    /// kept with their problem and never cached.
    public mutating func scan(_ folders: [URL], index: inout LibraryIndex) -> [LibraryEntry] {
        var entries: [LibraryEntry] = []
        var fresh: [Int] = []          // entries identified now (cached once every answer is in)
        var askConsole: [Int] = []     // entries the console identify answers
        for (url, kind) in findSources(in: folders) {
            let (size, mtime) = LibraryScanner.stamp(url, kind)
            if let cached = index.lookup(path: url.path, size: size, mtime: mtime) {
                entries.append(cached)
                continue
            }
            fresh.append(entries.count)
            var e = LibraryEntry(path: url.path, kind: kind, size: size, mtime: mtime)
            switch kind {
            case .folder:
                if identifyFolders, case .success(let id) = identifyImage(url) {
                    e.titleId = id.titleId
                    e.title = id.title
                    e.xbe = id.xbe ?? LibraryScanner.defaultXbe(in: url)?.path
                } else if let xbe = LibraryScanner.defaultXbe(in: url), let info = XbeInfo.read(xbe) {
                    e.titleId = info.titleId
                    e.title = info.title
                    e.xbe = xbe.path
                } else {
                    e.problem = "default.xbe could not be read"
                }
            case .image:
                switch identifyImage(url) {
                case .success(let id):
                    e.titleId = id.titleId
                    e.title = id.title
                    e.xbe = id.xbe
                case .failure(let err):
                    e.problem = err.message
                    // A GameCube or PlayStation .iso is no Xbox image: the console identify may know it.
                    if identifyConsoles != nil && ConsoleSources.claim(url.lastPathComponent) == .shared {
                        askConsole.append(entries.count)
                    }
                }
            case .console:
                e.problem = "Not identified"
                askConsole.append(entries.count)
            }
            entries.append(e)
        }
        if let identifyConsoles, !askConsole.isEmpty {
            let answers = identifyConsoles(askConsole.map { URL(fileURLWithPath: entries[$0].path) })
            for i in askConsole {
                let answer = answers[entries[i].path]
                if let a = answer, let c = a.consoleGame {
                    entries[i].kind = .console
                    entries[i].console = c.rawValue
                    entries[i].titleId = a.id
                    entries[i].title = a.title
                    entries[i].xbe = nil
                    entries[i].problem = nil
                } else if entries[i].kind == .console {
                    entries[i].problem = answer?.error ?? "Needs the Python environment (Setup)"
                }
            }
        }
        for i in fresh where entries[i].problem == nil { index.store(entries[i]) }
        index.prune(keeping: Set(entries.map(\.path)))
        return LibraryScanner.dedupe(entries)
    }

    public static func dedupe(_ entries: [LibraryEntry]) -> [LibraryEntry] {
        var out: [LibraryEntry] = []
        var byId: [String: Int] = [:]
        for var e in entries {
            e.duplicates = []
            if let tid = e.titleId, let i = byId[tid] {
                out[i].duplicates.append(e.path)
                continue
            }
            if let tid = e.titleId { byId[tid] = out.count }
            out.append(e)
        }
        return out
    }

    /// The real identify: `python -m tools.prepare.identify SOURCE --json --xbe-out <scanDir>/<hash>.xbe`, which
    /// extracts only default.xbe and writes nothing but that file.
    public static func pythonIdentify(python: String, toolkit: URL, scanDir: URL) -> Identify {
        return { url in
            try? FileManager.default.ensureDirectory(scanDir)
            let out = scanDir.appendingPathComponent(String(format: "%08X", stableHash(url.path)) + ".xbe")
            // A folder's default.xbe is read in place; only an image's is pulled out.
            let extract = FileManager.default.isDirectory(url) ? [] : ["--xbe-out", out.path]
            let r = Shell.run(python, ["-m", "tools.prepare.identify", url.path, "--json"] + extract,
                              cwd: toolkit, environment: ChildEnvironment.base(), timeout: 120)
            let text = r.output.trimmingCharacters(in: .whitespacesAndNewlines)
            let json = text.range(of: "{", options: [])
                .map { String(text[$0.lowerBound...]) } ?? text
            if r.ok, let d = json.data(using: .utf8), var id = try? JSONDecoder().decode(Identity.self, from: d) {
                if id.xbe == nil, FileManager.default.fileExists(atPath: out.path) { id.xbe = out.path }
                return .success(id)
            }
            return .failure(IdentifyError(identifyErrorMessage(text, status: r.status)))
        }
    }

    /// identify's failure line, {"error": {"code": "BAD_IMAGE", "msg": "..."}}, in words.
    public static func identifyErrorMessage(_ text: String, status: Int32) -> String {
        struct Wrap: Decodable { struct E: Decodable { let code: String; let msg: String? }; let error: E }
        let json = text.split(separator: "\n").last(where: { $0.hasPrefix("{") }).map(String.init) ?? ""
        if let w = try? JSONDecoder().decode(Wrap.self, from: Data(json.utf8)) {
            if w.error.code == "BAD_IMAGE" { return "Not readable yet (still being copied?)" }
            return "\(w.error.code): \(w.error.msg ?? "")"
        }
        let last = text.split(separator: "\n").last.map(String.init) ?? ""
        return last.isEmpty ? "identify failed (\(status))" : last
    }

    /// FNV-1a, for file names that stay the same across runs (String.hashValue does not).
    public static func stableHash(_ s: String) -> UInt32 {
        var h: UInt32 = 2_166_136_261
        for b in s.utf8 { h = (h ^ UInt32(b)) &* 16_777_619 }
        return h
    }
}
