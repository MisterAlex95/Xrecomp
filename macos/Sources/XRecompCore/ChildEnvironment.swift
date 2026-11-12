import Foundation

/// The environment of every process the launcher starts. An app opened from Finder gets launchd's PATH
/// (/usr/bin:/bin:/usr/sbin:/sbin), without Homebrew: prepare would fail at `cmake` only when started from Finder.
public enum ChildEnvironment {
    /// Homebrew (Apple silicon, then Intel) and the system, before whatever PATH the launcher inherited.
    public static let fixedPath = ["/opt/homebrew/bin", "/opt/homebrew/sbin", "/usr/local/bin", "/usr/bin", "/bin",
                                   "/usr/sbin", "/sbin"]

    /// The fixed entries, then the inherited ones not already there (order kept, empties dropped).
    public static func path(inherited: String?) -> String {
        var seen = Set<String>()
        var out: [String] = []
        for p in fixedPath + (inherited ?? "").split(separator: ":").map(String.init) where !p.isEmpty {
            if seen.insert(p).inserted { out.append(p) }
        }
        return out.joined(separator: ":")
    }

    /// The launcher's own environment with the fixed PATH.
    public static func base(_ env: [String: String] = ProcessInfo.processInfo.environment) -> [String: String] {
        var e = env
        e["PATH"] = path(inherited: env["PATH"])
        return e
    }

    /// A game's environment: the fixed PATH, then the global defaults, then the title's recomp.cfg lines (the first
    /// line for a key wins, as the runtime reads the file), then RECOMP_SAVE_DIR. A recomp.cfg value replaces an
    /// inherited variable of the same name; PATH and RECOMP_SAVE_DIR cannot be set from the file.
    public static func game(inherited: [String: String], globalDefaults: [String: String] = [:],
                            config: [(key: String, value: String)], saveDir: URL?) -> [String: String] {
        var e = base(inherited)
        for (k, v) in globalDefaults where k != "PATH" && k != "RECOMP_SAVE_DIR" { e[k] = v }
        for (k, v) in config where k != "PATH" && k != "RECOMP_SAVE_DIR" { e[k] = v }
        if let saveDir { e["RECOMP_SAVE_DIR"] = saveDir.path }
        return e
    }
}

/// Short synchronous commands (version checks, git, pip): output captured, stdout and stderr merged.
public enum Shell {
    public struct Result: Sendable {
        public let status: Int32
        public let output: String
        public var ok: Bool { status == 0 }

        public init(status: Int32, output: String) {
            self.status = status
            self.output = output
        }
    }

    /// Runs `exe args`; status -1 when it could not start, -2 on timeout (it is then killed).
    @discardableResult
    public static func run(_ exe: String, _ args: [String], cwd: URL? = nil, environment: [String: String]? = nil,
                           timeout: TimeInterval = 60) -> Result {
        let p = Process()
        p.executableURL = URL(fileURLWithPath: exe)
        p.arguments = args
        if let cwd { p.currentDirectoryURL = cwd }
        p.environment = environment ?? ChildEnvironment.base()
        let pipe = Pipe()
        p.standardOutput = pipe
        p.standardError = pipe
        p.standardInput = FileHandle.nullDevice
        let collected = LockedData()
        pipe.fileHandleForReading.readabilityHandler = { h in
            let d = h.availableData
            if d.isEmpty { h.readabilityHandler = nil } else { collected.append(d) }
        }
        let done = DispatchSemaphore(value: 0)
        p.terminationHandler = { _ in done.signal() }
        do { try p.run() } catch {
            pipe.fileHandleForReading.readabilityHandler = nil
            return Result(status: -1, output: "\(error.localizedDescription)")
        }
        var status: Int32
        if done.wait(timeout: .now() + timeout) == .timedOut {
            p.terminate()
            if done.wait(timeout: .now() + 2) == .timedOut { kill(p.processIdentifier, SIGKILL); done.wait() }
            status = -2
        } else {
            status = p.terminationStatus
        }
        // Drain what is left in the pipe.
        pipe.fileHandleForReading.readabilityHandler = nil
        if let rest = try? pipe.fileHandleForReading.readToEnd() { collected.append(rest) }
        return Result(status: status, output: String(decoding: collected.data, as: UTF8.self))
    }

    /// The first executable called `name` on the fixed PATH, or nil.
    public static func which(_ name: String, path: String = ChildEnvironment.path(inherited: nil)) -> String? {
        for dir in path.split(separator: ":") {
            let p = "\(dir)/\(name)"
            if FileManager.default.isExecutableFile(atPath: p) { return p }
        }
        return nil
    }
}

final class LockedData: @unchecked Sendable {
    private var buf = Data()
    private let lock = NSLock()

    func append(_ d: Data) {
        lock.lock()
        buf.append(d)
        lock.unlock()
    }

    var data: Data {
        lock.lock()
        defer { lock.unlock() }
        return buf
    }
}
