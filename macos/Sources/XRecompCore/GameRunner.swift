import Foundation

/// Runs a prepared game: `<work>/project/build/<name>` with cwd the build folder (its `game` link is the disc),
/// the environment of ChildEnvironment.game, output to `logs/run-<timestamp>.log` and to a callback for the live log.
/// One game at a time is the store's rule; Stop is SIGTERM by PID, then SIGKILL after 5 s, never by name.
public final class GameRunner: @unchecked Sendable {
    public struct Request: Sendable {
        public var exe: URL
        public var cwd: URL
        public var environment: [String: String]
        public var log: URL?

        public init(exe: URL, cwd: URL? = nil, environment: [String: String], log: URL?) {
            self.exe = exe
            self.cwd = cwd ?? exe.deletingLastPathComponent()
            self.environment = environment
            self.log = log
        }

        /// The request for a title: its recomp.cfg lines over the global defaults, its save folder.
        public static func forTitle(exe: URL, title: TitlePaths, config: RecompConfig, globalDefaults: [String: String],
                                    inherited: [String: String] = ProcessInfo.processInfo.environment,
                                    logs: RunLog) -> Request {
            let env = ChildEnvironment.game(inherited: inherited, globalDefaults: globalDefaults,
                                            config: config.activePairs, saveDir: title.save)
            return Request(exe: exe, environment: env, log: logs.newLog())
        }
    }

    public enum Event: Sendable {
        case line(String)
        case exited(ChildExit, stoppedByUser: Bool, seconds: TimeInterval)
    }

    public let request: Request
    public var stopGrace: TimeInterval = 5
    public private(set) var startedAt: Date?
    private var child: ChildProcess?
    private let lock = NSLock()
    private var stopRequested = false
    private var logHandle: FileHandle?
    private let handler: @Sendable (Event) -> Void

    public init(_ request: Request, handler: @escaping @Sendable (Event) -> Void) {
        self.request = request
        self.handler = handler
    }

    public func start() throws {
        if let log = request.log {
            try FileManager.default.ensureDirectory(log.deletingLastPathComponent())
            FileManager.default.createFile(atPath: log.path, contents: nil)
            logHandle = try? FileHandle(forWritingTo: log)
            let keys = request.environment.keys.filter { $0.hasPrefix("RECOMP_") || $0.hasPrefix("SDL_") }.sorted()
            write("# \(request.exe.path)")
            for k in keys { write("# \(k)=\(request.environment[k]!)") }
        }
        startedAt = Date()
        child = try ChildProcess(executable: request.exe.path, arguments: [], cwd: request.cwd,
                                 environment: request.environment, newProcessGroup: false,
                                 onLine: { [weak self] l in
                                     self?.write(l)
                                     self?.handler(.line(l))
                                 },
                                 onExit: { [weak self] e in self?.ended(e) })
    }

    public var pid: pid_t? { child?.pid }
    public var isRunning: Bool { child?.isRunning ?? false }

    /// SIGTERM by PID, SIGKILL after stopGrace.
    public func stop() {
        lock.lock()
        stopRequested = true
        lock.unlock()
        child?.terminate(grace: stopGrace, group: false)
    }

    @discardableResult
    public func wait(timeout: TimeInterval) -> ChildExit? { child?.wait(timeout: timeout) }

    private func write(_ s: String) {
        lock.lock()
        defer { lock.unlock() }
        if let h = logHandle, !appendLogLine(s, to: h) { logHandle = nil }
    }

    private func ended(_ e: ChildExit) {
        let secs = startedAt.map { Date().timeIntervalSince($0) } ?? 0
        write("# ended: \(e.description) after \(Int(secs)) s")
        lock.lock()
        let byUser = stopRequested
        try? logHandle?.close()
        logHandle = nil
        lock.unlock()
        handler(.exited(e, stoppedByUser: byUser, seconds: secs))
    }

    /// The words for an exit: nil for a clean one (or one the player asked for).
    public static func exitProblem(_ e: ChildExit, stoppedByUser: Bool) -> String? {
        if stoppedByUser { return nil }
        switch e {
        case .exited(0): return nil
        case .exited(let c): return "Stopped unexpectedly (exit \(c))"
        case .signaled(let s): return "Stopped unexpectedly (signal \(s))"
        }
    }
}

/// A title's run logs: `logs/run-<yyyyMMdd-HHmmss>.log`, the last `keep` kept.
public struct RunLog: Sendable {
    public let dir: URL
    public var keep = 10

    public init(dir: URL, keep: Int = 10) {
        self.dir = dir
        self.keep = keep
    }

    /// A new log's path (the oldest ones beyond keep - 1 removed first, so keep remain with it).
    public func newLog(now: Date = Date()) -> URL {
        try? FileManager.default.ensureDirectory(dir)
        rotate(leaving: keep - 1)
        let f = DateFormatter()
        f.locale = Locale(identifier: "en_US_POSIX")
        f.dateFormat = "yyyyMMdd-HHmmss"
        var url = dir.appendingPathComponent("run-\(f.string(from: now)).log")
        var n = 1
        while FileManager.default.fileExists(atPath: url.path) {
            n += 1
            url = dir.appendingPathComponent("run-\(f.string(from: now))-\(n).log")
        }
        return url
    }

    /// The run logs, oldest first.
    public func logs() -> [URL] {
        let names = (try? FileManager.default.contentsOfDirectory(atPath: dir.path)) ?? []
        return names.filter { $0.hasPrefix("run-") && $0.hasSuffix(".log") }.sorted().map { dir.appendingPathComponent($0) }
    }

    public func rotate(leaving n: Int) {
        let all = logs()
        guard all.count > n else { return }
        for u in all.prefix(all.count - max(0, n)) { try? FileManager.default.removeItem(at: u) }
    }

    public var latest: URL? { logs().last }
}

/// Appends `s` and a newline to a log. The throwing `write(contentsOf:)`, never the legacy `write(_:)`: that one
/// raises an Objective-C exception on ENOSPC / EIO (a full disk, an unplugged work root) that Swift cannot catch, and
/// the launcher would abort mid-prepare. False when the write failed: the caller drops the handle and goes on
/// without a log.
public func appendLogLine(_ s: String, to handle: FileHandle) -> Bool {
    do {
        try handle.write(contentsOf: Data((s + "\n").utf8))
        return true
    } catch {
        return false
    }
}

/// The last `n` lines of a text file (reads at most the last 256 KB).
public func tailLines(_ url: URL, _ n: Int) -> [String] {
    guard let h = FileHandle(forReadingAtPath: url.path) else { return [] }
    defer { try? h.close() }
    let size = (try? h.seekToEnd()) ?? 0
    let start = size > 262_144 ? size - 262_144 : 0
    try? h.seek(toOffset: start)
    let data = (try? h.readToEnd()) ?? Data()
    var lines = String(decoding: data, as: UTF8.self).components(separatedBy: "\n")
    if lines.last == "" { lines.removeLast() }
    if start > 0, !lines.isEmpty { lines.removeFirst() }
    return Array(lines.suffix(n))
}
