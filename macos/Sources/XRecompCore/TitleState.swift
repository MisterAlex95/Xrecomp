import Foundation

/// What `python -m tools.prepare SOURCE --work DIR --check --json` says about a title, without doing any work:
///
///     {"state": "absent"|"stale"|"ready", "reasons": [...], "exe": "...", "title": "...", "title_id": "...",
///      "env_force": {"RECOMP_X": "0"}, "work": "..."}
///
/// On failure it exits 1 with {"state": "error", "code": "...", "msg": "..."}. Stale reasons: toolkit, profile,
/// discovered, xbe, not_built, no_stamp; exe stays filled when stale (the old binary is playable).
/// A stale title can still be played (its old executable is there); the reasons say why it should be prepared again.
public struct TitleState: Codable, Equatable, Sendable {
    public enum State: String, Codable, Sendable {
        case absent, stale, ready
        /// The check itself failed (exit 1): code and msg say why (NO_XBE, BAD_IMAGE, BAD_XBE, BAD_PROFILE).
        case error
    }

    public var state: State
    public var reasons: [String]
    public var exe: String?
    public var title: String?
    public var titleId: String?
    /// Options the title's profile compiles in (they win over recomp.cfg): « set by the title » in Settings.
    public var envForce: [String: String]
    /// The work folder looked at.
    public var work: String?
    /// With state error.
    public var code: String?
    public var msg: String?

    enum CodingKeys: String, CodingKey {
        case state, reasons, exe, title, titleId = "title_id", envForce = "env_force", work, code, msg
    }

    public init(state: State, reasons: [String] = [], exe: String? = nil, title: String? = nil,
                titleId: String? = nil, envForce: [String: String] = [:]) {
        self.state = state
        self.reasons = reasons
        self.exe = exe
        self.title = title
        self.titleId = titleId
        self.envForce = envForce
    }

    public init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        let raw = try c.decode(String.self, forKey: .state)
        state = State(rawValue: raw) ?? .error
        reasons = (try? c.decodeIfPresent([String].self, forKey: .reasons)) ?? []
        exe = try? c.decodeIfPresent(String.self, forKey: .exe)
        if exe?.isEmpty == true { exe = nil }
        title = try? c.decodeIfPresent(String.self, forKey: .title)
        titleId = try? c.decodeIfPresent(String.self, forKey: .titleId)
        envForce = (try? c.decodeIfPresent([String: String].self, forKey: .envForce)) ?? [:]
        work = try? c.decodeIfPresent(String.self, forKey: .work)
        code = try? c.decodeIfPresent(String.self, forKey: .code)
        msg = try? c.decodeIfPresent(String.self, forKey: .msg)
    }

    /// The JSON object in `output` (the last line starting with '{', after any log lines), decoded; nil if none.
    public static func decode(_ output: String) -> TitleState? {
        let text = output.trimmingCharacters(in: .whitespacesAndNewlines)
        if let d = text.data(using: .utf8), let s = try? JSONDecoder().decode(TitleState.self, from: d) { return s }
        if let start = text.range(of: "\n{", options: .backwards) {
            let tail = text[text.index(after: start.lowerBound)...]
            return try? JSONDecoder().decode(TitleState.self, from: Data(tail.utf8))
        }
        return nil
    }

    /// The executable is there to play, even when stale.
    public var playable: Bool {
        guard let exe else { return false }
        return (state == .ready || state == .stale) && FileManager.default.isExecutableFile(atPath: exe)
    }

    /// One line for the status: why prepare again.
    public var reasonText: String {
        TitleState.describe(reasons)
    }

    public static func describe(_ reasons: [String]) -> String {
        let r = reasons.map { $0.lowercased() }
        if r.contains(where: { $0.contains("discover") }) { return "Improvements found while playing: prepare again" }
        if r.contains(where: { $0.contains("toolkit") || $0.contains("commit") }) {
            return "Prepare again (toolkit updated)"
        }
        if r.contains(where: { $0.contains("profile") }) { return "Prepare again (title profile changed)" }
        if r.contains(where: { $0.contains("xbe") }) { return "Prepare again (the game's files changed)" }
        if r.contains("not_built") { return "The last build did not finish: prepare again" }
        if r.contains("no_stamp") { return "Prepare again (made outside the launcher)" }
        return reasons.isEmpty ? "Prepare again" : "Prepare again (\(reasons.joined(separator: ", ")))"
    }

    /// Runs the check (blocking, a few seconds at most): nil when it could not run or said nothing readable.
    public static func check(python: String, toolkit: URL, source: URL, work: URL,
                             environment: [String: String] = ChildEnvironment.base()) -> TitleState? {
        let r = Shell.run(python, ["-m", "tools.prepare", source.path, "--work", work.path, "--check", "--json"],
                          cwd: toolkit, environment: environment, timeout: 60)
        return decode(r.output)
    }

    /// The executable's name from `project(NAME ...)` in `<work>/project/CMakeLists.txt` (tools/compat's
    /// project_exe), when it exists in the build folder.
    public static func projectExe(work: URL) -> URL? {
        let cm = work.appendingPathComponent("project/CMakeLists.txt")
        guard let text = try? String(contentsOf: cm, encoding: .utf8),
              let r = text.range(of: #"(?m)^project\(([^\s)]+)"#, options: .regularExpression) else { return nil }
        let name = text[r].dropFirst("project(".count)
        let exe = work.appendingPathComponent("project/build").appendingPathComponent(String(name))
        return FileManager.default.isExecutableFile(atPath: exe.path) ? exe : nil
    }
}
