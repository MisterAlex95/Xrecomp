import Foundation

/// settings.json: the launcher's global settings, shared with the `xrecomp` command line. Keys the app does not know
/// are kept on save; a missing or broken file reads as the defaults.
public struct Settings: Codable, Equatable, Sendable {
    /// The repository checkout the launcher drives (docs/macos/launcher.md « Toolkit »); nil = found by itself.
    public var toolkitPath: String?
    /// Folders scanned for games (3 levels deep). The first is « the » library folder.
    public var libraryFolders: [String]
    /// Where the work folders go; nil = `titles/<TID>/work` under Application Support.
    public var workRoot: String?
    /// A Python the player chose; nil = the toolkit's .venv, else the launcher's own venv.
    public var pythonPath: String?
    /// recomp.cfg lines every title starts from (KEY=VALUE); a title's own file wins.
    public var globalDefaults: [String: String]

    public static let defaultLibrary = "~/Documents/xbox"

    public init(toolkitPath: String? = nil, libraryFolders: [String] = [Settings.defaultLibrary],
                workRoot: String? = nil, pythonPath: String? = nil, globalDefaults: [String: String] = [:]) {
        self.toolkitPath = toolkitPath
        self.libraryFolders = libraryFolders
        self.workRoot = workRoot
        self.pythonPath = pythonPath
        self.globalDefaults = globalDefaults
    }

    enum CodingKeys: String, CodingKey, CaseIterable {
        case toolkitPath = "toolkit_path", libraryFolders = "library_folders", workRoot = "work_root"
        case pythonPath = "python_path", globalDefaults = "global_defaults"
    }

    public init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        toolkitPath = try c.decodeIfPresent(String.self, forKey: .toolkitPath)
        libraryFolders = try c.decodeIfPresent([String].self, forKey: .libraryFolders) ?? [Settings.defaultLibrary]
        workRoot = try c.decodeIfPresent(String.self, forKey: .workRoot)
        pythonPath = try c.decodeIfPresent(String.self, forKey: .pythonPath)
        globalDefaults = try c.decodeIfPresent([String: String].self, forKey: .globalDefaults) ?? [:]
    }

    public static func load(from url: URL) -> Settings {
        guard let data = try? Data(contentsOf: url),
              let s = try? JSONDecoder().decode(Settings.self, from: data) else { return Settings() }
        return s
    }

    public func save(to url: URL) throws {
        let known = Set(CodingKeys.allCases.map(\.rawValue))
        try writeAtomically(try keepingUnknownKeys(try JSONEncoder.pretty.encode(self), of: url, known: known), to: url)
    }

    /// The library folders as absolute URLs (`~` expanded), duplicates removed, order kept.
    public var libraryURLs: [URL] {
        var seen = Set<String>()
        return libraryFolders.compactMap { f in
            let p = ((f as NSString).expandingTildeInPath as NSString).standardizingPath
            guard !p.isEmpty, seen.insert(p).inserted else { return nil }
            return URL(fileURLWithPath: p, isDirectory: true)
        }
    }

    /// AppPaths with this work root applied.
    public func paths(base: AppPaths) -> AppPaths {
        var p = base
        if let w = workRoot, !w.isEmpty {
            p.workRoot = URL(fileURLWithPath: (w as NSString).expandingTildeInPath, isDirectory: true)
        }
        return p
    }
}
