import Foundation

/// Where the launcher keeps its files (docs/macos/launcher.md « Storage »):
///
///     ~/Library/Application Support/xboxrecomp/
///       settings.json, library.json, venv/
///       titles/<TID>/ work/ recomp.cfg save/ art.png theme.json logs/ state.json
///
/// `XRECOMP_HOME` moves the whole tree (the checks and the smoke run use a temporary one). The work folders can
/// live elsewhere (Settings.workRoot): an ISO source is extracted into `<work>/game`, several GB per title.
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

    public func title(_ tid: String) -> TitlePaths {
        let dir = titles.appendingPathComponent(tid.uppercased(), isDirectory: true)
        let work = workRoot.map { $0.appendingPathComponent(tid.uppercased(), isDirectory: true) }
            ?? dir.appendingPathComponent("work", isDirectory: true)
        return TitlePaths(titleId: tid.uppercased(), dir: dir, work: work)
    }
}

/// One title's files.
public struct TitlePaths: Sendable {
    public let titleId: String
    public let dir: URL
    /// `prepare --work`: analysis, project, build, stamp.json, discovered.jsonl, prepare.log.
    public let work: URL

    public var recompCfgDir: URL { dir }
    public var recompCfg: URL { dir.appendingPathComponent(RecompConfig.fileName) }
    /// RECOMP_SAVE_DIR: UDATA/<TID>, TDATA/<TID>, Cache/<TID> (Z:).
    public var save: URL { dir.appendingPathComponent("save", isDirectory: true) }
    public var art: URL { dir.appendingPathComponent("art.png") }
    public var theme: URL { dir.appendingPathComponent("theme.json") }
    public var logs: URL { dir.appendingPathComponent("logs", isDirectory: true) }
    public var state: URL { dir.appendingPathComponent("state.json") }
    public var prepareLog: URL { work.appendingPathComponent("prepare.log") }
    public var project: URL { work.appendingPathComponent("project", isDirectory: true) }
    public var build: URL { project.appendingPathComponent("build", isDirectory: true) }
    /// The disc extracted by prepare (ISO sources only).
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

/// Writes data through `<file>.tmp` and a rename, so a reader sees the old or the new file, never half of one.
public func writeAtomically(_ data: Data, to url: URL) throws {
    let fm = FileManager.default
    try fm.ensureDirectory(url.deletingLastPathComponent())
    let tmp = url.deletingLastPathComponent().appendingPathComponent(url.lastPathComponent + ".tmp")
    try data.write(to: tmp)
    if rename(tmp.path, url.path) != 0 {
        try? fm.removeItem(at: tmp)
        throw CocoaError(.fileWriteUnknown, userInfo: [NSFilePathErrorKey: url.path])
    }
}

public extension JSONEncoder {
    static var pretty: JSONEncoder {
        let e = JSONEncoder()
        e.outputFormatting = [.prettyPrinted, .sortedKeys]
        return e
    }
}
