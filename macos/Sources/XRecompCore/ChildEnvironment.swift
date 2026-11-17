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
    ///
    /// The output is read on a thread of its own until EOF. After the exit (or the kill on timeout) the call waits
    /// at most `drainLimit` seconds for that EOF: a grandchild that keeps the pipe open (a compiler under
    /// `cmake --build`, a build step of `pip install`) must not hold the call far past its timeout.
    @discardableResult
    public static func run(_ exe: String, _ args: [String], cwd: URL? = nil, environment: [String: String]? = nil,
                           timeout: TimeInterval = 60, drainLimit: TimeInterval = 2) -> Result {
        let p = Process()
        p.executableURL = URL(fileURLWithPath: exe)
        p.arguments = args
        if let cwd { p.currentDirectoryURL = cwd }
        p.environment = environment ?? ChildEnvironment.base()
        let pipe = Pipe()
        p.standardOutput = pipe
        p.standardError = pipe
        p.standardInput = FileHandle.nullDevice
        let done = DispatchSemaphore(value: 0)
        p.terminationHandler = { _ in done.signal() }
        do { try p.run() } catch {
            return Result(status: -1, output: "\(error.localizedDescription)")
        }
        // The reader owns a duplicate of the read end, so the Pipe closing its own when it goes away cannot pull
        // the descriptor from under a read that outlives this call.
        let collected = LockedData()
        let drained = DispatchSemaphore(value: 0)
        let fd = dup(pipe.fileHandleForReading.fileDescriptor)
        if fd >= 0 {
            Thread.detachNewThread {
                var buf = [UInt8](repeating: 0, count: 16384)
                while true {
                    let n = read(fd, &buf, buf.count)
                    if n < 0 && errno == EINTR { continue }
                    if n <= 0 { break }
                    collected.append(Data(buf[0..<n]))
                }
                close(fd)
                drained.signal()
            }
        } else {
            drained.signal()
        }
        var status: Int32
        if done.wait(timeout: .now() + timeout) == .timedOut {
            p.terminate()
            if done.wait(timeout: .now() + 2) == .timedOut { kill(p.processIdentifier, SIGKILL); done.wait() }
            status = -2
        } else {
            status = p.terminationStatus
        }
        _ = drained.wait(timeout: .now() + drainLimit)
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
