import Foundation

/// « Send report » on the Mac (XBA-342), the Android app's CrashReport.java (XBA-335) in Swift: one report about a
/// failed Prepare or the last game's run, sent only when the player presses Send. A Sentry envelope (an event and the
/// log as a text attachment) posted to the build's DSN (`$XBA_REPORT_DSN`, else Info.plist `XBAReportDsn`, else the
/// project's Rustrak server; an empty value: no report buttons). No Sentry SDK. Everything that leaves goes through
/// `scrub` first: no home folders, storage paths, URLs' hosts and logins, magnet links, IP or e-mail addresses, and
/// never any game data (the log is text the launcher and the game wrote). A report that could not be sent waits in
/// `<AppPaths.root>/reports/` (the newest `queueMax`) and leaves with the next one (ReportSender).
///
/// No breadcrumbs: the Android app's come from its own event ring (Breadcrumbs.java); the Mac host has none.
public struct CrashReport: Sendable {
    /// The log's share of a report: its head (how the run started) and its tail (how it ended) are kept.
    public static let logMax = 256 * 1024, logHead = 32 * 1024
    public static let queueMax = 5
    public static let kindPrepare = "prepare", kindGame = "game"
    /// Public by design (it ships in the app, as in the APK: android/app/build.gradle `xba_report_dsn`).
    public static let defaultDsn = "https://b3f3a378974546b0bd69b26fdfb24fa6@rustrak.adserver.fr/1"
    public static let dsnInfoKey = "XBAReportDsn", dsnEnvKey = "XBA_REPORT_DSN"

    public var kind = kindGame
    public var type = "", value = "", message = ""
    public var release = "", environment = "release"
    public var tags: [String: String] = [:]
    public var fingerprint: [String] = []
    public var logName = "log.txt"
    public var log: Data?
    /// Words scrubbed besides the patterns (the account's short and full names), 4 characters or more.
    public var redact: [String] = []

    public init() {}

    // ---- DSN -------------------------------------------------------------------------------------------------------

    /// A Sentry DSN: https://KEY@HOST[/PATH]/PROJECT → the envelope endpoint and the public key.
    public struct Dsn: Equatable, Sendable {
        public let endpoint: String
        public let key: String

        /// The server's name, for the sheet (« rustrak.adserver.fr »).
        public var host: String { URL(string: endpoint)?.host ?? endpoint }

        /// Nil when s is empty or not a DSN.
        public static func parse(_ s: String?) -> Dsn? {
            guard let s = s?.trimmingCharacters(in: .whitespacesAndNewlines), !s.isEmpty else { return nil }
            let re = try! NSRegularExpression(pattern: #"^(https?)://([0-9A-Za-z]+)(?::[^@]*)?@([^/]+)(/.*)?/([0-9]+)/?$"#)
            let ns = s as NSString
            guard let m = re.firstMatch(in: s, range: NSRange(location: 0, length: ns.length)) else { return nil }
            func g(_ i: Int) -> String { m.range(at: i).location == NSNotFound ? "" : ns.substring(with: m.range(at: i)) }
            return Dsn(endpoint: "\(g(1))://\(g(3))\(g(4))/api/\(g(5))/envelope/", key: g(2))
        }
    }

    /// The DSN string: `$XBA_REPORT_DSN` when set (even empty: no reports), else Info.plist `XBAReportDsn`, else the
    /// default.
    public static func dsnString(info: [String: Any]?, environment: [String: String]) -> String {
        if let e = environment[dsnEnvKey] { return e }
        if let i = info?[dsnInfoKey] as? String { return i }
        return defaultDsn
    }

    public static func dsn(info: [String: Any]?, environment: [String: String]) -> Dsn? {
        Dsn.parse(dsnString(info: info, environment: environment))
    }

    // ---- Scrubbing (the same patterns as CrashReport.java, in the same order) ---------------------------------------

    private static func re(_ p: String) -> NSRegularExpression { try! NSRegularExpression(pattern: p) }

    private static let magnet = re(#"magnet:\?\S+"#)
    private static let urlAny = re(#"\b([A-Za-z][A-Za-z0-9+.-]{1,15})://[^/\s"'<>]+"#)
    private static let email = re(#"[A-Za-z0-9._%+-]+@[A-Za-z0-9-]+(\.[A-Za-z0-9-]+)*\.[A-Za-z]{2,}"#)
    private static let ipv4 = re(#"(?<![\d.])(?:\d{1,3}\.){3}\d{1,3}(?![\d.])"#)
    private static let ipv6 = re(#"(?<![0-9A-Fa-f:])(?:[0-9A-Fa-f]{1,4}:){4,7}[0-9A-Fa-f]{1,4}(?![0-9A-Fa-f:])"#)
    /// An Android app's own folders: kept, the user id made generic.
    private static let appDir = re(#"/data/(?:user/\d+|data)/([A-Za-z0-9_.]+)/"#)
    /// Android shared storage, SD cards, USB drives: the folders go (spaces and all), the file name stays.
    private static let storage = re(
        #"(?:/storage|/sdcard|/mnt|/data/media|/data/user/\d+|/data/data)(?:/[^/\n\t"<>:]+(?=/))*/([^/\s"<>:]+)"#)
    /// This launcher's data folder (AppPaths.standard): kept (title IDs, our files), the account made generic.
    private static let macAppDir = re(#"(?:/Users/[^/\s"<>:]+|~)/Library/Application Support/xboxrecomp/"#)
    /// A home folder: only the file name stays; a bare home folder goes whole.
    private static let homeDir = re(
        #"(?:/Users|/home)/(?:[^/\n\t"<>:]+(?=/)|[^/\s"<>:]+)(?:/[^/\n\t"<>:]+(?=/))*(?:/([^/\s"<>:]+))?"#)
    private static let winHomeDir = re(
        #"[A-Za-z]:[\\/](?i:users)[\\/](?:[^\\/\n\t"<>:]+(?=[\\/])|[^\\/\s"<>:]+)"#
            + #"(?:[\\/][^\\/\n\t"<>:]+(?=[\\/]))*(?:[\\/]([^\\/\s"<>:]+))?"#)

    private static func replace(_ re: NSRegularExpression, _ s: String, _ template: String) -> String {
        re.stringByReplacingMatches(in: s, range: NSRange(location: 0, length: (s as NSString).length),
                                    withTemplate: template)
    }

    /// `<path>/file`, or `<path>` when the match names no file (group 1 empty).
    private static func paths(_ re: NSRegularExpression, _ s: String) -> String {
        let ns = s as NSString
        var out = ""
        var at = 0
        for m in re.matches(in: s, range: NSRange(location: 0, length: ns.length)) {
            out += ns.substring(with: NSRange(location: at, length: m.range.location - at))
            let file = m.range(at: 1)
            out += file.location == NSNotFound ? "<path>" : "<path>/" + ns.substring(with: file)
            at = m.range.location + m.range.length
        }
        return out + ns.substring(from: at)
    }

    /// Text safe to send: see the type's comment.
    public static func scrub(_ text: String?, redact: [String] = []) -> String {
        guard var s = text, !s.isEmpty else { return "" }
        s = replace(magnet, s, "magnet:<link>")
        s = replace(urlAny, s, "$1://<host>")
        s = replace(email, s, "<email>")
        s = replace(ipv4, s, "<ip>")
        s = replace(ipv6, s, "<ip>")
        s = replace(appDir, s, "<app:$1>/")
        s = paths(storage, s)
        s = replace(macAppDir, s, "<app:xboxrecomp>/")
        s = paths(homeDir, s)
        s = paths(winHomeDir, s)
        for w in redact where w.count >= 4 {
            let r = try? NSRegularExpression(pattern: "\\b" + NSRegularExpression.escapedPattern(for: w) + "\\b",
                                             options: [.caseInsensitive])
            if let r { s = replace(r, s, "<user>") }
        }
        return s
    }

    /// At most max bytes: the first logHead and the end, with a line saying what was cut.
    public static func trim(_ log: Data?, max: Int) -> Data {
        guard let log else { return Data() }
        if log.count <= max { return log }
        let head = min(logHead, max / 4)
        let mark = Data("\n# ---- \(log.count - max) bytes cut ----\n".utf8)
        let tail = max - head - mark.count
        var b = Data(capacity: max)
        b.append(log.prefix(head))
        b.append(mark)
        b.append(log.suffix(tail))
        return b
    }

    private func s(_ t: String) -> String { CrashReport.scrub(t, redact: redact) }

    // ---- The envelope --------------------------------------------------------------------------------------------

    /// The Sentry envelope: a header, the event and (when there is one) the log as a text attachment.
    public func envelope(eventId: String, now: Date) -> Data {
        let ts = CrashReport.iso(now)
        var ev: [String: Any] = [
            "event_id": eventId, "timestamp": ts, "platform": "native", "level": "error",
            "logger": "xrecomp." + kind, "release": release, "environment": environment,
        ]
        var t: [String: String] = [:]
        for (k, v) in tags where !v.isEmpty { t[k] = s(v) }
        ev["tags"] = t
        if !fingerprint.isEmpty { ev["fingerprint"] = fingerprint.map { s($0) } }
        if !message.isEmpty { ev["message"] = ["formatted": s(message)] }
        ev["exception"] = ["values": [["type": s(type), "value": s(value)]]]
        let event = CrashReport.json(ev)

        var b = Data()
        b.append(CrashReport.json(["event_id": eventId, "sent_at": ts]))
        b.append(Data("\n".utf8))
        b.append(CrashReport.json(["type": "event", "length": event.count]))
        b.append(Data("\n".utf8))
        b.append(event)
        b.append(Data("\n".utf8))
        if let log, !log.isEmpty {
            let a = CrashReport.trim(Data(s(String(decoding: log, as: UTF8.self)).utf8), max: CrashReport.logMax)
            b.append(CrashReport.json(["type": "attachment", "length": a.count, "filename": logName,
                                       "content_type": "text/plain"]))
            b.append(Data("\n".utf8))
            b.append(a)
            b.append(Data("\n".utf8))
        }
        return b
    }

    /// What the player sees before sending, in four lines: the problem, the game, the Mac, the log.
    public func summary() -> String {
        var lines = [s(type) + (value.isEmpty ? "" : ": " + s(value))]
        let game = tags["game"] ?? "", key = tags["game_key"] ?? ""
        let g = (game.isEmpty ? "" : s(game) + " ") + (key.isEmpty ? "" : "(\(s(key))) ") + (tags["console"] ?? "")
        lines.append(g.trimmingCharacters(in: .whitespaces))
        lines.append([tags["device"], tags["os"], tags["arch"], release].compactMap { $0 }.filter { !$0.isEmpty }
            .joined(separator: " · "))
        if let log, !log.isEmpty {
            lines.append("+ \(logName) (\(Swift.max(1, Swift.min(log.count, CrashReport.logMax) / 1024)) KB)")
        }
        return lines.joined(separator: "\n")
    }

    // ---- The Mac -------------------------------------------------------------------------------------------------

    /// The facts every report carries.
    public struct Host: Sendable {
        public var device: String
        public var os: String
        public var arch: String
        public var version: String
        public var environment: String
        public var redact: [String]

        public init(device: String, os: String, arch: String, version: String, environment: String,
                    redact: [String] = []) {
            self.device = device
            self.os = os
            self.arch = arch
            self.version = version
            self.environment = environment
            self.redact = redact
        }

        /// This Mac: its model (sysctl hw.model, « Mac14,2 »), macOS's version, the build's architecture, the app's
        /// version (CFBundleShortVersionString, « dev » under `swift run`), release unless that is not an app bundle.
        public static func current(info: [String: Any]?, bundleURL: URL) -> Host {
            let v = ProcessInfo.processInfo.operatingSystemVersion
            let os = "macOS \(v.majorVersion).\(v.minorVersion)" + (v.patchVersion > 0 ? ".\(v.patchVersion)" : "")
            #if arch(arm64)
            let arch = "arm64"
            #elseif arch(x86_64)
            let arch = "x86_64"
            #else
            let arch = "unknown"
            #endif
            let version = (info?["CFBundleShortVersionString"] as? String) ?? "dev"
            let env = bundleURL.pathExtension == "app" && version != "dev" ? "release" : "debug"
            return Host(device: macModel(), os: os, arch: arch, version: version, environment: env,
                        redact: [NSUserName(), NSFullUserName()])
        }

        public static func macModel() -> String {
            var size = 0
            guard sysctlbyname("hw.model", nil, &size, nil, 0) == 0, size > 0 else { return "Mac" }
            var buf = [CChar](repeating: 0, count: size)
            guard sysctlbyname("hw.model", &buf, &size, nil, 0) == 0 else { return "Mac" }
            let s = String(cString: buf)
            return s.isEmpty ? "Mac" : s
        }
    }

    // ---- The two kinds -------------------------------------------------------------------------------------------

    /// A failed Prepare: its code and message, the title, and prepare.log (or launcher-prepare.log).
    public static func prepare(host: Host, key: String?, title: String?, code: String?, message: String?,
                               log: URL?) -> CrashReport {
        var r = base(host, kindPrepare)
        let c = code ?? "", m = message ?? ""
        r.type = "PrepareFailed"
        r.value = (c.isEmpty ? "?" : c) + (m.isEmpty ? "" : ": " + m)
        r.setGame(key, title)
        r.tags["prepare_code"] = c
        r.fingerprint = ["prepare", r.tags["console"] ?? "", key ?? "", c]
        r.logName = log?.lastPathComponent ?? "prepare.log"
        r.log = read(log)
        return r
    }

    /// The last run of a game, from its run log (GameRunner: `logs/run-<timestamp>.log`, ending with
    /// « # ended: signal 11 after 12 s »). key: the title ID or console game ID.
    public static func game(host: Host, key: String?, title: String?, log: URL?) -> CrashReport {
        var r = base(host, kindGame)
        r.log = read(log)
        let text = String(decoding: r.log ?? Data(), as: UTF8.self)
        let end = runEnd(text)
        let reason = end?.reason ?? "unknown end"
        r.type = "Game: " + reason
        r.value = end?.value ?? ""
        r.setGame(key, title)
        r.tags["end"] = reason
        let frame = topFrame(text)
        r.message = frame.isEmpty ? "" : "top frame: " + frame
        // No duration in the fingerprint: the same crash after 12 s or 40 s is one issue.
        r.fingerprint = ["game", r.tags["console"] ?? "", key ?? "", reason, frame]
        r.logName = log?.lastPathComponent ?? "run.log"
        return r
    }

    /// How a run ended, from GameRunner's last « # ended: … after N s » line: (« signal 11 (SIGSEGV) », « after
    /// 12 s »); nil when the log has none (the launcher quit first, or the log was cut).
    public static func runEnd(_ text: String) -> (reason: String, value: String)? {
        let re = try! NSRegularExpression(pattern: #"(?m)^# ended: (exit -?\d+|signal (\d+)) after (\d+) s\s*$"#)
        let ns = text as NSString
        guard let m = re.matches(in: text, range: NSRange(location: 0, length: ns.length)).last else { return nil }
        var reason = ns.substring(with: m.range(at: 1))
        if m.range(at: 2).location != NSNotFound, let n = Int(ns.substring(with: m.range(at: 2))),
           let name = signalNames[n] {
            reason += " (\(name))"
        }
        return (reason, "after \(ns.substring(with: m.range(at: 3))) s")
    }

    static let signalNames: [Int: String] = [
        1: "SIGHUP", 2: "SIGINT", 3: "SIGQUIT", 4: "SIGILL", 5: "SIGTRAP", 6: "SIGABRT", 8: "SIGFPE", 9: "SIGKILL",
        10: "SIGBUS", 11: "SIGSEGV", 12: "SIGSYS", 13: "SIGPIPE", 15: "SIGTERM",
    ]

    /// The first frame of a backtrace in the log, its addresses dropped: Android's « #00 pc … lib (symbol+off) »
    /// or macOS backtrace_symbols' « 0   lib   0x… symbol + off ».
    public static func topFrame(_ text: String) -> String {
        let ns = text as NSString
        let all = NSRange(location: 0, length: ns.length)
        func g(_ m: NSTextCheckingResult, _ i: Int) -> String? {
            m.range(at: i).location == NSNotFound ? nil : ns.substring(with: m.range(at: i))
        }
        let android = try! NSRegularExpression(pattern: #"#00 pc [0-9a-fA-F]+\s+(\S+)(?:\s+\(([^)+]+))?"#)
        if let m = android.firstMatch(in: text, range: all), let lib = g(m, 1) {
            let l = (lib as NSString).lastPathComponent
            return g(m, 2).map { l + " " + $0 } ?? l
        }
        let mac = try! NSRegularExpression(pattern: #"(?m)^0\s+(\S+)\s+0x[0-9a-fA-F]+\s+([^\s+]+)"#)
        if let m = mac.firstMatch(in: text, range: all), let lib = g(m, 1) {
            return g(m, 2).map { lib + " " + $0 } ?? lib
        }
        return ""
    }

    /// The console of a game key: its prefix (« n64-… », « gc-… »); an Xbox title ID has none.
    public static func consoleOf(_ key: String?) -> String {
        guard let key, !key.isEmpty else { return "unknown" }
        let re = try! NSRegularExpression(pattern: "^([a-z][a-z0-9]{1,5})-")
        let ns = key as NSString
        guard let m = re.firstMatch(in: key, range: NSRange(location: 0, length: ns.length)) else { return "xbox" }
        return ns.substring(with: m.range(at: 1))
    }

    /// The newest run log of any title: (its title ID as AppPaths.title names it, the log); nil when no game ran yet.
    public static func lastRun(paths: AppPaths) -> (titleId: String, log: URL)? {
        let fm = FileManager.default
        let ids = (try? fm.contentsOfDirectory(atPath: paths.titles.path)) ?? []
        var best: (String, URL, Date)?
        for id in ids where !id.hasPrefix(".") {
            guard let log = RunLog(dir: paths.title(id).logs).latest else { continue }
            let when = ((try? fm.attributesOfItem(atPath: log.path))?[.modificationDate] as? Date) ?? .distantPast
            if best == nil || when > best!.2 { best = (paths.title(id).titleId, log, when) }
        }
        return best.map { ($0.0, $0.1) }
    }

    private static func base(_ host: Host, _ kind: String) -> CrashReport {
        var r = CrashReport()
        r.kind = kind
        r.release = "xrecomp@" + host.version
        r.environment = host.environment
        r.redact = host.redact
        r.tags["kind"] = kind
        r.tags["device"] = host.device
        r.tags["os"] = host.os
        r.tags["arch"] = host.arch
        return r
    }

    private mutating func setGame(_ key: String?, _ title: String?) {
        tags["console"] = CrashReport.consoleOf(key)
        tags["game_key"] = key ?? ""
        tags["game"] = title ?? ""
    }

    /// The file's last 8 × logMax bytes (a run log can grow large).
    private static func read(_ url: URL?) -> Data? {
        guard let url, let h = FileHandle(forReadingAtPath: url.path) else { return nil }
        defer { try? h.close() }
        let size = (try? h.seekToEnd()) ?? 0
        let cap = UInt64(8 * logMax)
        try? h.seek(toOffset: size > cap ? size - cap : 0)
        return try? h.readToEnd()
    }

    // ---- JSON ----------------------------------------------------------------------------------------------------

    static func json(_ o: Any) -> Data {
        (try? JSONSerialization.data(withJSONObject: o, options: [.sortedKeys, .withoutEscapingSlashes])) ?? Data("{}".utf8)
    }

    public static func iso(_ d: Date) -> String {
        let f = DateFormatter()
        f.locale = Locale(identifier: "en_US_POSIX")
        f.timeZone = TimeZone(identifier: "UTC")
        f.dateFormat = "yyyy-MM-dd'T'HH:mm:ss'Z'"
        return f.string(from: d)
    }

    // ---- Sending -------------------------------------------------------------------------------------------------

    /// The HTTP post, injected so the checks use a fake: the status code, or a thrown error.
    public typealias Transport = @Sendable (_ url: URL, _ headers: [String: String], _ body: Data) throws -> Int

    /// URLSession, synchronous (call it off the main thread), 15 s.
    public static let http: Transport = { url, headers, body in
        var req = URLRequest(url: url, timeoutInterval: 15)
        req.httpMethod = "POST"
        for (k, v) in headers { req.setValue(v, forHTTPHeaderField: k) }
        req.httpBody = body
        let box = HTTPResult()
        let done = DispatchSemaphore(value: 0)
        URLSession.shared.dataTask(with: req) { _, resp, err in
            box.code = (resp as? HTTPURLResponse)?.statusCode ?? 0
            box.error = err
            done.signal()
        }.resume()
        done.wait()
        if let e = box.error { throw e }
        return box.code
    }
}

/// Sends reports and keeps the ones that could not leave (`queueDir`, the newest CrashReport.queueMax).
public struct ReportSender: Sendable {
    public let dsn: CrashReport.Dsn
    public let version: String
    public let queueDir: URL
    public var transport: CrashReport.Transport

    public init(dsn: CrashReport.Dsn, version: String, queueDir: URL, transport: @escaping CrashReport.Transport) {
        self.dsn = dsn
        self.version = version
        self.queueDir = queueDir
        self.transport = transport
    }

    /// Sends the report (off the main thread; one retry), then the waiting ones. True when this one was accepted;
    /// when not, it waits in queueDir.
    @discardableResult
    public func send(_ r: CrashReport, now: Date = Date()) -> Bool {
        let id = UUID().uuidString.replacingOccurrences(of: "-", with: "").lowercased()
        let body = r.envelope(eventId: id, now: now)
        let ok = post(body) || post(body)
        if !ok {
            keep(id, body, now)
            return false
        }
        flush()
        return true
    }

    func post(_ body: Data) -> Bool {
        guard let url = URL(string: dsn.endpoint) else { return false }
        let h = ["Content-Type": "application/x-sentry-envelope",
                 "X-Sentry-Auth": "Sentry sentry_version=7, sentry_key=\(dsn.key), sentry_client=xrecomp-mac/\(version)"]
        guard let code = try? transport(url, h, body) else { return false }
        return (200..<300).contains(code)
    }

    private func keep(_ id: String, _ body: Data, _ now: Date) {
        guard (try? FileManager.default.ensureDirectory(queueDir)) != nil else { return }
        let name = "\(Int64(now.timeIntervalSince1970 * 1000))-\(id).envelope"
        guard (try? body.write(to: queueDir.appendingPathComponent(name))) != nil else { return }
        let all = queued()
        for u in all.prefix(max(0, all.count - CrashReport.queueMax)) { try? FileManager.default.removeItem(at: u) }
    }

    /// The waiting reports, oldest first.
    public func queued() -> [URL] {
        let names = (try? FileManager.default.contentsOfDirectory(atPath: queueDir.path)) ?? []
        return names.filter { $0.hasSuffix(".envelope") }.sorted().map { queueDir.appendingPathComponent($0) }
    }

    private func flush() {
        for u in queued() {
            guard let body = try? Data(contentsOf: u) else {
                try? FileManager.default.removeItem(at: u)
                continue
            }
            if !post(body) { return }   // still offline: the rest wait
            try? FileManager.default.removeItem(at: u)
        }
    }
}

/// The URLSession callback's answer, read once the semaphore is signalled.
private final class HTTPResult: @unchecked Sendable {
    var code = 0
    var error: Error?
}
