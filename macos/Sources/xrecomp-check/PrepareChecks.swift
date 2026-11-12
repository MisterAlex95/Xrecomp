import Foundation
import XRecompCore

/// Collects a runner's events across threads.
final class EventSink: @unchecked Sendable {
    private let lock = NSLock()
    private var _progress: [PrepareProtocol.Progress] = []
    private var _lines: [String] = []
    private var _outcome: PrepareRunner.Outcome?

    func take(_ e: PrepareRunner.Event) {
        lock.lock()
        defer { lock.unlock() }
        switch e {
        case .progress(let p): _progress.append(p)
        case .line(let l): _lines.append(l)
        case .finished(let o): _outcome = o
        }
    }

    var progress: [PrepareProtocol.Progress] { lock.lock(); defer { lock.unlock() }; return _progress }
    var lines: [String] { lock.lock(); defer { lock.unlock() }; return _lines }
    var outcome: PrepareRunner.Outcome? { lock.lock(); defer { lock.unlock() }; return _outcome }
}

/// A fake `python`: a shell script that ignores the prepare arguments and prints `body`'s lines.
func fakePython(_ dir: URL, _ body: String) -> String {
    let f = dir.appendingPathComponent("fake-python-\(UUID().uuidString.prefix(6))")
    write(f, "#!/bin/sh\n" + body + "\n", executable: true)
    return f.path
}

func runFake(_ body: String, timeout: TimeInterval = 10, configure: (PrepareRunner) -> Void = { _ in },
             during: (PrepareRunner, EventSink) -> Void = { _, _ in }) -> (EventSink, PrepareRunner) {
    let d = tempDir("prep")
    let sink = EventSink()
    var req = PrepareRunner.Request(python: fakePython(d, body), toolkit: d,
                                    source: d.appendingPathComponent("game dir"), work: d.appendingPathComponent("work"))
    req.environment = ChildEnvironment.base()
    let r = PrepareRunner(req) { sink.take($0) }
    configure(r)
    try! r.start()
    during(r, sink)
    waitFor(timeout) { sink.outcome != nil }
    return (sink, r)
}

func registerPrepareChecks() {
    // ---- the grammar: PrepareProtocolTest.java's cases -----------------------------------------------------------
    check("protocol: valid progress line") {
        let p = PrepareProtocol.parseProgress("XBA_PROGRESS step=4/5 id=compile pct=42 eta=730 msg=func_0001.c")
        expectEq(p, PrepareProtocol.Progress(step: 4, n: 5, pct: 42, id: "compile", eta: 730, msg: "func_0001.c"))
        let q = PrepareProtocol.parseProgress("XBA_PROGRESS step=1/5 id=identify pct=0")
        expectEq(q?.eta, -1)
        expectEq(q?.msg, "")
    }

    check("protocol: msg is greedy, holds spaces and =") {
        let p = PrepareProtocol.parseProgress(
            "XBA_PROGRESS step=3/5 id=lift pct=10 msg=lifting  sub_0001 eta=99 pct=100 id=x=y")
        expectEq(p?.msg, "lifting  sub_0001 eta=99 pct=100 id=x=y")
        expectEq(p?.pct, 10)
        expectEq(p?.eta, -1)
        expectEq(p?.id, "lift")
    }

    check("protocol: unknown keys ignored, values clamped, CR stripped") {
        let p = PrepareProtocol.parseProgress(
            "XBA_PROGRESS future=1 step=2/5 id=analyse rate=3.5 pct=104.6 eta=12.4 bare msg=ok\r")
        expectEq(p?.pct, 100)
        expectEq(p?.eta, 12)
        expectEq(p?.msg, "ok")
        expectEq(p?.step, 2)
    }

    check("protocol: malformed lines are nil") {
        for l in [nil, "", "hello world", "XBA_PROGRESSstep=1/5 id=x pct=1", "  XBA_PROGRESS step=1/5 id=x pct=1",
                  "XBA_PROGRESS", "XBA_PROGRESS step=1/5 id=lift", "XBA_PROGRESS step=1/5 pct=3",
                  "XBA_PROGRESS id=lift pct=3", "XBA_PROGRESS step=x/5 id=lift pct=3",
                  "XBA_PROGRESS step=6/5 id=lift pct=3", "XBA_PROGRESS step=1 id=lift pct=3",
                  "XBA_PROGRESS step=1/5 id=lift pct=lots", "XBA_PROGRESS step=1/5 id=lift msg=pct=3",
                  "XBA_PROGRESS step=1/0 id=lift pct=3", "XBA_PROGRESS step=1/5 id= pct=3"] as [String?] {
            expectNil(PrepareProtocol.parseProgress(l), "\(l ?? "nil")")
        }
        let p = PrepareProtocol.parseProgress("XBA_PROGRESS step=1/5 id=lift pct=3 eta=soon")
        expectEq(p?.eta, -1, "a bad eta drops only the eta")
    }

    check("protocol: result lines, desktop exe with spaces") {
        let ok = PrepareProtocol.parseResult("XBA_RESULT ok exe=/Users/a/Library/Application Support/x/build/Black")
        expectEq(ok?.ok, true)
        expectEq(ok?.exe, "/Users/a/Library/Application Support/x/build/Black")
        let pack = PrepareProtocol.parseResult("XBA_RESULT ok pack=/data/x/packs/4D530004")
        expectEq(pack?.pack, "/data/x/packs/4D530004")
        let err = PrepareProtocol.parseResult("XBA_RESULT error code=COMPILE_FAILED msg=clang: error: 3 files failed")
        expectEq(err?.ok, false)
        expectEq(err?.code, "COMPILE_FAILED")
        expectEq(err?.msg, "clang: error: 3 files failed")
        expectEq(PrepareProtocol.parseResult("XBA_RESULT error")?.code, "UNKNOWN")
        expectNil(PrepareProtocol.parseResult("XBA_RESULT maybe code=X"))
        expectNil(PrepareProtocol.parseResult("XBA_RESULTS ok"))
        expectNil(PrepareProtocol.parseResult("XBA_PROGRESS step=1/5 id=lift pct=3"))
        expectNil(PrepareProtocol.parseResult("[5/5] building for this computer"))
    }

    check("protocol: progress.py's own lines round-trip (desktop ids)") {
        // What progress_line() writes: head, optional eta, msg last, one line.
        for (i, id) in PrepareProtocol.steps.enumerated() {
            let line = "XBA_PROGRESS step=\(i + 1)/5 id=\(id) pct=\(i * 20) eta=\(i * 7) msg=step \(id): a=b c"
            let p = PrepareProtocol.parseProgress(line)
            expectEq(p, PrepareProtocol.Progress(step: i + 1, n: 5, pct: i * 20, id: id, eta: i * 7,
                                                 msg: "step \(id): a=b c"))
            expectEq(PrepareProtocol.stepIndex(id), i)
        }
        expectNil(PrepareProtocol.stepIndex("pack"))
    }

    check("protocol: overall, eta and clock formats") {
        expectEq(PrepareProtocol.overall(step: 0, n: 5, pct: 50), 0)
        expect(abs(PrepareProtocol.overall(step: 1, n: 5, pct: 50) - 0.1) < 1e-9)
        expect(abs(PrepareProtocol.overall(step: 4, n: 5, pct: 50) - 0.7) < 1e-9)
        expectEq(PrepareProtocol.overall(step: 5, n: 5, pct: 100), 1)
        expectEq(PrepareProtocol.formatEta(30), "< 1 min")
        expectEq(PrepareProtocol.formatEta(61), "2 min")
        expectEq(PrepareProtocol.formatEta(3900), "1 h 05")
        expectEq(PrepareProtocol.formatClock(42), "0:42")
        expectEq(PrepareProtocol.formatClock(3723), "1:02:03")
    }

    check("outcome: from the result line, else from the exit") {
        var r = PrepareProtocol.Result()
        r.ok = true
        r.exe = "/x/y"
        expectEq(PrepareRunner.outcome(result: r, exit: .exited(0), cancelled: false, lastLine: ""), .ok(exe: "/x/y"))
        expectEq(PrepareRunner.outcome(result: nil, exit: .exited(0), cancelled: false, lastLine: ""), .ok(exe: nil))
        expectEq(PrepareRunner.outcome(result: nil, exit: .exited(130), cancelled: false, lastLine: ""), .cancelled)
        expectEq(PrepareRunner.outcome(result: nil, exit: .signaled(15), cancelled: true, lastLine: ""), .cancelled)
        expectEq(PrepareRunner.outcome(result: nil, exit: .exited(1), cancelled: false, lastLine: "boom"),
                 .failed(code: "EXIT_1", message: "boom"))
        expectEq(ChildExit(waitStatus: 0x0300), .exited(3))
        expectEq(ChildExit(waitStatus: 9), .signaled(9))
    }

    // ---- the runner against fake prepare scripts ----------------------------------------------------------------
    check("runner: progress then ok") {
        let (sink, r) = runFake("""
        echo "[1/5] finding default.xbe"
        echo "XBA_PROGRESS step=1/5 id=identify pct=0 msg=reading default.xbe"
        echo "XBA_PROGRESS step=5/5 id=build pct=37 msg=[12/40] Building C object gen/a b.c.o"
        echo "XBA_RESULT ok exe=$PWD/build dir/Synth"
        """)
        expectEq(sink.progress.count, 2)
        expectEq(sink.progress.last?.id, "build")
        expectEq(sink.progress.last?.pct, 37)
        expectEq(sink.lines, ["[1/5] finding default.xbe"])
        guard case .ok(let exe)? = sink.outcome else { return expect(false, "outcome \(String(describing: sink.outcome))") }
        expect(exe?.hasSuffix("/build dir/Synth") == true, "exe \(exe ?? "nil")")
        expect(exe?.hasSuffix(r.request.toolkit.lastPathComponent + "/build dir/Synth") == true,
               "cwd is the toolkit: \(exe ?? "")")
        let log = read(r.request.work.appendingPathComponent(PrepareRunner.logName))
        expect(log.contains("XBA_RESULT ok") && log.contains("--progress"), log)
    }

    check("runner: arguments") {
        var req = PrepareRunner.Request(python: "/p", toolkit: URL(fileURLWithPath: "/t"),
                                        source: URL(fileURLWithPath: "/g/My Game.iso"),
                                        work: URL(fileURLWithPath: "/w"))
        req.force = true
        expectEq(req.arguments, ["-u", "-m", "tools.prepare", "/g/My Game.iso", "--work", "/w", "--progress", "--force"])
    }

    check("runner: error result") {
        let (sink, _) = runFake("""
        echo "XBA_PROGRESS step=2/5 id=analyse pct=50 msg=disasm"
        echo "XBA_RESULT error code=ANALYSE_FAILED msg=disasm: exit 1"
        exit 1
        """)
        expectEq(sink.outcome, .failed(code: "ANALYSE_FAILED", message: "disasm: exit 1"))
    }

    check("runner: crash without a result line") {
        let (sink, _) = runFake("echo 'Traceback (most recent call last):'; echo 'KeyError: x'; exit 1")
        expectEq(sink.outcome, .failed(code: "EXIT_1", message: "KeyError: x"))
    }

    check("runner: cancel kills the whole group within 6 s") {
        // The leader ignores TERM (like a busy step), its child is a long sleep: KILL after the grace takes both.
        let d = tempDir("cancel")
        let pidFile = d.appendingPathComponent("grandchild.pid")
        var t0 = Date()
        let (sink, r) = runFake("""
        trap '' TERM
        sleep 300 &
        echo $! > "\(pidFile.path)"
        echo "XBA_PROGRESS step=5/5 id=build pct=10 msg=building"
        while true; do sleep 1; done
        """, timeout: 8, during: { r, sink in
            waitFor(5) { sink.progress.count == 1 && FileManager.default.fileExists(atPath: pidFile.path) }
            t0 = Date()
            r.cancel()
        })
        let took = Date().timeIntervalSince(t0)
        expectEq(sink.outcome, .cancelled)
        expect(took < 6, "took \(took) s")
        let gpid = pid_t(read(pidFile).trimmingCharacters(in: .whitespacesAndNewlines)) ?? 0
        expect(gpid > 0)
        expect(waitFor(2) { kill(gpid, 0) != 0 }, "the grandchild \(gpid) survived")
        expect(!r.isRunning)
    }

    check("runner: a cooperative cancel ends at once") {
        var t0 = Date()
        let (sink, _) = runFake("""
        trap 'echo "XBA_RESULT error code=CANCELLED msg=cancelled"; exit 130' TERM
        echo "XBA_PROGRESS step=3/5 id=lift pct=1 msg=x"
        while true; do sleep 0.1; done
        """, during: { r, sink in
            waitFor(5) { sink.progress.count == 1 }
            t0 = Date()
            r.cancel()
        })
        expectEq(sink.outcome, .cancelled)
        expect(Date().timeIntervalSince(t0) < 3)
    }

    // ---- --check --json ---------------------------------------------------------------------------------------
    check("title state: --check JSON decodes") {
        let s = TitleState.decode("""
        {"state": "stale", "reasons": ["toolkit commit changed", "discovered.jsonl has 3 new entries"],
         "exe": "/w/project/build/Synth", "title": "SYNTH", "title_id": "12345678", "env_force": {"RECOMP_LSFG": "0"}}
        """)
        expectEq(s?.state, .stale)
        expectEq(s?.reasons.count, 2)
        expectEq(s?.exe, "/w/project/build/Synth")
        expectEq(s?.titleId, "12345678")
        expectEq(s?.envForce, ["RECOMP_LSFG": "0"])
        expectEq(s?.reasonText, "Improvements found while playing: prepare again")
        let absent = TitleState.decode("some log line\n{\"state\": \"absent\", \"reasons\": [], \"exe\": null}")
        expectEq(absent?.state, .absent)
        expectNil(absent?.exe)
        expect(absent?.playable == false)
        expectEq(TitleState.decode("{\"state\": \"ready\", \"exe\": \"\"}")?.exe, nil)
        expectNil(TitleState.decode("Traceback"))
        expectEq(TitleState.describe(["toolkit"]), "Prepare again (toolkit updated)")
        expectEq(TitleState.describe([]), "Prepare again")
    }

    check("title state: project exe from CMakeLists") {
        let w = tempDir("work")
        write(w.appendingPathComponent("project/CMakeLists.txt"), "cmake_minimum_required(VERSION 3.20)\nproject(Synth C)\n")
        expectNil(TitleState.projectExe(work: w))
        write(w.appendingPathComponent("project/build/Synth"), "#!/bin/sh\n", executable: true)
        expectEq(TitleState.projectExe(work: w)?.lastPathComponent, "Synth")
    }
}
