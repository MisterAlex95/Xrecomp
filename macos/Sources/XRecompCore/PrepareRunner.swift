import Foundation

/// One prepare: `python -u -m tools.prepare SOURCE --work DIR --progress`, cwd at the toolkit, in a process group of
/// its own. Protocol lines drive the events; every line also goes to the log callback and to
/// `<work>/launcher-prepare.log` (prepare writes the tools' own output to `<work>/prepare.log`).
public final class PrepareRunner: @unchecked Sendable {
    public enum Event: Sendable {
        case progress(PrepareProtocol.Progress)
        case line(String)
        case finished(Outcome)
    }

    public enum Outcome: Equatable, Sendable {
        /// exe: the built executable (nil with --no-build, or when the result line had none).
        case ok(exe: String?)
        case failed(code: String, message: String)
        case cancelled
    }

    public struct Request: Sendable {
        public var python: String
        public var toolkit: URL
        public var source: URL
        public var work: URL
        public var force = false
        public var noBuild = false
        /// --no-title-layer: the title's optional enhancements left out (profile-free WP6).
        public var noTitleLayer = false
        /// Extra arguments after the fixed ones (the checks use it).
        public var extra: [String] = []
        public var environment: [String: String] = ChildEnvironment.base()

        public init(python: String, toolkit: URL, source: URL, work: URL) {
            self.python = python
            self.toolkit = toolkit
            self.source = source
            self.work = work
        }

        public var arguments: [String] {
            var a = ["-u", "-m", "tools.prepare", source.path, "--work", work.path, "--progress"]
            if force { a.append("--force") }
            if noBuild { a.append("--no-build") }
            if noTitleLayer { a.append("--no-title-layer") }
            return a + extra
        }
    }

    public static let logName = "launcher-prepare.log"
    public let request: Request
    /// SIGTERM then SIGKILL after this many seconds.
    public var cancelGrace: TimeInterval = 5
    private var child: ChildProcess?
    private let lock = NSLock()
    private var result: PrepareProtocol.Result?
    private var cancelRequested = false
    private var lastLines: [String] = []
    private var logHandle: FileHandle?
    private let handler: @Sendable (Event) -> Void

    public init(_ request: Request, handler: @escaping @Sendable (Event) -> Void) {
        self.request = request
        self.handler = handler
    }

    public func start() throws {
        let fm = FileManager.default
        try fm.ensureDirectory(request.work)
        let logURL = request.work.appendingPathComponent(PrepareRunner.logName)
        fm.createFile(atPath: logURL.path, contents: nil)
        logHandle = try? FileHandle(forWritingTo: logURL)
        log("$ \(request.python) \(request.arguments.joined(separator: " "))")
        child = try ChildProcess(executable: request.python, arguments: request.arguments, cwd: request.toolkit,
                                 environment: request.environment, newProcessGroup: true,
                                 onLine: { [weak self] in self?.line($0) },
                                 onExit: { [weak self] in self?.exited($0) })
    }

    public var pid: pid_t? { child?.pid }
    public var isRunning: Bool { child?.isRunning ?? false }

    /// SIGTERM to the whole group, SIGKILL after cancelGrace. Stamps and the incremental build make the next
    /// prepare resume.
    public func cancel() {
        lock.lock()
        cancelRequested = true
        lock.unlock()
        child?.terminate(grace: cancelGrace, group: true)
    }

    @discardableResult
    public func wait(timeout: TimeInterval) -> ChildExit? { child?.wait(timeout: timeout) }

    private func log(_ s: String) {
        lock.lock()
        defer { lock.unlock() }
        if let h = logHandle, !appendLogLine(s, to: h) { logHandle = nil }
    }

    private func line(_ s: String) {
        log(s)
        if let p = PrepareProtocol.parseProgress(s) {
            handler(.progress(p))
            return
        }
        if let r = PrepareProtocol.parseResult(s) {
            lock.lock()
            result = r
            lock.unlock()
            return
        }
        lock.lock()
        lastLines.append(s)
        if lastLines.count > 40 { lastLines.removeFirst(lastLines.count - 40) }
        lock.unlock()
        handler(.line(s))
    }

    private func exited(_ e: ChildExit) {
        lock.lock()
        let r = result, cancelled = cancelRequested
        let tail = lastLines.last(where: { !$0.trimmingCharacters(in: .whitespaces).isEmpty }) ?? ""
        lock.unlock()
        let outcome = PrepareRunner.outcome(result: r, exit: e, cancelled: cancelled, lastLine: tail)
        log("# prepare ended: \(e.description)")
        lock.lock()
        try? logHandle?.close()
        logHandle = nil
        lock.unlock()
        handler(.finished(outcome))
    }

    /// The outcome from the result line, else from the exit: a cancel (or exit 130) is cancelled; exit 0 is ok;
    /// anything else failed, with the last output line as the message.
    public static func outcome(result: PrepareProtocol.Result?, exit: ChildExit, cancelled: Bool,
                               lastLine: String) -> Outcome {
        if let r = result {
            if r.ok { return .ok(exe: r.exe.isEmpty ? nil : r.exe) }
            if r.code == "CANCELLED" { return .cancelled }
            return .failed(code: r.code, message: r.msg)
        }
        if cancelled || exit == .exited(130) { return .cancelled }
        if exit == .exited(0) { return .ok(exe: nil) }
        let code: String
        switch exit {
        case .signaled(let s): code = "SIGNAL_\(s)"
        case .exited(let c): code = "EXIT_\(c)"
        }
        return .failed(code: code, message: lastLine)
    }
}
