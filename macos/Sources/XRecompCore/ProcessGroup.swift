import Foundation

/// How a child ended.
public enum ChildExit: Equatable, Sendable {
    case exited(Int32)
    case signaled(Int32)

    public var succeeded: Bool { self == .exited(0) }

    public var description: String {
        switch self {
        case .exited(let c): return "exit \(c)"
        case .signaled(let s): return "signal \(s)"
        }
    }

    /// From a waitpid status word (WIFEXITED / WTERMSIG by hand: the macros are not imported into Swift).
    public init(waitStatus st: Int32) {
        let sig = st & 0x7f
        if sig == 0 {
            self = .exited((st >> 8) & 0xff)
        } else {
            self = .signaled(sig)
        }
    }
}

/// A child process started with posix_spawn, optionally in a process group of its own (prepare: a cancel must stop
/// python, cmake, ninja and every compiler under it). stdout and stderr are merged and handed out line by line on a
/// background thread; stdin is /dev/null. Signals go to the PID, or to the group with `group: true`; never by name.
public final class ChildProcess: @unchecked Sendable {
    public let pid: pid_t
    public let ownGroup: Bool
    private let lock = NSLock()
    private var exitValue: ChildExit?
    private let exited = DispatchSemaphore(value: 0)
    private let drained = DispatchSemaphore(value: 0)

    public enum SpawnError: Error, CustomStringConvertible {
        case spawn(Int32, String)
        public var description: String {
            switch self {
            case .spawn(let e, let exe): return "cannot start \(exe): \(String(cString: strerror(e)))"
            }
        }
    }

    /// Starts `executable args` in cwd with exactly `environment`. `onLine` gets each output line (without the
    /// newline), `onExit` the exit once the output is drained (or 1 s after the exit if a grandchild keeps the
    /// pipe open). Both are called on background threads.
    public init(executable: String, arguments: [String], cwd: URL?, environment: [String: String],
                newProcessGroup: Bool, onLine: @escaping @Sendable (String) -> Void,
                onExit: @escaping @Sendable (ChildExit) -> Void) throws {
        var fds: [Int32] = [0, 0]
        guard pipe(&fds) == 0 else { throw SpawnError.spawn(errno, executable) }
        let (readFd, writeFd) = (fds[0], fds[1])

        var actions: posix_spawn_file_actions_t?
        posix_spawn_file_actions_init(&actions)
        defer { posix_spawn_file_actions_destroy(&actions) }
        posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0)
        posix_spawn_file_actions_adddup2(&actions, writeFd, 1)
        posix_spawn_file_actions_adddup2(&actions, writeFd, 2)
        if let cwd { posix_spawn_file_actions_addchdir_np(&actions, cwd.path) }

        var attr: posix_spawnattr_t?
        posix_spawnattr_init(&attr)
        defer { posix_spawnattr_destroy(&attr) }
        // Every other descriptor of the launcher stays out of the child; default signal handling and mask.
        var flags = Int32(POSIX_SPAWN_CLOEXEC_DEFAULT | POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSIGMASK)
        if newProcessGroup {
            flags |= Int32(POSIX_SPAWN_SETPGROUP)
            posix_spawnattr_setpgroup(&attr, 0)
        }
        posix_spawnattr_setflags(&attr, Int16(flags))
        var all = sigset_t()
        sigfillset(&all)
        posix_spawnattr_setsigdefault(&attr, &all)
        var none = sigset_t()
        sigemptyset(&none)
        posix_spawnattr_setsigmask(&attr, &none)

        let argv: [UnsafeMutablePointer<CChar>?] = ([executable] + arguments).map { strdup($0) } + [nil]
        let envp: [UnsafeMutablePointer<CChar>?] = environment.map { strdup("\($0.key)=\($0.value)") } + [nil]
        defer {
            argv.forEach { free($0) }
            envp.forEach { free($0) }
        }
        var child: pid_t = 0
        let rc = posix_spawn(&child, executable, &actions, &attr, argv, envp)
        close(writeFd)
        guard rc == 0 else {
            close(readFd)
            throw SpawnError.spawn(rc, executable)
        }
        pid = child
        ownGroup = newProcessGroup

        let drainedSem = drained
        Thread.detachNewThread {
            var pending = Data()
            var buf = [UInt8](repeating: 0, count: 16384)
            while true {
                let n = read(readFd, &buf, buf.count)
                if n < 0 && errno == EINTR { continue }
                if n <= 0 { break }
                pending.append(buf, count: n)
                while let nl = pending.firstIndex(of: 0x0A) {
                    let line = pending[pending.startIndex..<nl]
                    pending.removeSubrange(pending.startIndex...nl)
                    onLine(String(decoding: line, as: UTF8.self))
                }
                if pending.count > 1 << 20 {   // a runaway line without a newline
                    onLine(String(decoding: pending, as: UTF8.self))
                    pending.removeAll()
                }
            }
            if !pending.isEmpty { onLine(String(decoding: pending, as: UTF8.self)) }
            close(readFd)
            drainedSem.signal()
        }
        Thread.detachNewThread { [self] in
            var st: Int32 = 0
            while waitpid(child, &st, 0) < 0 && errno == EINTR {}
            let e = ChildExit(waitStatus: st)
            _ = drainedSem.wait(timeout: .now() + 1)
            lock.lock()
            exitValue = e
            lock.unlock()
            exited.signal()
            onExit(e)
        }
    }

    /// The exit, once the child is gone; nil while it runs.
    public var exit: ChildExit? {
        lock.lock()
        defer { lock.unlock() }
        return exitValue
    }

    public var isRunning: Bool { exit == nil }

    /// Sends sig to the child, or to its whole group when it has one and `group` is true.
    public func signal(_ sig: Int32, group: Bool) {
        guard isRunning || group else { return }
        if group && ownGroup {
            kill(-pid, sig)
        } else {
            kill(pid, sig)
        }
    }

    /// SIGTERM, then SIGKILL after `grace` seconds if it is still there (to the group with `group`). Returns at
    /// once; the exit arrives through onExit.
    public func terminate(grace: TimeInterval = 5, group: Bool) {
        signal(SIGTERM, group: group)
        DispatchQueue.global().asyncAfter(deadline: .now() + grace) { [self] in
            if group && ownGroup {
                // The leader may be gone while a compiler of its group lingers: the group gets the KILL anyway.
                kill(-pid, SIGKILL)
            } else if isRunning {
                kill(pid, SIGKILL)
            }
        }
    }

    /// Blocks until the child exits or timeout passes; the exit or nil.
    @discardableResult
    public func wait(timeout: TimeInterval) -> ChildExit? {
        if let e = exit { return e }
        if exited.wait(timeout: .now() + timeout) == .success { exited.signal() }
        return exit
    }
}
