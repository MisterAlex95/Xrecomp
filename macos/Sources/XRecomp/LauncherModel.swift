import AppKit
import SwiftUI
import XRecompCore

/// Where a title stands, for its card and its Home status line.
enum TitleStatus: Equatable {
    case checking
    case absent
    case stale(reasons: [String])
    case ready
    case error(code: String, message: String)
    /// The source could not be identified (an ISO still being copied, a broken folder).
    case unreadable(String)
}

/// A prepare in the queue, running, or ended badly.
enum PrepareState: Equatable {
    case queued(position: Int)
    case running(PrepareProtocol.Progress, started: Date)
    case failed(code: String, message: String, logTail: [String], log: URL?)
}

/// One game of the library, as the views see it.
struct GameItem: Identifiable, Equatable {
    var id: String
    var entry: LibraryEntry
    var name: String
    var titleId: String?
    var art: NSImage?
    var accent: UInt32
    var status: TitleStatus = .checking
    var exe: String?
    var envForce: [String: String] = [:]
    /// prepare --check's "title_layer": "on" / "off" when the toolkit has optional enhancements for this title.
    var titleLayer: String?
    var prepare: PrepareState?
    /// Since when the game runs (nil: not running).
    var runningSince: Date?
    /// The last run's problem, « Stopped unexpectedly (signal 11) », with the log's tail.
    var lastExit: String?
    var lastExitTail: [String] = []
    var record = TitleRecord()

    var accentColor: Color { Color(argb: accent) }
    var playable: Bool {
        guard exe != nil else { return false }
        switch status {
        case .ready, .stale: return true
        default: return false
        }
    }

    static func == (a: GameItem, b: GameItem) -> Bool {
        a.id == b.id && a.entry == b.entry && a.name == b.name && a.status == b.status && a.exe == b.exe
            && a.prepare == b.prepare && a.runningSince == b.runningSince && a.lastExit == b.lastExit
            && a.record == b.record && a.accent == b.accent && (a.art === b.art) && a.titleLayer == b.titleLayer
    }

    /// The one status line of Home and the card.
    var statusLine: (String, Color) {
        if let since = runningSince {
            return ("Running · " + PrepareProtocol.formatClock(Int(Date().timeIntervalSince(since))), Theme.accent)
        }
        switch prepare {
        case .queued(let p): return (p == 1 ? "Queued: next" : "Queued (\(p))", Theme.warn)
        case .running(let p, _):
            return ("Preparing · step \(p.step)/\(p.n) · \(Int(p.overall * 100)) %", Theme.accent)
        case .failed(let code, _, _, _): return ("Prepare failed (\(code))", Theme.error)
        case nil: break
        }
        if let e = lastExit { return (e, Theme.error) }
        switch status {
        case .checking: return ("Checking…", Theme.textFaint)
        case .absent: return ("Needs preparing", Theme.warn)
        case .stale(let r): return (TitleState.describe(r), Theme.warn)
        case .ready: return ("Ready · " + record.playTimeText, Theme.ok)
        case .error(let code, _): return ("Cannot prepare (\(code))", Theme.error)
        case .unreadable(let why): return (why, Theme.textFaint)
        }
    }
}

/// The detail pane's screens.
enum Route: Hashable {
    case library
    case home(String)
    case settings(String)
    case controls(String)
    case manage(String)
    case log(String)
    case setup
    case defaults
    case about

    var gameId: String? {
        switch self {
        case .home(let id), .settings(let id), .controls(let id), .manage(let id), .log(let id): return id
        default: return nil
        }
    }
}

/// The Setup screen's facts.
struct SetupInfo: Equatable {
    var toolkit: String?
    var toolkitSource: String?
    var toolkitCommit: String?
    var python: String?
    var dependencies: [Dependency] = []
    var checking = false
    var creatingVenv = false
    var venvLog: [String] = []
    var libraryFolders: [String] = []
    var workRoot: String?
    var appHome: String = ""

    var ready: Bool { toolkit != nil && python != nil && DependencyCheck.allRequiredOK(dependencies) }
}

/// What the views ask for. AppStore does it for real; PreviewStore (--demo) pretends.
@MainActor
protocol LauncherActions: AnyObject {
    func rescan()
    func addGame(_ url: URL)
    func prepare(_ id: String, force: Bool)
    func cancelPrepare(_ id: String)
    func play(_ id: String)
    func stop(_ id: String)
    func loadConfig(_ id: String?) -> RecompConfig
    func saveConfig(_ config: RecompConfig, for id: String?)
    func storage(_ id: String) -> [(Storage.Item, Int64)]
    func delete(_ item: Storage.Item, of id: String)
    func importSaves(_ id: String) -> Int
    /// The title's optional enhancements on or off for its next prepare (profile-free WP6).
    func setEnhancements(_ id: String, on: Bool)
    /// « Create an app for this game »: `<Game title>.app` at url (docs/macos/launcher.md « Game apps »).
    func createTitleApp(_ id: String, at url: URL)
    /// A game app's « Open XRecomp »: the main app it was made from.
    func openMainApp()
    func reveal(_ url: URL)
    func titlePaths(_ id: String) -> TitlePaths?
    func setToolkit(_ path: String)
    func addLibraryFolder(_ path: String)
    func removeLibraryFolder(_ path: String)
    func setWorkRoot(_ path: String?)
    func recheckSetup()
    func createVenv()
    func licences() -> [(String, String)]
}

/// The launcher's state, observed by every view.
@MainActor
final class LauncherModel: ObservableObject {
    @Published var games: [GameItem] = []
    @Published var route: Route = .library
    @Published var setup = SetupInfo()
    @Published var scanning = false
    /// The live log (prepare or game output), capped.
    @Published var liveLog: [String] = []
    @Published var liveLogTitle = ""
    /// A one-shot message (errors of actions).
    @Published var notice: String?
    /// Ticks once a second while something runs, so elapsed times refresh.
    @Published var tick = 0
    /// The game app being made (Manage shows a spinner).
    @Published var creatingApp: String?
    /// The library was scanned at least once (a game app says « not in the library » only after that).
    @Published var scannedOnce = false
    weak var actions: LauncherActions?
    let demo: Bool
    /// Single-game mode (a game app, Info.plist `XBASingleTitle`): the title ID, upper case. The window then shows
    /// that game's screens only: no sidebar, no Library, no « Create an app ».
    let singleTitle: String?
    static let logCap = 5000

    init(demo: Bool = false, singleTitle: String? = nil) {
        self.demo = demo
        self.singleTitle = singleTitle
        if let singleTitle { route = .home(singleTitle) }
    }

    func game(_ id: String) -> GameItem? { games.first { $0.id == id } }

    /// A game app's game, when the library has it.
    var singleGame: GameItem? {
        guard let s = singleTitle else { return nil }
        return games.first { $0.titleId?.uppercased() == s }
    }

    /// Where « back » ends: the Library, or a game app's one Home.
    var homeRoute: Route {
        guard let s = singleTitle else { return .library }
        return .home(singleGame?.id ?? s)
    }

    /// In a game app, only its own title is listed.
    func accepts(_ g: GameItem) -> Bool {
        guard let s = singleTitle else { return true }
        return g.titleId?.uppercased() == s
    }

    func update(_ id: String, _ change: (inout GameItem) -> Void) {
        guard let i = games.firstIndex(where: { $0.id == id }) else { return }
        change(&games[i])
    }

    func appendLog(_ line: String) {
        liveLog.append(line)
        if liveLog.count > LauncherModel.logCap { liveLog.removeFirst(liveLog.count - LauncherModel.logCap) }
    }

    var runningGame: GameItem? { games.first { $0.runningSince != nil } }
    var preparingGame: GameItem? {
        games.first { if case .running = $0.prepare { return true } else { return false } }
    }
}
