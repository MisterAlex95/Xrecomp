import Foundation

/// Where the launcher keeps its files (docs/macos/launcher.md « Storage »):
///
///     ~/Library/Application Support/xboxrecomp/
///       settings.json, library.json, venv/
///       titles/<TID>/ work/ recomp.cfg save/ art.png theme.json logs/ state.json
///
/// `XRECOMP_HOME` moves the whole tree (the checks and the smoke run use a temporary one). The work folders can
/// live elsewhere (Settings.workRoot): about 2 GB per title of analysis, generated C and build. A disc image is read
/// in place (XBA-126); only work folders made before that hold an extracted disc in `<work>/game`.
public struct AppPaths: Sendable {
    public let root: URL
    /// The folder that holds `<TID>/` work folders when the player moved them; nil = under `titles/<TID>/work`.
    public var workRoot: URL?

    public init(root: URL, workRoot: URL? = nil) {
        self.root = root
        self.workRoot = workRoot
    }

    /// `$XRECOMP_HOME`, else `~/Library/Application Support/xboxrecomp`.
    public static func standard(environment: [String: String] = ProcessInfo.processInfo.environment) -> AppPaths {
        if let home = environment["XRECOMP_HOME"], !home.isEmpty {
            return AppPaths(root: URL(fileURLWithPath: (home as NSString).expandingTildeInPath, isDirectory: true))
        }
        let base = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask).first
            ?? URL(fileURLWithPath: NSHomeDirectory()).appendingPathComponent("Library/Application Support")
        return AppPaths(root: base.appendingPathComponent("xboxrecomp", isDirectory: true))
    }

    public var settingsFile: URL { root.appendingPathComponent("settings.json") }
    public var libraryFile: URL { root.appendingPathComponent("library.json") }
    public var venv: URL { root.appendingPathComponent("venv", isDirectory: true) }
    public var titles: URL { root.appendingPathComponent("titles", isDirectory: true) }
    /// Scratch for the library scan (the default.xbe identify pulls out of an ISO, for its art).
    public var scanDir: URL { root.appendingPathComponent("scan", isDirectory: true) }

    /// GBA / PS1 / N64 / GameCube packs (`prepare --pack-dir`): `packs/<ID>/` (XBA-278, the xrecomp CLI's tree).
    public var packs: URL { root.appendingPathComponent("packs", isDirectory: true) }
    /// The console runtimes' RECOMP_SAVE_DIR: they add `<console>-<KEY>/` (= the game ID) themselves.
    public var consoleSaves: URL { root.appendingPathComponent("save", isDirectory: true) }

    /// A title's folders. An Xbox title ID is upper case (`4D530004`); a console game ID is kept as it is
    /// (`gba-AXVE-1A2B3C4D`), as the xrecomp command line names it (tools/xrecomp/home.py TitlePaths).
    public func title(_ tid: String) -> TitlePaths {
        let id = Console.of(gameId: tid) == .xbox ? tid.uppercased() : tid
        let dir = titles.appendingPathComponent(id, isDirectory: true)
        let work = workRoot.map { $0.appendingPathComponent(id, isDirectory: true) }
            ?? dir.appendingPathComponent("work", isDirectory: true)
        return TitlePaths(titleId: id, dir: dir, work: work, root: root)
    }
}

/// One title's files.
public struct TitlePaths: Sendable {
    public let titleId: String
    public let dir: URL
    /// `prepare --work`: analysis, project, build, stamp.json, discovered.jsonl, prepare.log.
    public let work: URL
    /// The data folder (AppPaths.root).
    public let root: URL

    public init(titleId: String, dir: URL, work: URL, root: URL? = nil) {
        self.titleId = titleId
        self.dir = dir
        self.work = work
        self.root = root ?? dir.deletingLastPathComponent().deletingLastPathComponent()
    }

    public var console: Console { Console.of(gameId: titleId) }
    public var recompCfgDir: URL { dir }
    public var recompCfg: URL { dir.appendingPathComponent(RecompConfig.fileName) }
    /// RECOMP_SAVE_DIR: UDATA/<tid>, TDATA/<tid>, Cache/<tid> (Z:), the title ID in lower case as the runtime names
    /// them (docs/technical/saves.md). A console game's: the shared `save/` (its runtime adds `<ID>/`).
    public var save: URL {
        console == .xbox ? dir.appendingPathComponent("save", isDirectory: true)
            : root.appendingPathComponent("save", isDirectory: true)
    }
    /// A console game's own saves, memory cards and states: `save/<ID>/`; nil for an Xbox title.
    public var consoleSave: URL? { console == .xbox ? nil : save.appendingPathComponent(titleId, isDirectory: true) }
    /// A console game's pack (`packs/<ID>/`); nil for an Xbox title.
    public var pack: URL? {
        console == .xbox ? nil : root.appendingPathComponent("packs", isDirectory: true)
            .appendingPathComponent(titleId, isDirectory: true)
    }
    public var art: URL { dir.appendingPathComponent("art.png") }
    public var theme: URL { dir.appendingPathComponent("theme.json") }
    public var logs: URL { dir.appendingPathComponent("logs", isDirectory: true) }
    public var state: URL { dir.appendingPathComponent("state.json") }
    public var prepareLog: URL { work.appendingPathComponent("prepare.log") }
    public var project: URL { work.appendingPathComponent("project", isDirectory: true) }
    public var build: URL { project.appendingPathComponent("build", isDirectory: true) }
    /// The disc a prepare extracted from an image before XBA-126 (or with `prepare --extract`); absent otherwise.
    public var extractedGame: URL { work.appendingPathComponent("game", isDirectory: true) }
    public var discovered: URL { work.appendingPathComponent("discovered.jsonl") }
}

extension FileManager {
    /// Creates the directory and its parents; no error when it exists.
    public func ensureDirectory(_ url: URL) throws {
        try createDirectory(at: url, withIntermediateDirectories: true)
    }

    public func isDirectory(_ url: URL) -> Bool {
        var d: ObjCBool = false
        return fileExists(atPath: url.path, isDirectory: &d) && d.boolValue
    }
}

/// Writes data through a temporary file and a rename, so a reader sees the old or the new file, never half of one.
/// The temporary name is the writer's own (`<file>.<pid>.<random>.tmp`): a game app and XRecomp.app share the data
/// tree, and two writers on one `<file>.tmp` could publish each other's half-written bytes.
public func writeAtomically(_ data: Data, to url: URL) throws {
    let fm = FileManager.default
    try fm.ensureDirectory(url.deletingLastPathComponent())
    let tmp = url.deletingLastPathComponent()
        .appendingPathComponent("\(url.lastPathComponent).\(getpid()).\(UUID().uuidString.prefix(8)).tmp")
    do {
        try data.write(to: tmp)
    } catch {
        try? fm.removeItem(at: tmp)
        throw error
    }
    if rename(tmp.path, url.path) != 0 {
        try? fm.removeItem(at: tmp)
        throw CocoaError(.fileWriteUnknown, userInfo: [NSFilePathErrorKey: url.path])
    }
}

/// A file that exists but does not read as what it should be is moved to `<file>.bad` (replacing an older one), so the
/// next save of the defaults does not overwrite what the player had (a hand edit with a syntax error). The `.bad`
/// path, or nil when nothing was moved (an empty file holds nothing to keep).
@discardableResult
public func setAsideBroken(_ url: URL) -> URL? {
    guard let size = (try? FileManager.default.attributesOfItem(atPath: url.path))?[.size] as? NSNumber,
          size.intValue > 0 else { return nil }
    let bad = url.deletingLastPathComponent().appendingPathComponent(url.lastPathComponent + ".bad")
    return rename(url.path, bad.path) == 0 ? bad : nil
}

/// `encoded` (a JSON object) with the keys of the file now at url that `known` does not list put back. The `xrecomp`
/// command line shares these files on a Mac (docs/desktop/cli.md « Storage ») and writes keys the app does not know
/// (state.json's `title` and `console`): rewriting the file must not drop them. The file is read at save time, so a
/// key written since the app loaded it survives too. `encoded` as it is when nothing is to be kept.
public func keepingUnknownKeys(_ encoded: Data, of url: URL, known: Set<String>) throws -> Data {
    guard let old = try? Data(contentsOf: url),
          let oldObject = (try? JSONSerialization.jsonObject(with: old)) as? [String: Any] else { return encoded }
    let extra = oldObject.filter { !known.contains($0.key) }
    guard !extra.isEmpty, var object = (try JSONSerialization.jsonObject(with: encoded)) as? [String: Any] else {
        return encoded
    }
    for (k, v) in extra where object[k] == nil { object[k] = v }
    var data = try JSONSerialization.data(withJSONObject: object,
                                          options: [.prettyPrinted, .sortedKeys, .withoutEscapingSlashes])
    data.append(0x0A)
    return data
}

public extension JSONEncoder {
    static var pretty: JSONEncoder {
        let e = JSONEncoder()
        e.outputFormatting = [.prettyPrinted, .sortedKeys, .withoutEscapingSlashes]
        return e
    }
}
