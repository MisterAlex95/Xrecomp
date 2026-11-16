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

    /// A prepare's (or --check's) environment: XBA_NO_TITLE_LAYER=1 when the title's optional enhancements are off,
    /// and never an inherited one otherwise (profile-free WP6).
    public static func prepare(_ env: [String: String] = base(), enhancements: Bool) -> [String: String] {
        var e = env
        e["XBA_NO_TITLE_LAYER"] = enhancements ? nil : "1"
        return e
    }

    /// A game's environment: the fixed PATH, then the global defaults, then the title's recomp.cfg lines (the first
    /// line for a key wins, as the runtime reads the file), then RECOMP_SAVE_DIR. A recomp.cfg value replaces an
    /// inherited variable of the same name; PATH and RECOMP_SAVE_DIR cannot be set from the file. An empty value
    /// (`KEY=`: a title's « runtime default » over Default settings, XBA-188) unsets the variable.
    ///
    /// With frame generation on (RECOMP_LSFG=1) and no DYLD_FALLBACK_LIBRARY_PATH of its own, the game gets one with
    /// Homebrew's lib folders: the bridge dlopens libvulkan / MoltenVK by name, and dyld's default fallback
    /// (~/lib:/usr/local/lib:/usr/lib) misses /opt/homebrew/lib on Apple silicon.
    public static func game(inherited: [String: String], globalDefaults: [String: String] = [:],
                            config: [(key: String, value: String)], saveDir: URL?) -> [String: String] {
        var e = base(inherited)
        func put(_ k: String, _ v: String) {
            guard k != "PATH" && k != "RECOMP_SAVE_DIR" else { return }
            if v.isEmpty { e.removeValue(forKey: k) } else { e[k] = v }
        }
        for (k, v) in globalDefaults { put(k, v) }
        for (k, v) in config { put(k, v) }
        if let saveDir { e["RECOMP_SAVE_DIR"] = saveDir.path }
        if e["RECOMP_LSFG"] == "1" && (e["DYLD_FALLBACK_LIBRARY_PATH"] ?? "").isEmpty {
            e["DYLD_FALLBACK_LIBRARY_PATH"] = VulkanLoader.fallbackPath(home: e["HOME"])
        }
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
