import Foundation
import XRecompCore

final class GameSink: @unchecked Sendable {
    private let lock = NSLock()
    private var _lines: [String] = []
    private var _exit: (ChildExit, Bool, TimeInterval)?

    func take(_ e: GameRunner.Event) {
        lock.lock()
        defer { lock.unlock() }
        switch e {
        case .line(let l): _lines.append(l)
        case .exited(let x, let u, let s): _exit = (x, u, s)
        }
    }

    var lines: [String] { lock.lock(); defer { lock.unlock() }; return _lines }
    var exit: (ChildExit, Bool, TimeInterval)? { lock.lock(); defer { lock.unlock() }; return _exit }
}

/// A fake game: an executable shell script in a build folder.
func fakeGame(_ body: String) -> (URL, URL) {
    let d = tempDir("game")
    let exe = d.appendingPathComponent("build/Synth")
    write(exe, "#!/bin/sh\n" + body + "\n", executable: true)
    return (exe, d)
}

func registerGameChecks() {
    // ---- recomp.cfg: RecompConfig.java's semantics ---------------------------------------------------------------
    check("cfg: first line wins, comments and blanks skipped, trimmed") {
        let c = RecompConfig(lines: ["# comment", "", "  RECOMP_GPU = 1 ", "RECOMP_GPU=0", "BROKEN", "=x",
                                     "#RECOMP_ASPECT=4:3", "RECOMP_ASPECT=16:9"])
        expectEq(c.get("RECOMP_GPU"), "1")
        expectEq(c.get("RECOMP_ASPECT"), "16:9")
        expectNil(c.get("BROKEN"))
        expectNil(c.get("RECOMP_NOPE"))
        expectEq(c.activePairs.map { "\($0.key)=\($0.value)" }, ["RECOMP_GPU=1", "RECOMP_ASPECT=16:9"])
    }

    check("cfg: set rewrites the first line in place, removes duplicates, keeps hand lines") {
        var c = RecompConfig(lines: ["# mine", "RECOMP_FOO=bar", "RECOMP_GPU=1", "RECOMP_X=1", "RECOMP_GPU=0"])
        c.set("RECOMP_GPU", "0")
        expectEq(c.lines, ["# mine", "RECOMP_FOO=bar", "RECOMP_GPU=0", "RECOMP_X=1"])
        c.set("RECOMP_ASPECT", "4:3")
        expectEq(c.lines, ["# mine", "RECOMP_FOO=bar", "RECOMP_GPU=0", "RECOMP_X=1", "", RecompConfig.header,
                           "RECOMP_ASPECT=4:3"])
        c.set("RECOMP_ASPECT", "16:9")
        expectEq(c.lines.last, "RECOMP_ASPECT=16:9")
        expectEq(c.lines.filter { $0 == RecompConfig.header }.count, 1)
        c.set("RECOMP_GPU", nil)
        expect(!c.lines.contains { $0.hasPrefix("RECOMP_GPU") }, "a default removes the line")
        expect(c.lines.contains("RECOMP_FOO=bar") && c.lines.contains("# mine"))
    }

    check("cfg: options, choice index, raw value, reset keeps hand lines") {
        var c = RecompConfig(lines: ["RECOMP_FOO=bar"])
        let gpu = OptionTable.option("gpu")!, tex = OptionTable.option("filtering")!, buf = OptionTable.option("audio-buffer")!
        expectEq(c.choiceIndex(gpu), 0)
        expect(c.allDefault())
        c.set(gpu, gpu.choices[1])
        expectEq(c.get("RECOMP_GPU"), "1")
        c.set(tex, tex.choices[2])
        expectEq(c.get("RECOMP_TEX_ANISO"), "8")
        expectNil(c.get("RECOMP_TEX_BILINEAR"))
        expectEq(c.rawValue(tex), "RECOMP_TEX_ANISO=8")
        expectEq(c.choiceIndex(buf), 1, "Normal is the default, not the first")
        c.set(buf, buf.choices[0])
        expectEq(c.get("RECOMP_AUDIO_BUF_SAMPLES"), "256")
        c.set("RECOMP_GPU_SCALE", "1.5")
        expectNil(c.choiceIndex(OptionTable.option("gpu-scale")!), "a value the list does not offer")
        expect(!c.allDefault())
        c.resetAll()
        expect(c.allDefault())
        expectEq(c.get("RECOMP_FOO"), "bar")
        expectNil(c.get("RECOMP_GPU"))
        // Every option has exactly one default choice with as many values as keys.
        for o in OptionTable.all {
            expectEq(o.choices.filter(\.isDefault).count, 1, o.id)
            expect(o.choices.allSatisfy { $0.values.count == o.keys.count }, o.id)
            expect(!o.keys.contains("RECOMP_BIG_CORES"), o.id)
        }
    }

    check("cfg: Advanced text edits the unmanaged lines only") {
        var c = RecompConfig(lines: ["# mine", "RECOMP_FOO=bar", "", RecompConfig.header, "RECOMP_GPU=1"])
        expectEq(c.unmanagedText(), "# mine\nRECOMP_FOO=bar\n")
        c.setUnmanagedText("RECOMP_FOO=baz\nRECOMP_GPU=0\nRECOMP_NEW=1\n")
        expectEq(c.get("RECOMP_FOO"), "baz")
        expectEq(c.get("RECOMP_GPU"), "1", "a managed key typed into Advanced is dropped")
        expectEq(c.get("RECOMP_NEW"), "1")
        expectEq(c.lines, ["RECOMP_FOO=baz", "RECOMP_NEW=1", "", RecompConfig.header, "RECOMP_GPU=1"])
    }

    check("cfg: load and save, atomic, nothing written for an all-default new file") {
        let d = tempDir("cfg")
        var c = RecompConfig.load(dir: d)
        expectEq(c.lines, [])
        c.set("RECOMP_GPU", nil)
        try c.save()
        expect(!FileManager.default.fileExists(atPath: d.appendingPathComponent("recomp.cfg").path))
        c.set("RECOMP_GPU", "1")
        try c.save()
        expectEq(read(d.appendingPathComponent("recomp.cfg")), "\(RecompConfig.header)\nRECOMP_GPU=1\n")
        expect(!FileManager.default.fileExists(atPath: d.appendingPathComponent("recomp.cfg.tmp").path))
        write(d.appendingPathComponent("recomp.cfg"), "RECOMP_FOO=1\r\nRECOMP_GPU=1\r\n")
        var back = RecompConfig.load(dir: d)
        expectEq(back.lines, ["RECOMP_FOO=1", "RECOMP_GPU=1"])
        back.set("RECOMP_GPU", nil)
        try back.save()
        expectEq(read(d.appendingPathComponent("recomp.cfg")), "RECOMP_FOO=1\n", "an existing file is rewritten")
    }

    check("game env: the title's cfg, defaults, save dir, PATH") {
        let home = AppPaths(root: tempDir("home"))
        let t = home.title("12345678")
        var cfg = RecompConfig(lines: ["RECOMP_GPU=1", "RECOMP_GPU=0", "SDL_AUDIODRIVER=dummy"])
        cfg.set("RECOMP_ASPECT", "4:3")
        let req = GameRunner.Request.forTitle(exe: t.build.appendingPathComponent("Synth"), title: t, config: cfg,
                                              globalDefaults: ["RECOMP_ASPECT": "16:9", "RECOMP_WINDOW_SCALE": "3"],
                                              inherited: ["PATH": "/usr/bin:/bin", "HOME": "/h"],
                                              logs: RunLog(dir: t.logs))
        expectEq(req.environment["RECOMP_GPU"], "1")
        expectEq(req.environment["RECOMP_ASPECT"], "4:3")
        expectEq(req.environment["RECOMP_WINDOW_SCALE"], "3")
        expectEq(req.environment["SDL_AUDIODRIVER"], "dummy")
        expectEq(req.environment["RECOMP_SAVE_DIR"], t.save.path)
        expect(req.environment["PATH"]!.hasPrefix("/opt/homebrew/bin:"))
        expectEq(req.cwd.path, t.build.path)
        expect(req.log?.lastPathComponent.hasPrefix("run-") == true)
    }

    // ---- the runner against /bin/sh fakes --------------------------------------------------------------------------
    check("game: clean exit, output to the log and the callback") {
        let (exe, d) = fakeGame("echo \"hello $RECOMP_GPU\"; pwd; exit 0")
        let sink = GameSink()
        let log = d.appendingPathComponent("logs/run-1.log")
        let g = GameRunner(.init(exe: exe, environment: ChildEnvironment.base().merging(["RECOMP_GPU": "1"]) { $1 },
                                 log: log)) { sink.take($0) }
        try g.start()
        expect(waitFor(5) { sink.exit != nil })
        expectEq(sink.exit?.0, .exited(0))
        expectEq(sink.lines.first, "hello 1")
        expect(sink.lines.last?.hasSuffix("/build") == true, "cwd is the build folder")
        let text = read(log)
        expect(text.contains("# RECOMP_GPU=1") && text.contains("hello 1") && text.contains("# ended: exit 0"), text)
        expectNil(GameRunner.exitProblem(.exited(0), stoppedByUser: false))
    }

    check("game: non-zero exit and signal are reported") {
        let (exe, _) = fakeGame("echo boom >&2; exit 3")
        let sink = GameSink()
        let g = GameRunner(.init(exe: exe, environment: ChildEnvironment.base(), log: nil)) { sink.take($0) }
        try g.start()
        expect(waitFor(5) { sink.exit != nil })
        expectEq(sink.exit?.0, .exited(3))
        expectEq(sink.lines, ["boom"])
        expectEq(GameRunner.exitProblem(.exited(3), stoppedByUser: false), "Stopped unexpectedly (exit 3)")
        let (exe2, _) = fakeGame("kill -SEGV $$")
        let sink2 = GameSink()
        let g2 = GameRunner(.init(exe: exe2, environment: ChildEnvironment.base(), log: nil)) { sink2.take($0) }
        try g2.start()
        expect(waitFor(5) { sink2.exit != nil })
        expectEq(sink2.exit?.0, .signaled(SIGSEGV))
        expectEq(GameRunner.exitProblem(.signaled(11), stoppedByUser: false), "Stopped unexpectedly (signal 11)")
        expectNil(GameRunner.exitProblem(.signaled(15), stoppedByUser: true))
    }

    check("game: stop is TERM, then KILL by PID") {
        // Cooperative: TERM ends it at once.
        let (exe, _) = fakeGame("trap 'echo bye; exit 0' TERM; echo up; while true; do sleep 0.1; done")
        let sink = GameSink()
        let g = GameRunner(.init(exe: exe, environment: ChildEnvironment.base(), log: nil)) { sink.take($0) }
        try g.start()
        expect(waitFor(5) { sink.lines.contains("up") })
        var t0 = Date()
        g.stop()
        expect(waitFor(3) { sink.exit != nil })
        expect(Date().timeIntervalSince(t0) < 2)
        expectEq(sink.lines.last, "bye")
        expectEq(sink.exit?.1, true, "stopped by the player")
        // Stubborn: TERM ignored, KILL after the grace.
        let (exe2, _) = fakeGame("trap '' TERM; echo up; while true; do sleep 0.1; done")
        let sink2 = GameSink()
        let g2 = GameRunner(.init(exe: exe2, environment: ChildEnvironment.base(), log: nil)) { sink2.take($0) }
        g2.stopGrace = 1
        try g2.start()
        expect(waitFor(5) { sink2.lines.contains("up") })
        t0 = Date()
        g2.stop()
        expect(waitFor(4) { sink2.exit != nil })
        let took = Date().timeIntervalSince(t0)
        expect(took >= 0.9 && took < 3, "took \(took)")
        expectEq(sink2.exit?.0, .signaled(SIGKILL))
    }

    check("game: a missing executable fails to start") {
        let g = GameRunner(.init(exe: URL(fileURLWithPath: "/nonexistent/game"), environment: [:], log: nil)) { _ in }
        var threw = false
        do { try g.start() } catch { threw = true }
        expect(threw)
    }

    // ---- logs, play time, storage --------------------------------------------------------------------------------
    check("game: a crash's tail comes from its own run log") {
        // The store shows this tail on « Stopped unexpectedly »; the live log can hold a prepare's output by then.
        let (exe, d) = fakeGame("i=0; while [ $i -lt 60 ]; do echo out$i; i=$((i+1)); done; exit 3")
        let sink = GameSink()
        let log = d.appendingPathComponent("logs/run-1.log")
        let g = GameRunner(.init(exe: exe, environment: ChildEnvironment.base(), log: log)) { sink.take($0) }
        try g.start()
        expect(waitFor(5) { sink.exit != nil })
        expectEq(sink.exit?.0, .exited(3))
        let tail = tailLines(log, 40)
        expectEq(tail.count, 40)
        expectEq(tail.first, "out21")
        expect(tail.last?.hasPrefix("# ended: exit 3") == true, tail.last ?? "")
    }

    check("logs: rotation keeps 10") {
        let r = RunLog(dir: tempDir("logs"))
        let base = Date(timeIntervalSince1970: 1_800_000_000)
        for i in 0..<14 {
            let u = r.newLog(now: base.addingTimeInterval(Double(i) * 60))
            write(u, "run \(i)\n")
        }
        let logs = r.logs()
        expectEq(logs.count, 10)
        expectEq(read(logs.last!), "run 13\n")
        expectEq(read(logs.first!), "run 4\n")
        // Same second twice: a second name, not an overwrite.
        let later = base.addingTimeInterval(3600)
        let a = r.newLog(now: later)
        write(a, "")
        let b = r.newLog(now: later)
        expect(b != a, "a second name, not an overwrite")
        expectEq(r.latest, a)
        expectEq(tailLines(a, 5).count, 0, "an empty log")
        let t = tempDir("tail").appendingPathComponent("t.log")
        write(t, (1...100).map(String.init).joined(separator: "\n") + "\n")
        expectEq(tailLines(t, 3), ["98", "99", "100"])
    }

    check("logs: a failed log write is reported, not raised (full disk)") {
        let f = tempDir("logw").appendingPathComponent("a.log")
        write(f, "")
        let w = try FileHandle(forWritingTo: f)
        expect(appendLogLine("one", to: w))
        try w.close()
        expectEq(read(f), "one\n")
        // A write the system refuses (here EBADF on a read-only handle; ENOSPC on a full disk) comes back as false:
        // the legacy write(_:) raised an Objective-C exception there and the launcher aborted.
        let r = try FileHandle(forReadingFrom: f)
        expect(!appendLogLine("two", to: r))
        try r.close()
        expectEq(read(f), "one\n")
    }

    check("state: play time and prepare record round trip") {
        let f = tempDir("state").appendingPathComponent("state.json")
        var r = TitleRecord.load(from: f)
        expectEq(r.playTimeText, "Not played yet")
        r.sourcePath = "/g/x.iso"
        r.recordRun(seconds: 30, end: Date(timeIntervalSince1970: 1_800_000_000))
        expectEq(r.playTimeText, "Under a minute played")
        r.recordRun(seconds: 3 * 3600 + 5 * 60)
        expectEq(r.playTimeText, "3 h 05 played")
        r.lastPrepare = .init(ok: false, code: "COMPILE_FAILED", message: "x", at: Date(timeIntervalSince1970: 1_800_000_100))
        try r.save(to: f)
        expectEq(TitleRecord.load(from: f), r)
        expectEq(r.runs, 2)
    }

    check("storage: sizes, deletes never follow the game link") {
        let home = AppPaths(root: tempDir("store"))
        let t = home.title("12345678")
        let source = tempDir("players-disc")
        write(source.appendingPathComponent("default.xbe"), String(repeating: "x", count: 10000))
        write(source.appendingPathComponent("data/level.bin"), String(repeating: "y", count: 50000))
        write(t.build.appendingPathComponent("Synth"), String(repeating: "z", count: 20000))
        try FileManager.default.createSymbolicLink(at: t.build.appendingPathComponent("game"), withDestinationURL: source)
        write(t.save.appendingPathComponent("UDATA/12345678/save1/a.dat"), String(repeating: "s", count: 4000))
        write(t.save.appendingPathComponent("Cache/12345678/z.bin"), String(repeating: "c", count: 8000))
        let sizes = Dictionary(uniqueKeysWithValues: Storage.sizes(t).map { ($0.0, $0.1) })
        expect(sizes[.work]! >= 20000 && sizes[.work]! < 50000, "the link's target is not counted: \(sizes[.work]!)")
        expect(sizes[.saves]! >= 12000)
        expect(sizes[.utilityCache]! >= 8000)
        expectEq(sizes[.extractedDisc], 0)

        try Storage.delete(.work, t)
        expect(!FileManager.default.fileExists(atPath: t.work.path))
        expectEq(read(source.appendingPathComponent("data/level.bin")).count, 50000, "the player's folder is intact")
        expect(FileManager.default.fileExists(atPath: source.appendingPathComponent("default.xbe").path))

        // A link as the item itself is unlinked, not followed.
        let link = home.root.appendingPathComponent("link")
        try FileManager.default.createSymbolicLink(at: link, withDestinationURL: source)
        try Storage.delete(link)
        expect(FileManager.default.fileExists(atPath: source.appendingPathComponent("default.xbe").path))
        try Storage.delete(.saves, t)
        expect(!FileManager.default.fileExists(atPath: t.save.path))
        try Storage.delete(.saves, t)   // already gone: no error
        expect(Storage.freeSpace(at: t.work)! > 0)
        // A disc image is read in place (XBA-126): a first prepare needs the same 2 GB as a folder.
        expectEq(Storage.prepareEstimate, 2 << 30)
    }

    check("storage: the Z: cache is the runtime's lower-case folder") {
        let t = AppPaths(root: tempDir("zcache")).title("4541005b")
        expectEq(Storage.url(.utilityCache, t).lastPathComponent, "4541005b")
        expectEq(Storage.url(.utilityCache, t).deletingLastPathComponent().lastPathComponent, "Cache")
        // Only an upper-case folder there (made by hand): that one, so its size and Delete still mean something.
        write(t.save.appendingPathComponent("Cache/4541005B/z.bin"), "z")
        let found = Storage.url(.utilityCache, t)
        expect(FileManager.default.fileExists(atPath: found.appendingPathComponent("z.bin").path), found.path)
    }

    check("saves: import from command-line runs, never overwriting") {
        let old = tempDir("xdg"), save = tempDir("save")
        write(old.appendingPathComponent("UDATA/4541005B/slot/a.sav"), "old save")
        write(old.appendingPathComponent("TDATA/4541005B/t.dat"), "t")
        expectEq(SaveImport.defaultSaveDir(environment: ["XDG_DATA_HOME": "/x"]).path, "/x/xboxrecomp")
        expectEq(SaveImport.candidates(titleId: "4541005B", from: old, to: save).count, 2)
        expectEq(try SaveImport.run(titleId: "4541005B", from: old, to: save), 2)
        expectEq(read(save.appendingPathComponent("UDATA/4541005B/slot/a.sav")), "old save")
        write(old.appendingPathComponent("UDATA/4541005B/slot/a.sav"), "newer")
        expectEq(try SaveImport.run(titleId: "4541005B", from: old, to: save), 0, "nothing overwritten")
        expectEq(read(save.appendingPathComponent("UDATA/4541005B/slot/a.sav")), "old save")
    }
}
