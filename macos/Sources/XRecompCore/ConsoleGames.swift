import Foundation

/// The other consoles in the Mac launcher (XBA-278): Game Boy Advance, PlayStation, Nintendo 64 and GameCube games,
/// listed, prepared and played next to the Xbox titles. The launcher does not read ROM headers, disc formats or
/// pack.json itself: it asks the toolkit's desktop command line (`python -m tools.xrecomp`, docs/desktop/cli.md),
/// which tells consoles apart the way `tools.prepare` does and shares this data folder on a Mac, so the two never
/// disagree:
///
///     xrecomp --home ROOT identify SOURCE... --json      console, game ID, title, how prepared, the player
///     xrecomp --home ROOT play ID --print --json         the player's argv, cwd and environment
///
/// The launcher then starts the player itself (GameRunner: its run log, stop by PID) and prepares with
/// `tools.prepare SOURCE --work DIR --pack-dir ROOT/packs --progress` (PrepareRunner), as `xrecomp prepare` does.
/// A console game plays before it is prepared (the runtime interprets it); a prepare makes it run natively.
public enum Console: String, Codable, CaseIterable, Sendable {
    case xbox, gba, ps1, n64, gc

    /// The console of a game ID: « gba-AXVE-1A2B3C4D » -> .gba, an Xbox title ID -> .xbox (tools/xrecomp/home.py
    /// console_of).
    public static func of(gameId: String?) -> Console {
        guard let id = gameId, let dash = id.firstIndex(of: "-") else { return .xbox }
        let head = id[..<dash].lowercased()
        return Console(rawValue: head).flatMap { $0 == .xbox ? nil : $0 } ?? .xbox
    }

    /// A console game's ID as a game app records it, upper case (LauncherModel.singleTitle compares upper case); nil
    /// for anything else (an Xbox title ID is TitleApp.normalizedTitleId's).
    public static func normalizedGameId(_ s: String?) -> String? {
        guard let t = s?.trimmingCharacters(in: .whitespacesAndNewlines).uppercased(), !t.isEmpty,
              of(gameId: t) != .xbox else { return nil }
        return t
    }

    public var name: String {
        switch self {
        case .xbox: return "Xbox"
        case .gba: return "Game Boy Advance"
        case .ps1: return "PlayStation"
        case .n64: return "Nintendo 64"
        case .gc: return "GameCube"
        }
    }

    /// The card's badge.
    public var short: String {
        switch self {
        case .xbox: return "XBOX"
        case .gba: return "GBA"
        case .ps1: return "PS1"
        case .n64: return "N64"
        case .gc: return "GC"
        }
    }

    /// The player that runs its games (tools/xrecomp PLAYERS).
    public var player: String? { self == .xbox ? nil : "\(rawValue)_play" }

    /// The libretro-thumbnails repository of its box art (the Android launcher's CoverArt.system).
    public var thumbnailSystem: String? {
        switch self {
        case .xbox: return nil
        case .gba: return "Nintendo_-_Game_Boy_Advance"
        case .ps1: return "Sony_-_PlayStation"
        case .n64: return "Nintendo_-_Nintendo_64"
        case .gc: return "Nintendo_-_GameCube"
        }
    }
}

/// Which files the scan hands to the console identify (tools/xrecomp/library.py candidates: ROM_EXT, PS1_EXT,
/// GC_EXT). A .iso or .ciso is an Xbox image first (the Xbox identify reads it) and a console disc when that fails
/// (a GameCube or PlayStation .iso); a .bin / .img is a PlayStation track only when no .cue sits next to it (the
/// cue names its tracks); a bare .exe / .dol is taken only when it is .dol (a PS-X EXE is given by hand).
public enum ConsoleSources {
    public static let consoleExtensions: Set<String> = ["gba", "z64", "v64", "n64", "cue", "chd", "gcm", "gcz",
                                                        "rvz", "wia", "dol"]
    /// Xbox first, a console disc when the Xbox identify fails.
    public static let sharedExtensions: Set<String> = ["iso", "ciso"]
    /// PlayStation tracks, unless a .cue in the same folder names them.
    public static let trackExtensions: Set<String> = ["bin", "img"]

    public enum Claim: Equatable, Sendable {
        case none, console, shared, track
    }

    public static func claim(_ name: String) -> Claim {
        let ext = (name.lowercased() as NSString).pathExtension
        if consoleExtensions.contains(ext) { return .console }
        if sharedExtensions.contains(ext) { return .shared }
        if trackExtensions.contains(ext) { return .track }
        return .none
    }

    /// What the file picker and a drop take besides the Xbox's images and default.xbe.
    public static var pickerExtensions: [String] {
        (consoleExtensions.union(trackExtensions)).sorted()
    }
}

/// One answer of `xrecomp identify --json` (tools/xrecomp/__main__.py identify_dict).
public struct ConsoleIdentity: Codable, Equatable, Sendable {
    public var id: String?
    public var console: String?
    public var title: String?
    public var source: String
    /// prepared | stale | absent | missing | unreadable (tools/xrecomp/library.py).
    public var state: String?
    public var label: String?
    public var reasons: [String]
    public var note: String?
    public var pack: String?
    public var error: String?
    public var key: String?
    /// The player found (gba_play...), nil when it is not built.
    public var player: String?
    public var saveDir: String?
    public var packDir: String?

    enum CodingKeys: String, CodingKey {
        case id, console, title, source, state, label, reasons, note, pack, error, key, player
        case saveDir = "save_dir", packDir = "pack_dir"
    }

    public init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        id = try c.decodeIfPresent(String.self, forKey: .id)
        console = try c.decodeIfPresent(String.self, forKey: .console)
        title = try c.decodeIfPresent(String.self, forKey: .title)
        source = try c.decode(String.self, forKey: .source)
        state = try c.decodeIfPresent(String.self, forKey: .state)
        label = try c.decodeIfPresent(String.self, forKey: .label)
        reasons = try c.decodeIfPresent([String].self, forKey: .reasons) ?? []
        note = try c.decodeIfPresent(String.self, forKey: .note)
        pack = try c.decodeIfPresent(String.self, forKey: .pack)
        error = try c.decodeIfPresent(String.self, forKey: .error)
        key = try c.decodeIfPresent(String.self, forKey: .key)
        player = try c.decodeIfPresent(String.self, forKey: .player)
        saveDir = try c.decodeIfPresent(String.self, forKey: .saveDir)
        packDir = try c.decodeIfPresent(String.self, forKey: .packDir)
    }

    /// A console game (not an Xbox one, not unreadable).
    public var consoleGame: Console? {
        guard error == nil, let c = console.flatMap(Console.init(rawValue:)), c != .xbox, id != nil else { return nil }
        return c
    }

    /// Prepared and up to date / prepared by an older toolkit (still plays natively) / not prepared (interpreted).
    public enum Prepared: Equatable, Sendable {
        case native, stale([String]), interpreted, problem(String)
    }

    public var prepared: Prepared {
        if let error { return .problem(error) }
        switch state {
        case "prepared": return .native
        case "stale": return .stale(reasons)
        case "absent": return .interpreted
        default: return .problem(label ?? state ?? "unknown state")
        }
    }

    /// Every answer of one `identify --json` run; [] when the output is not that.
    public static func parse(_ output: String) -> [ConsoleIdentity] {
        guard let start = output.firstIndex(of: "[") else { return [] }
        let json = String(output[start...])
        return (try? JSONDecoder().decode([ConsoleIdentity].self, from: Data(json.utf8))) ?? []
    }
}

/// What `xrecomp play ID --print --json` answers: the player's command line.
public struct ConsolePlay: Codable, Equatable, Sendable {
    public var id: String?
    public var console: String?
    public var argv: [String]
    public var cwd: String
    public var env: [String: String]

    enum CodingKeys: String, CodingKey { case id, console, argv, cwd, env }

    public init(argv: [String], cwd: String, env: [String: String], id: String? = nil, console: String? = nil) {
        self.argv = argv
        self.cwd = cwd
        self.env = env
        self.id = id
        self.console = console
    }

    public static func parse(_ output: String) -> ConsolePlay? {
        guard let start = output.firstIndex(of: "{") else { return nil }
        guard let p = try? JSONDecoder().decode(ConsolePlay.self, from: Data(String(output[start...]).utf8)),
              !p.argv.isEmpty else { return nil }
        return p
    }

    /// The environment the launcher starts the player with: the command line's, plus what ChildEnvironment.game
    /// adds for an Xbox title on a Mac (the fixed PATH, Homebrew's lib folders for frame generation's Vulkan).
    public var macEnvironment: [String: String] {
        var e = ChildEnvironment.base(env)
        if e["RECOMP_LSFG"] == "1" && (e["DYLD_FALLBACK_LIBRARY_PATH"] ?? "").isEmpty {
            e["DYLD_FALLBACK_LIBRARY_PATH"] = VulkanLoader.fallbackPath(home: e["HOME"])
        }
        return e
    }

    /// GameRunner's request: argv[0] with the rest, in cwd.
    public func request(log: URL?) -> GameRunner.Request {
        GameRunner.Request(exe: URL(fileURLWithPath: argv[0]), arguments: Array(argv.dropFirst()),
                           cwd: URL(fileURLWithPath: cwd, isDirectory: true), environment: macEnvironment, log: log)
    }
}

/// The toolkit's command line, run by the launcher (off the main thread: each call starts a Python).
public struct ConsoleCLI: Sendable {
    public var python: String
    public var toolkit: URL
    /// The data folder (AppPaths.root): the command line's --home, so both read one tree even with XRECOMP_HOME.
    public var home: URL
    public var timeout: TimeInterval = 300

    public init(python: String, toolkit: URL, home: URL) {
        self.python = python
        self.toolkit = toolkit
        self.home = home
    }

    public func identifyArguments(_ sources: [URL]) -> [String] {
        ["-m", "tools.xrecomp", "--home", home.path, "identify"] + sources.map(\.path) + ["--json"]
    }

    public func playArguments(_ gameId: String) -> [String] {
        ["-m", "tools.xrecomp", "--home", home.path, "play", gameId, "--print", "--json"]
    }

    /// The answers by source path; the sources it could not run for are missing (a Python that fails to start).
    public func identify(_ sources: [URL]) -> [String: ConsoleIdentity] {
        guard !sources.isEmpty else { return [:] }
        var out: [String: ConsoleIdentity] = [:]
        // A long library goes in batches: one Python each, and no command line over ARG_MAX.
        var i = 0
        while i < sources.count {
            let batch = Array(sources[i..<min(i + 64, sources.count)])
            i += batch.count
            let r = Shell.run(python, identifyArguments(batch), cwd: toolkit, environment: ChildEnvironment.base(),
                              timeout: timeout)
            for id in ConsoleIdentity.parse(r.output) { out[id.source] = id }
        }
        return out
    }

    public enum PlayError: Error, Equatable, CustomStringConvertible {
        case failed(String)
        public var description: String {
            switch self { case .failed(let s): return s }
        }
    }

    /// The player's command line for a game, or why not (« n64_play is not built: ... »). game: its source path (only
    /// that file is read) or its ID (the command line then walks the whole library to find it).
    public func play(_ game: String) -> Result<ConsolePlay, PlayError> {
        let r = Shell.run(python, playArguments(game), cwd: toolkit, environment: ChildEnvironment.base(),
                          timeout: 120)
        if r.ok, let p = ConsolePlay.parse(r.output) { return .success(p) }
        return .failure(.failed(ConsoleCLI.errorLine(r.output, status: r.status)))
    }

    /// The command line's own words (« xrecomp: ... »), else its last line.
    public static func errorLine(_ output: String, status: Int32) -> String {
        let lines = output.split(separator: "\n").map(String.init)
        if let l = lines.last(where: { $0.hasPrefix("xrecomp: ") }) { return String(l.dropFirst("xrecomp: ".count)) }
        return lines.last ?? "xrecomp failed (exit \(status))"
    }
}

/// The prepare of a console game: the pack goes under ROOT/packs/<ID>/ (tools/xrecomp prepare_command).
public extension PrepareRunner.Request {
    static func console(python: String, toolkit: URL, source: URL, work: URL, packs: URL) -> PrepareRunner.Request {
        var r = PrepareRunner.Request(python: python, toolkit: toolkit, source: source, work: work)
        r.packDir = packs
        return r
    }
}
