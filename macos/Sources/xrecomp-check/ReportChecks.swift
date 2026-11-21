import Foundation
import XRecompCore

/// « Send report » (CrashReport, XBA-342): the DSN, the scrubbing (the same vectors as the Android app's
/// CrashReportTest), the log's trim, the envelope parsed back, how a run ended, the queue of unsent reports. Never the
/// real transport: a fake records what would be posted.

let reportHost = CrashReport.Host(device: "Mac14,2", os: "macOS 15.1", arch: "arm64", version: "0.14.0",
                                  environment: "release")

/// The items of a Sentry envelope: the header, then (item header, payload) pairs, by their lengths.
func envelopeItems(_ data: Data) -> (header: [String: Any], items: [([String: Any], Data)])? {
    let bytes = [UInt8](data)
    var at = 0
    func line() -> Data? {
        guard at < bytes.count, let nl = bytes[at...].firstIndex(of: 0x0A) else { return nil }
        defer { at = nl + 1 }
        return Data(bytes[at..<nl])
    }
    func obj(_ d: Data?) -> [String: Any]? { d.flatMap { try? JSONSerialization.jsonObject(with: $0) as? [String: Any] } }
    guard let header = obj(line()) else { return nil }
    var items: [([String: Any], Data)] = []
    while at < bytes.count {
        guard let h = obj(line()), let n = h["length"] as? Int, at + n <= bytes.count else { return nil }
        items.append((h, Data(bytes[at..<(at + n)])))
        at += n
        guard at < bytes.count, bytes[at] == 0x0A else { return nil }
        at += 1
    }
    return (header, items)
}

/// A transport that answers `codes` in turn (the last one after that) and records each post.
final class FakeTransport: @unchecked Sendable {
    var codes: [Int]
    var posts: [(URL, [String: String], Data)] = []
    init(_ codes: [Int]) { self.codes = codes }

    var transport: CrashReport.Transport {
        { [self] url, headers, body in
            posts.append((url, headers, body))
            let c = codes.count > 1 ? codes.removeFirst() : (codes.first ?? 200)
            if c < 0 { throw URLError(.notConnectedToInternet) }
            return c
        }
    }
}

func registerReportChecks() {
    check("report: DSN") {
        let d = CrashReport.Dsn.parse(CrashReport.defaultDsn)
        expectEq(d?.endpoint, "https://rustrak.adserver.fr/api/1/envelope/")
        expectEq(d?.key, "b3f3a378974546b0bd69b26fdfb24fa6")
        expectEq(d?.host, "rustrak.adserver.fr")
        expectEq(CrashReport.Dsn.parse(" http://abc:secret@host.example:9000/sub/path/42/ ")?.endpoint,
                 "http://host.example:9000/sub/path/api/42/envelope/")
        expectNil(CrashReport.Dsn.parse(""))
        expectNil(CrashReport.Dsn.parse(nil))
        expectNil(CrashReport.Dsn.parse("https://rustrak.adserver.fr/1"))
        expectNil(CrashReport.Dsn.parse("ftp://k@h/1"))
        // The environment wins (empty: no reports), then Info.plist, then the default.
        expectEq(CrashReport.dsnString(info: nil, environment: [:]), CrashReport.defaultDsn)
        expectEq(CrashReport.dsnString(info: ["XBAReportDsn": "https://k@i.example/2"], environment: [:]),
                 "https://k@i.example/2")
        expectEq(CrashReport.dsnString(info: ["XBAReportDsn": "https://k@i.example/2"],
                                       environment: ["XBA_REPORT_DSN": "https://k@e.example/3"]),
                 "https://k@e.example/3")
        expectNil(CrashReport.dsn(info: nil, environment: ["XBA_REPORT_DSN": ""]))
    }

    check("report: scrub, the Android patterns") {
        let s = { (x: String) in CrashReport.scrub(x) }
        expectEq(s("open /storage/emulated/0/Games/Xbox/Halo (USA).iso failed"), "open <path>/Halo (USA).iso failed")
        expectEq(s("/sdcard/Alex's stuff/xbox/default.xbe"), "<path>/default.xbe")
        expectEq(s("/mnt/media_rw/1A2B-3C4D/x.bin"), "<path>/x.bin")
        expectEq(s("/data/user/0/fr.adserver.xrecomp.shell/files/work/4D530004/prepare.log"),
                 "<app:fr.adserver.xrecomp.shell>/files/work/4D530004/prepare.log")
        expectEq(s("smb://alex:hunter2@nas.local/share/Halo.iso"), "smb://<host>/share/Halo.iso")
        expectEq(s("GET https://user:pw@192.168.1.20:8080/a?b=1"), "GET https://<host>/a?b=1")
        expectEq(s("added magnet:?xt=urn:btih:ABCDEF&dn=Halo&tr=udp ok"), "added magnet:<link> ok")
        expectEq(s("peer 10.0.0.12:6881"), "peer <ip>:6881")
        expectEq(s("peer fe80:0:0:0:1c2b:3a4d:5e6f:7a8b"), "peer <ip>")
        expectEq(s("by someone.name+x@example.co.uk"), "by <email>")
        expectEq(s("app 0.15.0, macOS 15.1"), "app 0.15.0, macOS 15.1")
        expectEq(CrashReport.scrub(nil), "")
    }

    check("report: scrub, home folders (the same vectors as CrashReportTest.scrubsHomeFolders)") {
        let s = { (x: String) in CrashReport.scrub(x) }
        expectEq(s("# RECOMP_SAVE_DIR=/Users/you/Library/Application Support/xboxrecomp/titles/58410001/save"),
                 "# RECOMP_SAVE_DIR=<app:xboxrecomp>/titles/58410001/save")
        expectEq(s("~/Library/Application Support/xboxrecomp/settings.json"), "<app:xboxrecomp>/settings.json")
        expectEq(s("open /Users/you/Games/Xbox/Halo (USA).iso failed"), "open <path>/Halo (USA).iso failed")
        expectEq(s("/Users/you/src/Xrecomp/tools/prepare/x.py:12: error"), "<path>/x.py:12: error")
        expectEq(s("HOME=/Users/you"), "HOME=<path>")
        expectEq(s("cd /Users/you/ ok"), "cd <path>/ ok")
        expectEq(s("/home/you/roms/disc.iso"), "<path>/disc.iso")
        expectEq(s(#"C:\Users\John Smith\Documents\x.iso"#), "<path>/x.iso")
        expectEq(s("log c:/users/you/AppData/Local/xrecomp/run.log"), "log <path>/run.log")
        expectEq(s(#"cwd C:\Users\you"#), "cwd <path>")
        for x in ["/Users/you/a", "/home/you/a b/c", #"C:\Users\you\x"#] {
            expect(!s(x).contains("you"), "\(x) -> \(s(x))")
        }
        expectEq(s("/usr/lib/libSystem.B.dylib"), "/usr/lib/libSystem.B.dylib")
        // The Mac only: the account's names, whole words of 4 characters or more.
        expectEq(CrashReport.scrub("gamer Someone Else played, someone", redact: ["Someone Else", "abc"]),
                 "gamer <user> played, someone")
        expectEq(CrashReport.scrub("abc abcd", redact: ["abc", "abcd"]), "abc <user>")
    }

    check("report: trim keeps head and tail") {
        let small = Data("short".utf8)
        expectEq(CrashReport.trim(small, max: 100), small)
        expectEq(CrashReport.trim(nil, max: 100), Data())
        var big = Data(repeating: UInt8(ascii: "h"), count: 1000)
        big.append(Data(repeating: UInt8(ascii: "t"), count: 1000))
        let t = CrashReport.trim(big, max: 400)
        expectEq(t.count, 400)
        let text = String(decoding: t, as: UTF8.self)
        expect(text.hasPrefix(String(repeating: "h", count: 100)), "head kept")
        expect(text.hasSuffix("ttttt"), "tail kept")
        expect(text.contains("# ---- 1600 bytes cut ----"), text)
    }

    check("report: how a run ended, its top frame, the console") {
        let e = CrashReport.runEnd("# a\nline\n# ended: signal 11 after 12 s\n")
        expectEq(e?.reason, "signal 11 (SIGSEGV)")
        expectEq(e?.value, "after 12 s")
        expectEq(CrashReport.runEnd("# ended: exit 3 after 0 s")?.reason, "exit 3")
        expectEq(CrashReport.runEnd("# ended: signal 77 after 5 s")?.reason, "signal 77")
        expectNil(CrashReport.runEnd("no end here\n"))
        expectEq(CrashReport.topFrame("x\n#00 pc 000000000012a40c  /data/app/x/lib/arm64/libtitle.so (func_00012a40+12)"),
                 "libtitle.so func_00012a40")
        expectEq(CrashReport.topFrame("0   libtitle.dylib   0x0000000104a2c40c func_00012a40 + 12\n"),
                 "libtitle.dylib func_00012a40")
        expectEq(CrashReport.topFrame("nothing"), "")
        expectEq(CrashReport.consoleOf("4D530004"), "xbox")
        expectEq(CrashReport.consoleOf("gba-AXVE-1A2B3C4D"), "gba")
        expectEq(CrashReport.consoleOf("gc-GALE01-1A2B3C4D"), "gc")
        expectEq(CrashReport.consoleOf(nil), "unknown")
    }

    check("report: a game's envelope parsed back, nothing of the home folder in it") {
        let d = tempDir("report")
        let log = d.appendingPathComponent("run-20261121-101500.log")
        write(log, [
            "# /Users/you/Library/Application Support/xboxrecomp/titles/58410001/work/project/build/nightops",
            "# RECOMP_SAVE_DIR=/Users/you/Library/Application Support/xboxrecomp/titles/58410001/save",
            "[KERNEL] loading /Users/you/Games/Night Ops/default.xbe from 192.168.1.20",
            "[GPUGL] OpenGL 4.1 Metal",
            "# ended: signal 11 after 754 s",
        ].joined(separator: "\n") + "\n")
        let r = CrashReport.game(host: reportHost, key: "58410001", title: "Night Ops", log: log)
        expectEq(r.type, "Game: signal 11 (SIGSEGV)")
        expectEq(r.fingerprint, ["game", "xbox", "58410001", "signal 11 (SIGSEGV)", ""])
        let summary = r.summary().components(separatedBy: "\n")
        expectEq(summary.count, 4)
        expectEq(summary.first, "Game: signal 11 (SIGSEGV): after 754 s")
        expectEq(summary.dropFirst().first, "Night Ops (58410001) xbox")
        expectEq(summary.dropFirst(2).first, "Mac14,2 · macOS 15.1 · arm64 · xrecomp@0.14.0")
        expect(summary.last?.hasPrefix("+ run-20261121-101500.log (") == true, summary.last ?? "")

        let id = "0123456789abcdef0123456789abcdef"
        guard let env = expectNotNil(envelopeItems(r.envelope(eventId: id, now: Date(timeIntervalSince1970: 0)))) else {
            return
        }
        expectEq(env.header["event_id"] as? String, id)
        expectEq(env.header["sent_at"] as? String, "1970-01-01T00:00:00Z")
        expectEq(env.items.count, 2)
        guard env.items.count == 2 else { return }
        expectEq(env.items[0].0["type"] as? String, "event")
        let ev = (try? JSONSerialization.jsonObject(with: env.items[0].1)) as? [String: Any] ?? [:]
        expectEq(ev["event_id"] as? String, id)
        expectEq(ev["release"] as? String, "xrecomp@0.14.0")
        expectEq(ev["environment"] as? String, "release")
        expectEq(ev["level"] as? String, "error")
        expectEq(ev["logger"] as? String, "xrecomp.game")
        let tags = ev["tags"] as? [String: String] ?? [:]
        expectEq(tags, ["kind": "game", "console": "xbox", "game_key": "58410001", "game": "Night Ops",
                        "device": "Mac14,2", "os": "macOS 15.1", "arch": "arm64", "end": "signal 11 (SIGSEGV)"])
        let ex = ((ev["exception"] as? [String: Any])?["values"] as? [[String: String]])?.first
        expectEq(ex?["type"], "Game: signal 11 (SIGSEGV)")
        expectEq(ex?["value"], "after 754 s")
        expectEq(env.items[1].0["type"] as? String, "attachment")
        expectEq(env.items[1].0["filename"] as? String, "run-20261121-101500.log")
        expectEq(env.items[1].0["content_type"] as? String, "text/plain")
        let att = String(decoding: env.items[1].1, as: UTF8.self)
        expect(att.contains("# RECOMP_SAVE_DIR=<app:xboxrecomp>/titles/58410001/save"), att)
        expect(att.contains("loading <path>/default.xbe from <ip>"), att)
        let all = String(decoding: r.envelope(eventId: id, now: Date()), as: UTF8.self)
        expect(!all.contains("/Users/") && !all.contains("you/"), "a home folder left in the envelope")
    }

    check("report: a failed Prepare") {
        let d = tempDir("report")
        let log = d.appendingPathComponent("prepare.log")
        write(log, "cmake failed in /Users/you/Library/Application Support/xboxrecomp/titles/58410004/work\n")
        let r = CrashReport.prepare(host: reportHost, key: "gba-AXVE-1A2B3C4D", title: "Pocket Quest",
                                    code: "COMPILE_FAILED", message: "cmake failed (exit 1)", log: log)
        expectEq(r.kind, "prepare")
        expectEq(r.type, "PrepareFailed")
        expectEq(r.value, "COMPILE_FAILED: cmake failed (exit 1)")
        expectEq(r.tags["prepare_code"], "COMPILE_FAILED")
        expectEq(r.tags["console"], "gba")
        expectEq(r.fingerprint, ["prepare", "gba", "gba-AXVE-1A2B3C4D", "COMPILE_FAILED"])
        expectEq(r.logName, "prepare.log")
        let none = CrashReport.prepare(host: reportHost, key: nil, title: nil, code: nil, message: nil, log: nil)
        expectEq(none.value, "?")
        expectNil(none.log)
        expectEq(envelopeItems(none.envelope(eventId: "x", now: Date()))?.items.count, 1)
    }

    check("report: sending, the retry, the queue of unsent reports") {
        let dir = tempDir("reports")
        let dsn = CrashReport.Dsn.parse(CrashReport.defaultDsn)!
        let r = CrashReport.prepare(host: reportHost, key: "4D530004", title: "T", code: "X", message: "", log: nil)
        // Offline: two tries each, then kept; never more than queueMax.
        let off = FakeTransport([-1])
        let s1 = ReportSender(dsn: dsn, version: "0.14.0", queueDir: dir, transport: off.transport)
        for i in 0..<7 { expect(!s1.send(r, now: Date(timeIntervalSince1970: Double(1000 + i)))) }
        expectEq(off.posts.count, 14)
        expectEq(s1.queued().count, CrashReport.queueMax)
        expect(s1.queued().first?.lastPathComponent.hasPrefix("1002000-") == true, "the oldest dropped first")
        // A refusal (not 2xx) is kept too.
        let refused = FakeTransport([500])
        expect(!ReportSender(dsn: dsn, version: "0.14.0", queueDir: dir, transport: refused.transport).send(r))
        expectEq(refused.posts.count, 2)
        // Online: this one, then the waiting ones, oldest first.
        let on = FakeTransport([200])
        let s2 = ReportSender(dsn: dsn, version: "0.14.0", queueDir: dir, transport: on.transport)
        expect(s2.send(r))
        expectEq(on.posts.count, 1 + CrashReport.queueMax)
        expectEq(s2.queued().count, 0)
        let (url, headers, _) = on.posts[0]
        expectEq(url.absoluteString, "https://rustrak.adserver.fr/api/1/envelope/")
        expectEq(headers["Content-Type"], "application/x-sentry-envelope")
        expectEq(headers["X-Sentry-Auth"],
                 "Sentry sentry_version=7, sentry_key=b3f3a378974546b0bd69b26fdfb24fa6, sentry_client=xrecomp-mac/0.14.0")
        // A first try that fails and a retry that passes: sent, nothing kept.
        let flaky = FakeTransport([503, 200])
        expect(ReportSender(dsn: dsn, version: "0.14.0", queueDir: dir, transport: flaky.transport).send(r))
        expectEq(flaky.posts.count, 2)
        expectEq(s2.queued().count, 0)
    }

    check("report: the last game is the newest run log") {
        let paths = AppPaths(root: tempDir("home"))
        expect(CrashReport.lastRun(paths: paths) == nil, "no run yet")
        let a = paths.title("4D530004").logs.appendingPathComponent("run-20261120-100000.log")
        let b = paths.title("gba-AXVE-1A2B3C4D").logs.appendingPathComponent("run-20261101-100000.log")
        write(a, "# ended: exit 0 after 3 s\n")
        write(b, "# ended: signal 6 after 9 s\n")
        try FileManager.default.setAttributes([.modificationDate: Date(timeIntervalSince1970: 1_000)], ofItemAtPath: a.path)
        try FileManager.default.setAttributes([.modificationDate: Date(timeIntervalSince1970: 2_000)], ofItemAtPath: b.path)
        let last = CrashReport.lastRun(paths: paths)
        expectEq(last?.titleId, "gba-AXVE-1A2B3C4D")
        expectEq(last?.log.lastPathComponent, "run-20261101-100000.log")
    }
}
