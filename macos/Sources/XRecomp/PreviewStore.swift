import AppKit
import SwiftUI
import XRecompCore

/// `XRecomp --demo [screen]`: every screen from mock data, with no toolkit, no Python and no game. The titles are
/// invented and their art is drawn here, so screenshots carry nothing of a real game. A prepare is simulated.
@MainActor
final class PreviewStore: LauncherActions {
    let model: LauncherModel
    private var configs: [String: RecompConfig] = [:]
    private var timer: Timer?

    static let screens = ["library", "home", "preparing", "failed", "running", "settings", "controls", "manage", "log",
                          "setup", "about", "single", "single-missing"]

    init(screen: String) {
        // « single »: a game app (single-game mode) on Sample Racer; « single-missing »: one whose title is gone.
        let single: String? = screen == "single" ? "58410002" : screen == "single-missing" ? "584100FF" : nil
        model = LauncherModel(demo: true, singleTitle: single)
        model.actions = self
        model.games = PreviewStore.mockGames()
        model.scannedOnce = true
        model.setup = SetupInfo(
            toolkit: "/Users/you/xboxrecomp-android", toolkitSource: "infoPlist",
            toolkitCommit: "504a47a1c2d3e4f5a6b7c8d9e0f1a2b3c4d5e6f7",
            python: "/Users/you/xboxrecomp-android/.venv/bin/python3",
            dependencies: [
                Dependency(id: "cmake", name: "CMake", ok: true, detail: "cmake version 3.31.6", fix: "brew install cmake"),
                Dependency(id: "ninja", name: "Ninja (faster builds)", ok: true, optional: true, detail: "1.12.1",
                           fix: "brew install ninja"),
                Dependency(id: "clang", name: "Clang (Command Line Tools)", ok: true,
                           detail: "Apple clang version 21.0.0", fix: "xcode-select --install"),
                Dependency(id: "git", name: "Git", ok: true, detail: "git version 2.50.1", fix: "xcode-select --install"),
                Dependency(id: "sdl2", name: "SDL2 (sdl2-compat is fine)", ok: true, detail: "2.32.56",
                           fix: "brew install sdl2-compat"),
                Dependency(id: "epoxy", name: "libepoxy", ok: false, detail: "pkg-config cannot find epoxy",
                           fix: "brew install libepoxy"),
                Dependency(id: "python", name: "Python with capstone, pefile, numpy", ok: true,
                           detail: "3.12.11 · /Users/you/xboxrecomp-android/.venv/bin/python3",
                           fix: ""),
            ],
            libraryFolders: ["~/Documents/xbox", "~/Downloads/xboxrecomp/titles"],
            appHome: "~/Library/Application Support/xboxrecomp")
        let first = model.games[0].id
        refreshLsfg()
        model.liveLogTitle = "Night Ops · run started 21:04"
        model.liveLog = PreviewStore.mockLog
        switch screen {
        case "home":
            model.games[0].runningSince = nil
            model.route = .home(model.games[1].id)
        case "preparing": model.route = .home(model.games[2].id)
        case "failed": model.route = .home(model.games[3].id)
        case "running": model.route = .home(first)
        case "settings":
            // Default settings: the GPU renderer and the CRT shader; the game: its own resolution and aspect, frame
            // generation 2x, and no shader (« KEY= »: the runtime default over Default settings').
            var d = RecompConfig()
            d.set("RECOMP_GPU", "1")
            d.set("RECOMP_SHADER", "crt")
            configs["_"] = d
            var c = RecompConfig()
            c.set("RECOMP_GPU_SCALE", "2")
            c.set("RECOMP_ASPECT", "16:9")
            c.set("RECOMP_SHADER", "")
            c.set("RECOMP_LSFG", "1")
            c.set("RECOMP_LSFG_MULT", "2")
            c.set("RECOMP_LSFG_PERF", "")
            configs[model.games[1].id] = c
            SettingsView.initialTab = .graphics
            model.route = .settings(model.games[1].id)
        case "controls": model.route = .controls(first)
        case "manage": model.route = .manage(model.games[1].id)
        case "log": model.route = .log(first)
        case "setup": model.route = .setup
        case "about": model.route = .about
        case "single", "single-missing":
            let m = model
            m.games = m.games.filter { m.accepts($0) }
            m.route = m.homeRoute
        default: model.route = .library
        }
        timer = Timer.scheduledTimer(withTimeInterval: 1, repeats: true) { [weak self] _ in
            Task { @MainActor in self?.step() }
        }
    }

    /// Advances the simulated prepare once a second.
    private func step() {
        model.tick += 1
        for g in model.games {
            guard case .running(var p, let started)? = g.prepare else { continue }
            p.pct += 1
            if p.pct > 100 {
                if p.step >= p.n {
                    model.update(g.id) { $0.prepare = nil; $0.status = .ready; $0.exe = "/demo/\(g.id)" }
                    continue
                }
                p.step += 1
                p.pct = 0
                p.id = PrepareProtocol.steps[p.step - 1]
            }
            if p.id == "build" { p.msg = "compiling \(p.pct * 4)/400" }
            model.update(g.id) { $0.prepare = .running(p, started: started) }
        }
    }

    static func art(hue: Double, rings: Bool = true, seed: Int) -> NSImage? {
        let n = 128
        var px = [UInt32](repeating: 0, count: n * n)
        for y in 0..<n {
            for x in 0..<n {
                let dx = Double(x - n / 2 + seed % 17), dy = Double(y - n / 3)
                let r = (dx * dx + dy * dy).squareRoot()
                let band = rings ? (sin(r / 5.5) * 0.5 + 0.5) : Double(y) / Double(n)
                let h = hue + Double(x + y) * 0.25
                let v = 0.18 + 0.75 * band * (1 - Double(y) / Double(n) * 0.5)
                px[y * n + x] = Palette.hsv(h, 0.72, v)
            }
        }
        guard let cg = TitleArt.cgImage(ArgbImage(width: n, height: n, pixels: px)) else { return nil }
        return NSImage(cgImage: cg, size: NSSize(width: n, height: n))
    }

    static func mockGames() -> [GameItem] {
        func item(_ tid: String, _ name: String, _ hue: Double?, kind: LibraryEntry.Kind, path: String,
                  status: TitleStatus) -> GameItem {
            let e = LibraryEntry(path: path, kind: kind, size: kind == .image ? 3_100_000_000 : 0, mtime: 0,
                                 titleId: tid, title: name)
            var g = GameItem(id: tid, entry: e, name: name, titleId: tid,
                             art: hue.flatMap { art(hue: $0, seed: Int(tid.suffix(2), radix: 16) ?? 0) },
                             accent: hue.map { Accent.color(hue: $0) } ?? Accent.color(hue: Double(TitleArt.titleHue(tid))))
            g.status = status
            if status == .ready { g.exe = "/demo/\(tid)" }
            return g
        }
        var games = [
            item("58410001", "Night Ops", 205, kind: .image, path: "~/Documents/xbox/Night Ops (USA)/Night Ops (USA).iso",
                 status: .ready),
            item("58410002", "Sample Racer", 28, kind: .image,
                 path: "~/Documents/xbox/Sample Racer (Europe)/Sample Racer (Europe).iso", status: .ready),
            item("58410003", "Hollow Keep", 130, kind: .folder, path: "~/Downloads/xboxrecomp/titles/keep/game",
                 status: .absent),
            item("58410004", "Star Courier", 265, kind: .image,
                 path: "~/Documents/xbox/Star Courier (USA)/Star Courier (USA).iso", status: .absent),
            item("12345678", "SYNTH", nil, kind: .folder, path: "~/Documents/xbox/Synthetic Game", status: .stale(reasons: ["toolkit"])),
            item("58410006", "Tide Runner", 175, kind: .image, path: "~/Documents/xbox/Tide Runner/Tide Runner.iso",
                 status: .unreadable("Not readable yet (still being copied?)")),
        ]
        // XBA-278: console games (homebrew names), one interpreted, one prepared.
        func consoleItem(_ id: String, _ name: String, _ console: Console, _ hue: Double, path: String,
                         status: TitleStatus) -> GameItem {
            let e = LibraryEntry(path: path, kind: .console, size: 4_194_304, mtime: 0, titleId: id, title: name,
                                 console: console.rawValue)
            var g = GameItem(id: id, entry: e, name: name, titleId: id, art: art(hue: hue, seed: name.count),
                             accent: Accent.color(hue: hue))
            g.status = status
            g.exe = "/demo/\(console.player ?? "")"
            return g
        }
        games.append(consoleItem("gba-DEMO-1A2B3C4D", "Pocket Quest", .gba, 95, path: "~/Documents/xbox/gba/Pocket Quest.gba",
                                 status: .absent))
        games.append(consoleItem("n64-NDMO-635A2BFF", "Kart Demo 64", .n64, 330, path: "~/Documents/xbox/n64/Kart Demo.z64",
                                 status: .ready))
        games[0].runningSince = Date().addingTimeInterval(-754)
        games[0].record.runs = 6
        games[0].record.playSeconds = 4 * 3600 + 12 * 60
        games[0].record.lastPlayed = Date().addingTimeInterval(-3600 * 20)
        games[1].record.runs = 3
        games[1].record.playSeconds = 2 * 3600 + 5 * 60
        games[1].record.lastPlayed = Date().addingTimeInterval(-3600 * 50)
        games[1].envForce = ["RECOMP_LSFG": "0"]
        games[2].prepare = .running(PrepareProtocol.Progress(step: 5, n: 5, pct: 42, id: "build",
                                                             msg: "compiling 168/400"),
                                    started: Date().addingTimeInterval(-1385))
        games[3].prepare = .failed(code: "COMPILE_FAILED",
                                   message: "cmake failed (exit 1), see ~/Library/Application Support/xboxrecomp/titles/58410004/work/prepare.log",
                                   logTail: [
                                       "[212/398] Building C object CMakeFiles/title.dir/gen/recomp_0041.c.o",
                                       "[213/398] Building C object CMakeFiles/title.dir/gen/recomp_0042.c.o",
                                       "FAILED: CMakeFiles/title.dir/gen/recomp_0043.c.o",
                                       "gen/recomp_0043.c:1812:5: error: use of undeclared identifier 'sub_0004A1F0'",
                                       "    sub_0004A1F0(ctx);",
                                       "    ^",
                                       "1 error generated.",
                                       "ninja: build stopped: subcommand failed.",
                                   ], log: URL(fileURLWithPath: "/tmp/prepare.log"))
        return games
    }

    static let mockLog: [String] = [
        "# /Users/you/Library/Application Support/xboxrecomp/titles/58410001/work/project/build/nightops",
        "# RECOMP_GPU=1",
        "# RECOMP_SAVE_DIR=/Users/you/Library/Application Support/xboxrecomp/titles/58410001/save",
        "[KERNEL] guest memory: 64 MB at 0x0000000100000000",
        "[GPUGL] OpenGL 4.1 Metal - 90.5, internal scale 1",
        "[APU] SDL audio: 48000 Hz, 2 channels, 512 samples",
        "[XAPI] XInputGetCapabilities: pad 0 connected (Xbox Wireless Controller)",
        "[FBWIN] aspect NORMAL -> WIDE (frame 1312, 182044 triangles)",
        "warning: [D3D] unhandled render state 0x0000015C",
        "[SAVE] UDATA/58410001/4F2A1B09 written (12 KB)",
    ]

    // ---- LauncherActions: pretend --------------------------------------------------------------------------------
    func rescan() {
        model.scanning = true
        DispatchQueue.main.asyncAfter(deadline: .now() + 1) { self.model.scanning = false }
    }
    func addGame(_ url: URL) { model.notice = "Demo mode: \(url.lastPathComponent) would be added." }
    func prepare(_ id: String, force: Bool) {
        model.update(id) {
            $0.prepare = .running(PrepareProtocol.Progress(step: 1, n: 5, pct: 0, id: "identify", msg: "finding default.xbe"),
                                  started: Date())
        }
    }
    func cancelPrepare(_ id: String) { model.update(id) { $0.prepare = nil } }
    func play(_ id: String) { model.update(id) { $0.runningSince = Date() } }
    func stop(_ id: String) { model.update(id) { $0.runningSince = nil } }
    func loadConfig(_ id: String?) -> RecompConfig { configs[id ?? "_"] ?? RecompConfig() }
    func saveConfig(_ config: RecompConfig, for id: String?) { configs[id ?? "_"] = config }
    func storage(_ id: String, done: @escaping ([(Storage.Item, Int64)]) -> Void) {
        if model.game(id)?.entry.isConsole == true {
            return done([(.work, 41_000_000), (.pack, 6_300_000), (.saves, 140_000), (.logs, 90_000), (.art, 61_000)])
        }
        done([(.work, 9_812_000_000), (.extractedDisc, 3_100_000_000), (.saves, 1_420_000), (.utilityCache, 880_000),
              (.logs, 2_300_000), (.art, 21_000)])
    }
    func delete(_ item: Storage.Item, of id: String, done: @escaping () -> Void) {
        model.notice = "Demo mode: nothing deleted."
        done()
    }
    func importSaves(_ id: String, done: @escaping (Int?) -> Void) { done(0) }
    func setEnhancements(_ id: String, on: Bool) { model.update(id) { $0.record.enhancements = on ? nil : false } }
    func createTitleApp(_ id: String, at url: URL) {
        model.notice = "Demo mode: \(url.lastPathComponent) would be made in \(url.deletingLastPathComponent().path)."
    }
    func openMainApp() { model.notice = "Demo mode: XRecomp would open." }
    func reveal(_ url: URL) {}
    func titlePaths(_ id: String) -> TitlePaths? { AppPaths(root: URL(fileURLWithPath: "/tmp/xrecomp-demo")).title(id) }
    func setToolkit(_ path: String) { model.setup.toolkit = path }
    func addLibraryFolder(_ path: String) { model.setup.libraryFolders.append(path) }
    func removeLibraryFolder(_ path: String) { model.setup.libraryFolders.removeAll { $0 == path } }
    func setWorkRoot(_ path: String?) { model.setup.workRoot = path }
    func recheckSetup() {}
    func createVenv() {}
    func refreshLsfg() {
        var s = LsfgCache.Status()
        s.key = "3f2a9c41d07e5b88"
        s.ready31 = true
        s.readyPerf = true
        model.lsfg.status = s
        model.lsfg.base = "~/Library/Caches/xboxrecomp/lsfg"
        model.lsfg.vulkan = "/opt/homebrew/lib/libvulkan.1.dylib"
    }
    func importLossless(_ url: URL) { model.notice = "Demo mode: \(url.lastPathComponent) would be translated." }
    func licences() -> [(String, String)] {
        [("LICENSE", "MIT License\n\nCopyright (c) the xboxrecomp contributors\n\nPermission is hereby granted, free of charge, …"),
         ("NOTICE", "Third-party notices for the toolkit."), ("LICENSES/zlib.txt", "zlib License …")]
    }
}
