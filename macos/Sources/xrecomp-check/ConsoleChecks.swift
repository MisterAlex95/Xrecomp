import Foundation
import XRecompCore

/// XBA-278: GBA, PS1, N64 and GameCube games in the launcher (ConsoleGames.swift): which files the scan hands to the
/// console identify, the xrecomp command line's answers, the folders a console game uses, its prepare and its play
/// request; and, when the toolkit and a Python are there, the real `xrecomp identify` / `play --print --json` on a
/// synthetic N64 ROM (tools/prepare/test_n64_prepare.py make_rom: ours, no game content).

/// One identify answer as tools/xrecomp/__main__.py identify_dict writes it.
func identifyJSON(_ source: String, id: String?, console: String?, state: String, error: String? = nil,
                  player: String? = "/usr/local/bin/n64_play") -> String {
    func q(_ s: String?) -> String { s.map { "\"\($0)\"" } ?? "null" }
    return """
    {"id": \(q(id)), "console": \(q(console)), "title": "Test", "source": "\(source)", "state": "\(state)",
     "label": "x", "reasons": \(state == "stale" ? "[\"toolkit\"]" : "[]"), "note": null, "exe": null, "pack": null,
     "error": \(q(error)), "key": null, "player": \(q(player)), "save_dir": "/h/save", "pack_dir": "/h/packs/\(id ?? "")"}
    """
}

/// tools/prepare/test_n64_prepare.py make_rom: a big-endian (z64) ROM, game code NSME, CRC1 635A2BFF.
func synthN64Rom(size: Int = 0x10000) -> [UInt8] {
    var rom = [UInt8](repeating: 0, count: size)
    rom.replaceSubrange(0..<4, with: [0x80, 0x37, 0x12, 0x40])
    rom.replaceSubrange(8..<12, with: [0x80, 0x00, 0x04, 0x00])
    rom.replaceSubrange(0x10..<0x14, with: [0x63, 0x5A, 0x2B, 0xFF])
    let title = Array("XBA PREPARE TEST".utf8) + [UInt8](repeating: 0x20, count: 4)
    rom.replaceSubrange(0x20..<0x34, with: title)
    rom.replaceSubrange(0x3B..<0x3F, with: Array("NSME".utf8))
    rom.replaceSubrange(0x340..<0x350, with: Array(" Libdragon IPL3 ".utf8))
    for i in 0x1000..<size { rom[i] = UInt8((i * 13 + (i >> 8)) & 0xFF) }
    return rom
}

func registerConsoleChecks() {
    let repo = URL(fileURLWithPath: #filePath).deletingLastPathComponent()   // Sources/xrecomp-check
        .deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent()

    check("console: game IDs, names, players") {
        expectEq(Console.of(gameId: "gba-AXVE-1A2B3C4D"), .gba)
        expectEq(Console.of(gameId: "GC-GALE01-0"), .gc)
        expectEq(Console.of(gameId: "n64-NSME-635A2BFF"), .n64)
        expectEq(Console.of(gameId: "4D530004"), .xbox)
        expectEq(Console.of(gameId: "xbox-1"), .xbox, "no « xbox- » game IDs")
        expectEq(Console.of(gameId: "snes-ABC"), .xbox)
        expectEq(Console.of(gameId: nil), .xbox)
        expectEq(Console.ps1.player, "ps1_play")
        expectNil(Console.xbox.player)
        expectEq(Console.gc.thumbnailSystem, "Nintendo_-_GameCube")
    }

    check("console: which files the scan hands to the console identify") {
        for n in ["a.gba", "b.Z64", "c.v64", "d.n64", "e.cue", "f.chd", "g.gcm", "h.gcz", "i.rvz", "j.wia", "k.dol"] {
            expectEq(ConsoleSources.claim(n), .console, n)
        }
        expectEq(ConsoleSources.claim("x.iso"), .shared)
        expectEq(ConsoleSources.claim("x.CISO"), .shared)
        expectEq(ConsoleSources.claim("x.bin"), .track)
        expectEq(ConsoleSources.claim("x.img"), .track)
        for n in ["x.exe", "x.txt", "default.xbe", "x.cso", "x.sav"] { expectEq(ConsoleSources.claim(n), .none, n) }
    }

    check("console: identify answers parsed, and how prepared") {
        let out = "some warning\n[" + [
            identifyJSON("/l/m.z64", id: "n64-NSME-635A2BFF", console: "n64", state: "absent"),
            identifyJSON("/l/g.gba", id: "gba-K", console: "gba", state: "prepared"),
            identifyJSON("/l/p.cue", id: "ps1-K", console: "ps1", state: "stale"),
            identifyJSON("/l/x.iso", id: "12345678", console: "xbox", state: "absent", player: nil),
            identifyJSON("/l/junk.rvz", id: nil, console: "gc", state: "unreadable", error: "BAD_IMAGE: no"),
        ].joined(separator: ",") + "]\n"
        let ids = ConsoleIdentity.parse(out)
        expectEq(ids.count, 5)
        expectEq(ids[0].consoleGame, .n64)
        expectEq(ids[0].prepared, .interpreted)
        expectEq(ids[0].player, "/usr/local/bin/n64_play")
        expectEq(ids[0].packDir, "/h/packs/n64-NSME-635A2BFF")
        expectEq(ids[1].prepared, .native)
        expectEq(ids[2].prepared, .stale(["toolkit"]))
        expectNil(ids[3].consoleGame, "an Xbox source is not a console game")
        expectNil(ids[4].consoleGame)
        expectEq(ids[4].prepared, .problem("BAD_IMAGE: no"))
        expectEq(ConsoleIdentity.parse("Traceback ...").count, 0)
    }

    check("console: the play request, started by the launcher") {
        let out = """
        {"id": "n64-K", "console": "n64", "title": "T", "argv": ["/b/n64_play", "/l/m.z64", "--pack", "/h/p.so"],
         "cwd": "/toolkit", "env": {"PATH": "/usr/bin", "RECOMP_SAVE_DIR": "/h/save", "RECOMP_LSFG": "1",
         "HOME": "/Users/x"}, "changes": {}}
        """
        guard let p = expectNotNil(ConsolePlay.parse(out)) else { return }
        let r = p.request(log: nil)
        expectEq(r.exe.path, "/b/n64_play")
        expectEq(r.arguments, ["/l/m.z64", "--pack", "/h/p.so"])
        expectEq(r.cwd.path, "/toolkit")
        expectEq(r.environment["RECOMP_SAVE_DIR"], "/h/save")
        expect(r.environment["PATH"]!.hasPrefix("/opt/homebrew/bin:"), "Homebrew first, as an Xbox title's")
        expectNotNil(r.environment["DYLD_FALLBACK_LIBRARY_PATH"], "frame generation's Vulkan, as an Xbox title's")
        expectNil(ConsolePlay.parse("{\"argv\": [], \"cwd\": \"/\", \"env\": {}}"), "no player")
        expectNil(ConsolePlay.parse("xrecomp: n64_play is not built"))
        expectEq(ConsoleCLI.errorLine("noise\nxrecomp: n64_play is not built\n", status: 1), "n64_play is not built")
        expectEq(ConsoleCLI.errorLine("", status: 3), "xrecomp failed (exit 3)")
        let cli = ConsoleCLI(python: "/py", toolkit: URL(fileURLWithPath: "/tk"), home: URL(fileURLWithPath: "/h"))
        expectEq(cli.playArguments("gba-K"), ["-m", "tools.xrecomp", "--home", "/h", "play", "gba-K", "--print", "--json"])
        expectEq(cli.identifyArguments([URL(fileURLWithPath: "/l/a.gba")]),
                 ["-m", "tools.xrecomp", "--home", "/h", "identify", "/l/a.gba", "--json"])
    }

    check("console: a game's folders are the xrecomp command line's") {
        let paths = AppPaths(root: URL(fileURLWithPath: "/h", isDirectory: true))
        let t = paths.title("gba-AXVE-1A2B3C4D")
        expectEq(t.titleId, "gba-AXVE-1A2B3C4D", "kept as it is (tools/xrecomp/home.py)")
        expectEq(t.dir.path, "/h/titles/gba-AXVE-1A2B3C4D")
        expectEq(t.console, .gba)
        expectEq(t.save.path, "/h/save", "the runtime adds <ID>/")
        expectEq(t.consoleSave?.path, "/h/save/gba-AXVE-1A2B3C4D")
        expectEq(t.pack?.path, "/h/packs/gba-AXVE-1A2B3C4D")
        expectEq(Storage.url(.saves, t).path, "/h/save/gba-AXVE-1A2B3C4D", "never the shared save/")
        expectEq(Storage.items(t), [.work, .pack, .saves, .logs, .art])
        let x = paths.title("4d530004")
        expectEq(x.titleId, "4D530004")
        expectEq(x.save.path, "/h/titles/4D530004/save")
        expectNil(x.consoleSave)
        expectNil(x.pack)
        expect(!Storage.items(x).contains(.pack) && Storage.items(x).contains(.utilityCache))
        expectEq(paths.packs.path, "/h/packs")
        var moved = paths
        moved.workRoot = URL(fileURLWithPath: "/w", isDirectory: true)
        expectEq(moved.title("n64-K").work.path, "/w/n64-K")
    }

    check("console: prepare with the packs folder") {
        let r = PrepareRunner.Request.console(python: "/py", toolkit: URL(fileURLWithPath: "/tk"),
                                              source: URL(fileURLWithPath: "/l/m.z64"),
                                              work: URL(fileURLWithPath: "/h/titles/n64-K/work"),
                                              packs: URL(fileURLWithPath: "/h/packs"))
        expectEq(r.arguments, ["-u", "-m", "tools.prepare", "/l/m.z64", "--work", "/h/titles/n64-K/work",
                               "--progress", "--pack-dir", "/h/packs"])
        let x = PrepareRunner.Request(python: "/py", toolkit: URL(fileURLWithPath: "/tk"),
                                      source: URL(fileURLWithPath: "/l/a.iso"), work: URL(fileURLWithPath: "/w"))
        expect(!x.arguments.contains("--pack-dir"), "an Xbox title's prepare is as it was")
    }

    check("console: the scan lists console games, a cue's tracks once, a GameCube .iso after the Xbox identify") {
        let lib = tempDir("lib-console")
        write(lib.appendingPathComponent("gba/game.gba"), "rom")
        write(lib.appendingPathComponent("ps1/Disc.cue"), "cue")
        write(lib.appendingPathComponent("ps1/Disc (Track 1).bin"), "track")
        write(lib.appendingPathComponent("ps1/Disc (Track 2).bin"), "track")
        write(lib.appendingPathComponent("lone/Lone.bin"), "bin")
        write(lib.appendingPathComponent("gc/Game.iso"), "gc disc")
        write(lib.appendingPathComponent("xbox/Xbox.iso"), "xbox disc")
        write(lib.appendingPathComponent("gc/notes.txt"), "x")
        write(lib.appendingPathComponent("bad/broken.z64"), "x")
        var asked: [[String]] = []
        var s = LibraryScanner { url in
            url.lastPathComponent == "Xbox.iso" ? .success(Identity(titleId: "4D530004", title: "Xbox Game"))
                : .failure(.init("BAD_IMAGE"))
        }
        // Xbox titles only without the console identify: nothing else is even looked at.
        expectEq(s.findSources(in: [lib]).map { $0.0.lastPathComponent }.sorted(), ["Game.iso", "Xbox.iso"])
        s.identifyConsoles = { urls in
            asked.append(urls.map(\.lastPathComponent))
            var out: [String: ConsoleIdentity] = [:]
            for u in urls {
                let name = u.lastPathComponent
                let (id, console): (String?, String) = name == "game.gba" ? ("gba-K1", "gba")
                    : name == "Disc.cue" ? ("ps1-K2", "ps1") : name == "Lone.bin" ? ("ps1-K3", "ps1")
                    : name == "Game.iso" ? ("gc-GALE01-0", "gc") : (nil, "n64")
                let json = identifyJSON(u.path, id: id, console: console, state: "absent",
                                        error: id == nil ? "not an N64 ROM" : nil)
                out[u.path] = ConsoleIdentity.parse("[" + json + "]").first
            }
            return out
        }
        var index = LibraryIndex()
        let entries = s.scan([lib], index: &index)
        expectEq(asked.count, 1, "one batch")
        expectEq(Set(asked.first ?? []), ["game.gba", "Disc.cue", "Lone.bin", "Game.iso", "broken.z64"],
                 "the cue's tracks and the Xbox image are not asked")
        let byName = Dictionary(uniqueKeysWithValues: entries.map { (URL(fileURLWithPath: $0.path).lastPathComponent, $0) })
        expectEq(byName["game.gba"]?.titleId, "gba-K1")
        expectEq(byName["game.gba"]?.kind, .console)
        expectEq(byName["game.gba"]?.consoleKind, .gba)
        expectEq(byName["Game.iso"]?.titleId, "gc-GALE01-0")
        expectEq(byName["Game.iso"]?.console, "gc")
        expectNil(byName["Game.iso"]?.problem)
        expectEq(byName["Xbox.iso"]?.titleId, "4D530004")
        expect(byName["Xbox.iso"]?.isConsole == false)
        expectEq(byName["broken.z64"]?.problem, "not an N64 ROM")
        expectNil(byName["Disc (Track 1).bin"])
        expectNil(byName["notes.txt"])
        // The index keeps them (with their console): a rescan asks nothing but the broken one again.
        asked = []
        let again = s.scan([lib], index: &index)
        expectEq(asked, [["broken.z64"]])
        expectEq(again.first { $0.titleId == "gc-GALE01-0" }?.console, "gc")
        // An index written before XBA-278 (no console key) still reads.
        let old = Data("{\"v\": 1, \"entries\": {\"/a.iso\": {\"path\": \"/a.iso\", \"kind\": \"image\", \"title_id\": \"1\"}}}".utf8)
        let decoded = try JSONDecoder().decode(LibraryIndex.self, from: old)
        expectNil(decoded.entries["/a.iso"]?.console)
        expectEq(decoded.entries["/a.iso"]?.consoleKind, .xbox)
    }

    check("console: the real xrecomp identify and play request on a synthetic N64 ROM") {
        let python = ["/usr/bin/python3", "/opt/homebrew/bin/python3", "/usr/local/bin/python3"]
            .first { FileManager.default.isExecutableFile(atPath: $0) }
        guard let python, FileManager.default.fileExists(atPath: repo.appendingPathComponent("tools/xrecomp").path)
        else { return print("      (skipped: no python3 or toolkit)") }
        let d = tempDir("console-cli")
        let rom = d.appendingPathComponent("lib/m.z64")
        try FileManager.default.createDirectory(at: rom.deletingLastPathComponent(), withIntermediateDirectories: true)
        write(rom, Data(synthN64Rom()))
        let player = d.appendingPathComponent("n64_play")
        write(player, "#!/bin/sh\necho \"$@\"\n", executable: true)
        setenv("XRECOMP_N64_PLAY", player.path, 1)
        defer { unsetenv("XRECOMP_N64_PLAY") }
        let home = d.appendingPathComponent("home", isDirectory: true)
        let cli = ConsoleCLI(python: python, toolkit: repo, home: home)
        let ids = cli.identify([rom])
        guard let a = expectNotNil(ids[rom.path], "identify answered") else { return }
        expectEq(a.id, "n64-NSME-635A2BFF")
        expectEq(a.consoleGame, .n64)
        expectEq(a.prepared, .interpreted)
        expectEq(a.player, player.path)
        expectEq(a.saveDir, home.appendingPathComponent("save").path)
        // play needs the game in the library: the command line finds it by its source too.
        var rec = TitleRecord()
        rec.sourcePath = rom.path
        try rec.save(to: AppPaths(root: home).title("n64-NSME-635A2BFF").state)
        switch cli.play("n64-NSME-635A2BFF") {
        case .success(let p):
            expectEq(p.argv, [player.path, rom.path])
            expectEq(p.cwd, repo.path)
            expectEq(p.env["RECOMP_SAVE_DIR"], home.appendingPathComponent("save").path)
        case .failure(let e):
            H.fail("play: \(e)", #file, #line)
        }
        if case .success = cli.play("n64-NOPE") { H.fail("an unknown game played", #file, #line) }
    }
}
