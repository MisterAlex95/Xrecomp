import AppKit
import CoreServices
import XRecompCore

/// The real launcher: drives XRecompCore for the views. Settings and per-title files live under AppPaths
/// (Application Support); the toolkit is the checkout next to the app. One prepare at a time (the others queue),
/// one game at a time. Slow work (scan, --check, dependency checks, the venv) runs off the main thread.
@MainActor
final class AppStore: LauncherActions {
    /// The one store of the process (the delegate stops its children at quit).
    static weak var current: AppStore?
    /// A game app (Info.plist `XBASingleTitle`, or `$XRECOMP_SINGLE_TITLE`) runs on its one title; the data is the
    /// same Application Support tree as the main app's (nothing here depends on the bundle ID).
    let model = LauncherModel(singleTitle: TitleApp.singleTitle(info: Bundle.main.infoDictionary,
                                                                environment: ProcessInfo.processInfo.environment))
    private let basePaths = AppPaths.standard()
    private var settings: Settings
    private var paths: AppPaths
    private var toolkit: Toolkit?
    private var python: URL?
    private var queue = PrepareQueue()
    private var preparing: (id: String, runner: PrepareRunner)?
    private var game: (id: String, runner: GameRunner)?
    private var ticker: Timer?
    /// A rescan asked for while one ran (a folder dropped, Setup found Python): run again once it ends.
    private var rescanPending = false
    private let work = DispatchQueue(label: "xrecomp.store", qos: .userInitiated)

    init() {
        settings = Settings.load(from: basePaths.settingsFile)
        paths = settings.paths(base: basePaths)
        model.actions = self
        AppStore.current = self
    }

    func start() {
        model.setup.appHome = basePaths.root.path
        model.setup.libraryFolders = settings.libraryFolders
        model.setup.workRoot = settings.workRoot
        ticker = Timer.scheduledTimer(withTimeInterval: 1, repeats: true) { [weak self] _ in
            Task { @MainActor in
                guard let self, self.preparing != nil || self.game != nil else { return }
                self.model.tick += 1
            }
        }
        recheckSetup(then: { [weak self] in self?.rescan() })
        refreshLsfg()
    }

    // ---- setup ---------------------------------------------------------------------------------------------------

    func recheckSetup() { recheckSetup(then: nil) }

    private func recheckSetup(then: (() -> Void)?) {
        model.setup.checking = true
        let s = settings, base = basePaths
        work.async {
            let tk = Toolkit.resolveForApp(settings: s)
            let py = PythonEnv.resolve(settingPath: s.pythonPath, toolkit: tk?.root, appVenv: base.venv)
            let commit = tk?.commit()
            let deps = DependencyCheck.run(python: py,
                                           prebuiltRuntime: DependencyCheck.hasPrebuiltRuntime(toolkit: tk?.root))
            DispatchQueue.main.async { [self] in
                toolkit = tk
                python = py
                model.setup.toolkit = tk?.root.path
                model.setup.toolkitSource = tk.map(sourceName)
                model.setup.toolkitCommit = commit
                model.setup.python = py?.path
                model.setup.dependencies = deps
                model.setup.checking = false
                if tk == nil && model.route == model.homeRoute { model.route = .setup }
                then?()
            }
        }
    }

    private func sourceName(_ t: Toolkit) -> String {
        switch t.source {
        case .setting: return "your setting"
        case .environment: return "XBA_TOOLKIT"
        case .infoPlist: return "the app (build-app.sh)"
        case .bundle: return "the folders around the app"
        }
    }

    private func saveSettings() {
        do { try settings.save(to: basePaths.settingsFile) } catch { model.notice = "Cannot save the settings: \(error.localizedDescription)" }
        paths = settings.paths(base: basePaths)
        model.setup.libraryFolders = settings.libraryFolders
        model.setup.workRoot = settings.workRoot
    }

    func setToolkit(_ path: String) {
        guard Toolkit.isValid(URL(fileURLWithPath: path)) else {
            model.notice = "\(path) is not an xboxrecomp-android checkout (no tools/prepare or templates/new-game)."
            return
        }
        settings.toolkitPath = path
        saveSettings()
        recheckSetup(then: { [weak self] in self?.rescan() })
    }

    func addLibraryFolder(_ path: String) {
        let tilde = (path as NSString).abbreviatingWithTildeInPath
        if !settings.libraryFolders.contains(tilde) && !settings.libraryFolders.contains(path) {
            settings.libraryFolders.append(tilde)
            saveSettings()
        }
        rescan()
    }

    func removeLibraryFolder(_ path: String) {
        settings.libraryFolders.removeAll { $0 == path }
        if settings.libraryFolders.isEmpty { settings.libraryFolders = [Settings.defaultLibrary] }
        saveSettings()
        rescan()
    }

    func setWorkRoot(_ path: String?) {
        settings.workRoot = path
        saveSettings()
        refreshStates()
    }

    func createVenv() {
        guard let base = PythonEnv.findBase() else {
            model.notice = "No Python 3.10 or later found. Install one with: brew install python@3.12"
            return
        }
        model.setup.creatingVenv = true
        model.setup.venvLog = []
        let venv = basePaths.venv
        work.async {
            let ok = PythonEnv.createVenv(at: venv, base: base) { line in
                DispatchQueue.main.async { self.model.setup.venvLog.append(line) }
            }
            DispatchQueue.main.async { [self] in
                model.setup.creatingVenv = false
                if !ok { model.notice = "Creating the Python environment failed; see its log in Setup." }
                recheckSetup(then: { [weak self] in self?.rescan() })
            }
        }
    }

    func licences() -> [(String, String)] {
        guard let tk = toolkit else { return [] }
        return tk.licenceFiles().map { u in
            (u.path.replacingOccurrences(of: tk.root.path + "/", with: ""),
             (try? String(contentsOf: u, encoding: .utf8)) ?? "")
        }
    }

    // ---- library -------------------------------------------------------------------------------------------------

    func rescan() {
        guard !model.scanning else {
            rescanPending = true
            return
        }
        model.scanning = true
        let folders = settings.libraryURLs, p = paths, tk = toolkit, py = python
        work.async {
            var scanner = LibraryScanner { _ in .failure(.init("Needs the Python environment (Setup)")) }
            if let tk, let py {
                scanner.identifyImage = LibraryScanner.pythonIdentify(python: py.path, toolkit: tk.root, scanDir: p.scanDir)
            }
            var index = LibraryIndex.load(from: p.libraryFile)
            let entries = scanner.scan(folders, index: &index)
            try? index.save(to: p.libraryFile)
            let items = entries.map { AppStore.item(for: $0, paths: p) }
            DispatchQueue.main.async { [self] in
                merge(items)
                model.scanning = false
                model.scannedOnce = true
                if rescanPending {
                    // The folders or the toolkit / Python changed during this scan: its results are already old.
                    rescanPending = false
                    rescan()
                } else {
                    refreshStates()
                }
            }
        }
    }

    /// A GameItem for a scanned entry: art (cached art.png, else decoded from the XBE), accent, state.json.
    nonisolated static func item(for e: LibraryEntry, paths: AppPaths) -> GameItem {
        var art: NSImage?
        var accent = Accent.color(hue: Double(TitleArt.titleHue(e.titleId ?? e.path)))
        var record = TitleRecord()
        if let tid = e.titleId {
            let t = paths.title(tid)
            if let cg = TitleArt.load(cache: t.art, xbe: e.xbe.map { URL(fileURLWithPath: $0) }) {
                art = NSImage(cgImage: cg, size: NSSize(width: cg.width, height: cg.height))
                accent = Accent.forTitle(image: TitleArt.argb(cg), titleId: tid)
            }
            record = TitleRecord.load(from: t.state)
        }
        var g = GameItem(id: e.id, entry: e, name: e.displayName, titleId: e.titleId, art: art, accent: accent)
        g.record = record
        if let why = e.problem { g.status = .unreadable(why) }
        return g
    }

    /// New scan results in, keeping what the running prepare and game know.
    private func merge(_ items: [GameItem]) {
        var out: [GameItem] = []
        for var n in items where model.accepts(n) {
            if let old = model.game(n.id) {
                n.prepare = old.prepare
                n.runningSince = old.runningSince
                n.lastExit = old.lastExit
                n.lastExitTail = old.lastExitTail
                if case .unreadable = n.status {} else {
                    n.status = old.status
                    n.exe = old.exe
                    n.envForce = old.envForce
                    n.titleLayer = old.titleLayer
                }
            }
            out.append(n)
        }
        // A game that runs or prepares stays listed even if its folder went away meanwhile.
        for old in model.games where !out.contains(where: { $0.id == old.id })
            && (old.runningSince != nil || old.prepare != nil) {
            out.append(old)
        }
        model.games = out
    }

    func addGame(_ url: URL) {
        let isDir = FileManager.default.isDirectory(url)
        if isDir {
            addLibraryFolder(url.path)
        } else if LibraryScanner.imageExtensions.contains(url.pathExtension.lowercased()) {
            addLibraryFolder(url.deletingLastPathComponent().path)
        } else if url.lastPathComponent.lowercased() == "default.xbe" {
            addLibraryFolder(url.deletingLastPathComponent().path)
        } else {
            model.notice = "\(url.lastPathComponent) is not a disc image (.iso) or a folder holding default.xbe."
        }
    }

    // ---- title state ---------------------------------------------------------------------------------------------

    private func refreshStates() {
        for g in model.games where g.titleId != nil { refreshState(g.id) }
    }

    /// `prepare --check --json` for one title, off the main thread.
    private func refreshState(_ id: String) {
        guard let g = model.game(id), let tid = g.titleId, let tk = toolkit, let py = python else { return }
        if case .unreadable = g.status { return }
        let source = URL(fileURLWithPath: g.entry.path), workDir = paths.title(tid).work
        // The stamp is compared with the prepare this title would get: its enhancements off, a build made with them
        // is stale (reason "profile"), and the other way round.
        let env = ChildEnvironment.prepare(enhancements: TitleRecord.load(from: paths.title(tid).state).enhancementsOn)
        work.async {
            let st = TitleState.check(python: py.path, toolkit: tk.root, source: source, work: workDir,
                                      environment: env)
            DispatchQueue.main.async { [self] in
                model.update(id) { item in
                    guard let st else {
                        item.status = .error(code: "CHECK_FAILED", message: "prepare --check did not answer")
                        return
                    }
                    if let t = st.title, !t.isEmpty { item.name = t }
                    item.envForce = st.envForce
                    item.titleLayer = st.titleLayer
                    item.exe = st.playable ? st.exe : nil
                    switch st.state {
                    case .ready: item.status = .ready
                    case .stale: item.status = .stale(reasons: st.reasons)
                    case .absent: item.status = .absent
                    case .error: item.status = .error(code: st.code ?? "ERROR", message: st.msg ?? "")
                    }
                }
            }
        }
    }

    func titlePaths(_ id: String) -> TitlePaths? {
        model.game(id)?.titleId.map { paths.title($0) }
    }

    // ---- prepare -------------------------------------------------------------------------------------------------

    func prepare(_ id: String, force: Bool) {
        guard let g = model.game(id), let tid = g.titleId else { return }
        guard toolkit != nil, python != nil else {
            model.notice = "Finish Setup first: XRecomp needs the toolkit and its Python environment."
            model.route = .setup
            return
        }
        let t = paths.title(tid)
        // A first prepare of an image extracts the disc: check the room for it.
        if g.status == .absent, let free = Storage.freeSpace(at: t.work) {
            let need = Storage.estimate(sourceKind: g.entry.kind, sourceSize: g.entry.size)
            if free < need {
                model.notice = "Not enough free space: preparing \(g.name) needs about \(Storage.format(need)), "
                    + "\(Storage.format(free)) is free on that disk. Free some space or move the work folders (Setup)."
                return
            }
        }
        if preparing != nil {
            // Queued with its own --force: a Re-prepare… asked while another title prepares stays a full rebuild.
            if preparing?.id != id { queue.enqueue(id, force: force) }
            updateQueuePositions()
            return
        }
        startPrepare(id, force: force)
    }

    private func startPrepare(_ id: String, force: Bool) {
        guard let g = model.game(id), let tid = g.titleId, let tk = toolkit, let py = python else { return }
        let t = paths.title(tid)
        var req = PrepareRunner.Request(python: py.path, toolkit: tk.root, source: URL(fileURLWithPath: g.entry.path),
                                        work: t.work)
        req.force = force
        var record = TitleRecord.load(from: t.state)
        req.noTitleLayer = !record.enhancementsOn
        req.environment = ChildEnvironment.prepare(req.environment, enhancements: record.enhancementsOn)
        record.sourcePath = g.entry.path
        try? record.save(to: t.state)
        let runner = PrepareRunner(req) { [weak self] e in
            DispatchQueue.main.async { self?.prepareEvent(id, e) }
        }
        model.liveLog = []
        model.liveLogTitle = "\(g.name) · prepare"
        model.update(id) {
            $0.prepare = .running(PrepareProtocol.Progress(step: 0, n: 5, pct: 0, id: "", msg: "starting"), started: Date())
        }
        do {
            try runner.start()
            preparing = (id, runner)
        } catch {
            model.update(id) {
                $0.prepare = .failed(code: "START_FAILED", message: "\(error)", logTail: [], log: nil)
            }
            nextInQueue()
        }
    }

    private func prepareEvent(_ id: String, _ e: PrepareRunner.Event) {
        switch e {
        case .progress(let p):
            model.appendLog("· \(p.id) \(p.pct) % \(p.msg)")
            model.update(id) { item in
                if case .running(_, let started)? = item.prepare { item.prepare = .running(p, started: started) }
            }
        case .line(let l):
            model.appendLog(l)
        case .finished(let outcome):
            preparing = nil
            guard let tid = model.game(id)?.titleId else { return nextInQueue() }
            let t = paths.title(tid)
            var record = TitleRecord.load(from: t.state)
            switch outcome {
            case .ok:
                model.update(id) { $0.prepare = nil }
                record.lastPrepare = .init(ok: true)
            case .cancelled:
                model.update(id) { $0.prepare = nil }
                record.lastPrepare = .init(ok: false, code: "CANCELLED")
            case .failed(let code, let message):
                var tail = tailLines(t.work.appendingPathComponent(PrepareRunner.logName), 40)
                    .filter { !$0.hasPrefix("XBA_PROGRESS") }
                let toolLog = tailLines(t.prepareLog, 40)
                if !toolLog.isEmpty { tail = toolLog }
                model.update(id) {
                    $0.prepare = .failed(code: code, message: message, logTail: tail,
                                         log: FileManager.default.fileExists(atPath: t.prepareLog.path) ? t.prepareLog
                                             : t.work.appendingPathComponent(PrepareRunner.logName))
                }
                record.lastPrepare = .init(ok: false, code: code, message: message)
            }
            try? record.save(to: t.state)
            refreshState(id)
            nextInQueue()
        }
    }

    private func nextInQueue() {
        guard preparing == nil, let next = queue.next() else { return updateQueuePositions() }
        updateQueuePositions()
        startPrepare(next.id, force: next.force)
    }

    private func updateQueuePositions() {
        for g in model.games {
            if let position = queue.position(of: g.id) {
                model.update(g.id) { $0.prepare = .queued(position: position) }
            } else if case .queued? = g.prepare {
                model.update(g.id) { $0.prepare = nil }
            }
        }
    }

    func cancelPrepare(_ id: String) {
        if queue.remove(id) {
            updateQueuePositions()
            return
        }
        if preparing?.id == id { preparing?.runner.cancel() }
        if case .failed? = model.game(id)?.prepare { model.update(id) { $0.prepare = nil } }
    }

    // ---- play ----------------------------------------------------------------------------------------------------

    func play(_ id: String) {
        guard game == nil else { return model.notice = "Another game is running: stop it first." }
        guard let g = model.game(id), let tid = g.titleId, let exe = g.exe else { return }
        let t = paths.title(tid)
        let cfg = RecompConfig.load(dir: t.recompCfgDir)
        let defaults = loadConfig(nil).activePairs.reduce(into: [String: String]()) { $0[$1.key] = $1.value }
        let req = GameRunner.Request.forTitle(exe: URL(fileURLWithPath: exe), title: t, config: cfg,
                                              globalDefaults: defaults, logs: RunLog(dir: t.logs))
        try? FileManager.default.ensureDirectory(t.save)
        let runner = GameRunner(req) { [weak self] e in
            DispatchQueue.main.async { self?.gameEvent(id, e) }
        }
        model.liveLog = []
        model.liveLogTitle = "\(g.name) · run started \(Date().formatted(date: .omitted, time: .shortened))"
        do {
            try runner.start()
            game = (id, runner)
            model.update(id) {
                $0.runningSince = Date()
                $0.lastExit = nil
                $0.lastExitTail = []
            }
        } catch {
            model.notice = "Cannot start \(g.name): \(error)"
        }
    }

    private func gameEvent(_ id: String, _ e: GameRunner.Event) {
        switch e {
        case .line(let l):
            model.appendLog(l)
        case .exited(let exit, let byUser, let seconds):
            let runLog = game?.id == id ? game?.runner.request.log : nil
            game = nil
            let problem = GameRunner.exitProblem(exit, stoppedByUser: byUser)
            // The tail comes from this run's own log: the live log is shared with a prepare that may have started
            // (and reset it) meanwhile, and would show that prepare's output as the crash.
            let tail = runLog.map { tailLines($0, 40) } ?? Array(model.liveLog.suffix(40))
            model.update(id) {
                $0.runningSince = nil
                $0.lastExit = problem
                $0.lastExitTail = problem == nil ? [] : tail
            }
            if let tid = model.game(id)?.titleId {
                let t = paths.title(tid)
                var r = TitleRecord.load(from: t.state)
                r.recordRun(seconds: seconds)
                try? r.save(to: t.state)
                model.update(id) { $0.record = r }
            }
            // A run may have found new code (discovered.jsonl): the title can turn stale.
            refreshState(id)
        }
    }

    func stop(_ id: String) {
        if game?.id == id { game?.runner.stop() }
    }

    // ---- settings ------------------------------------------------------------------------------------------------

    private var defaultsDir: URL { basePaths.root.appendingPathComponent("defaults", isDirectory: true) }

    func loadConfig(_ id: String?) -> RecompConfig {
        if let id, let t = titlePaths(id) { return RecompConfig.load(dir: t.recompCfgDir) }
        return RecompConfig.load(dir: defaultsDir)
    }

    func saveConfig(_ config: RecompConfig, for id: String?) {
        var c = config
        if c.file == nil {
            // A config built in memory: put its lines in the right file.
            var target = id.flatMap { titlePaths($0) }.map { RecompConfig.load(dir: $0.recompCfgDir) }
                ?? RecompConfig.load(dir: defaultsDir)
            target.setUnmanagedText(c.unmanagedText())
            for key in OptionTable.all.flatMap(\.keys) { target.set(key, c.get(key)) }
            c = target
        }
        do {
            try c.save()
        } catch {
            model.notice = "Cannot save the settings: \(error.localizedDescription)"
        }
        if id == nil {
            settings.globalDefaults = c.activePairs.reduce(into: [:]) { $0[$1.key] = $1.value }
            saveSettings()
        }
    }

    // ---- frame generation ----------------------------------------------------------------------------------------

    /// The desktop cache base the runtime reads when RECOMP_LSFG_DIR is unset.
    private var lsfgBase: URL { LsfgCache.defaultBase() }

    func refreshLsfg() {
        let base = lsfgBase
        work.async {
            let s = LsfgCache.status(base: base)
            let vk = VulkanLoader.find()
            DispatchQueue.main.async { [self] in
                model.lsfg.status = s
                model.lsfg.base = base.path
                model.lsfg.vulkan = vk?.path
            }
        }
    }

    func importLossless(_ url: URL) {
        guard model.lsfg.busy == nil else { return }
        guard let tk = toolkit else {
            model.notice = "Finish Setup first: the shader translator is built from the toolkit."
            return
        }
        let base = lsfgBase, appRoot = basePaths.root
        model.lsfg.busy = "Starting…"
        model.lsfg.error = nil
        model.lsfg.log = []
        // Not the store's serial queue: the first import builds the translator (minutes) and must not hold scans.
        DispatchQueue.global(qos: .userInitiated).async {
            let r = LsfgCache.importDll(url, toolkit: tk.root, appRoot: appRoot, base: base) { step in
                DispatchQueue.main.async { self.model.lsfg.busy = step }
            }
            DispatchQueue.main.async { [self] in
                model.lsfg.busy = nil
                model.lsfg.status = r.status
                model.lsfg.base = base.path
                model.lsfg.error = r.message
                model.lsfg.log = Array(r.log.suffix(300))
            }
        }
    }

    // ---- storage -------------------------------------------------------------------------------------------------

    func storage(_ id: String) -> [(Storage.Item, Int64)] {
        guard let t = titlePaths(id) else { return [] }
        return Storage.sizes(t)
    }

    func delete(_ item: Storage.Item, of id: String) {
        guard let t = titlePaths(id) else { return }
        if game?.id == id || preparing?.id == id { return model.notice = "Stop the game or the prepare first." }
        do {
            try Storage.delete(item, t)
        } catch {
            model.notice = "Cannot delete: \(error.localizedDescription)"
        }
        if item == .work || item == .extractedDisc { refreshState(id) }
    }

    func setEnhancements(_ id: String, on: Bool) {
        guard let t = titlePaths(id) else { return }
        var r = TitleRecord.load(from: t.state)
        r.enhancements = on ? nil : false
        do {
            try r.save(to: t.state)
        } catch {
            return model.notice = "Cannot save the setting: \(error.localizedDescription)"
        }
        model.update(id) { $0.record = r }
        refreshState(id)     // a build made the other way is stale now: Home offers Prepare again
    }

    func importSaves(_ id: String) -> Int {
        guard let t = titlePaths(id) else { return 0 }
        do { return try SaveImport.run(titleId: t.titleId, to: t.save) } catch {
            model.notice = "Import failed: \(error.localizedDescription)"
            return 0
        }
    }

    // ---- game apps -----------------------------------------------------------------------------------------------

    /// `<Game title>.app` at dest: a copy of this XRecomp.app with the game's name, bundle ID and icon and
    /// `XBASingleTitle`, signed ad hoc. The icon is drawn from the cached title image (else the monogram).
    func createTitleApp(_ id: String, at dest: URL) {
        guard let g = model.game(id), let tid = g.titleId else { return }
        guard model.singleTitle == nil else { return }
        // Only for a prepared game (as on Android): the app opens straight on it, so it must be playable at once.
        guard g.playable else {
            return model.notice = "Prepare \(g.name) first, then create its app."
        }
        let source = Bundle.main.bundleURL
        guard source.pathExtension == "app" else {
            return model.notice = "Only XRecomp.app can make a game app: build it with macos/scripts/build-app.sh "
                + "and open it from Finder (not swift run)."
        }
        guard model.creatingApp == nil else { return }
        let t = paths.title(tid), xbe = g.entry.xbe.map { URL(fileURLWithPath: $0) }
        let name = g.name, accent = g.accent, toolkitPath = toolkit?.root.path
        model.creatingApp = id
        work.async {
            var problem: String?
            do {
                let art = TitleArt.load(cache: t.art, xbe: xbe)
                guard let icon = TitleApp.iconImage(art: art, title: name, titleId: tid, accent: accent),
                      let icns = TitleApp.icns(master: icon) else { throw TitleApp.BuildError.icon }
                try TitleApp.build(source: source, dest: dest, title: name, titleId: tid, icns: icns,
                                   toolkitPath: toolkitPath)
                let r = TitleApp.sign(dest)
                if !r.ok {
                    problem = "codesign failed (exit \(r.status)): "
                        + r.output.trimmingCharacters(in: .whitespacesAndNewlines)
                }
            } catch let e as TitleApp.BuildError {
                problem = e.description
            } catch {
                problem = error.localizedDescription
            }
            DispatchQueue.main.async { [self] in
                model.creatingApp = nil
                if let problem {
                    model.notice = "Cannot create \(dest.lastPathComponent): \(problem)"
                    return
                }
                // Launch Services learns the new bundle (name, icon) now rather than at its next scan.
                _ = LSRegisterURL(dest as CFURL, true)
                NSWorkspace.shared.activateFileViewerSelecting([dest])
                model.notice = "\(dest.lastPathComponent) is ready in \(dest.deletingLastPathComponent().path). "
                    + "It opens straight on \(name) and shares this library, its settings and saves."
            }
        }
    }

    /// A game app's « Open XRecomp »: the app it was made from, else the one Launch Services knows by bundle ID,
    /// else the toolkit's macos/build/XRecomp.app.
    func openMainApp() {
        let info = Bundle.main.infoDictionary ?? [:]
        var candidates: [URL] = []
        if let p = info[TitleApp.mainAppPathKey] as? String, !p.isEmpty { candidates.append(URL(fileURLWithPath: p)) }
        let bid = (info[TitleApp.mainBundleIdKey] as? String) ?? TitleApp.mainBundleId
        if let u = NSWorkspace.shared.urlForApplication(withBundleIdentifier: bid) { candidates.append(u) }
        if let tk = toolkit { candidates.append(tk.root.appendingPathComponent("macos/build/XRecomp.app")) }
        let me = Bundle.main.bundleURL.standardizedFileURL.path
        guard let app = candidates.first(where: {
            $0.standardizedFileURL.path != me
                && FileManager.default.fileExists(atPath: $0.appendingPathComponent("Contents/Info.plist").path)
        }) else {
            return model.notice = "XRecomp.app was not found. Build it with macos/scripts/build-app.sh."
        }
        NSWorkspace.shared.openApplication(at: app, configuration: NSWorkspace.OpenConfiguration()) { _, error in
            guard let error else { return }
            let text = "Cannot open XRecomp: \(error.localizedDescription)"
            DispatchQueue.main.async { AppStore.current?.model.notice = text }
        }
    }

    func reveal(_ url: URL) {
        if FileManager.default.fileExists(atPath: url.path) {
            NSWorkspace.shared.activateFileViewerSelecting([url])
        } else {
            NSWorkspace.shared.open(url.deletingLastPathComponent())
        }
    }

    /// Quitting stops a running game and a prepare (by PID / process group), so nothing outlives the launcher.
    func shutdown() {
        game?.runner.stop()
        preparing?.runner.cancel()
        _ = game?.runner.wait(timeout: 6)
        _ = preparing?.runner.wait(timeout: 6)
    }
}
