import Foundation
import XRecompCore

/// A fake toolkit: the two things Toolkit.isValid looks for.
func fakeToolkit(_ dir: URL) -> URL {
    write(dir.appendingPathComponent("tools/prepare/__main__.py"), "# fake\n")
    try! FileManager.default.createDirectory(at: dir.appendingPathComponent("templates/new-game"),
                                             withIntermediateDirectories: true)
    return dir
}

func registerSetupChecks() {
    check("toolkit: validity needs tools/prepare and templates/new-game") {
        let d = tempDir("tk")
        expect(!Toolkit.isValid(d))
        write(d.appendingPathComponent("tools/prepare/__main__.py"), "#")
        expect(!Toolkit.isValid(d), "templates/new-game missing")
        _ = fakeToolkit(d)
        expect(Toolkit.isValid(d))
    }

    check("toolkit: resolution order setting > env > Info.plist > above the executable") {
        let a = fakeToolkit(tempDir("tkA")), b = fakeToolkit(tempDir("tkB")), c = fakeToolkit(tempDir("tkC"))
        let d = fakeToolkit(tempDir("tkD"))
        let exe = d.appendingPathComponent("macos/build/XRecomp.app/Contents/MacOS/XRecomp")
        write(exe, "", executable: true)
        let env = ["XBA_TOOLKIT": b.path]
        var t = Toolkit.resolve(setting: a.path, environment: env, infoPlistPath: c.path, executable: exe)
        expectEq(t?.root.path, a.path)
        expectEq(t?.source, .setting)
        t = Toolkit.resolve(setting: nil, environment: env, infoPlistPath: c.path, executable: exe)
        expectEq(t?.source, .environment)
        expectEq(t?.root.path, b.path)
        t = Toolkit.resolve(setting: "", environment: [:], infoPlistPath: c.path, executable: exe)
        expectEq(t?.source, .infoPlist)
        t = Toolkit.resolve(setting: nil, environment: [:], infoPlistPath: nil, executable: exe)
        expectEq(t?.source, .bundle)
        expectEq(t?.root.resolvingSymlinksInPath().path, d.resolvingSymlinksInPath().path)
        // An invalid candidate is skipped, not taken.
        let bogus = tempDir("bogus")
        t = Toolkit.resolve(setting: bogus.path, environment: ["XBA_TOOLKIT": bogus.path], infoPlistPath: c.path,
                            executable: exe)
        expectEq(t?.source, .infoPlist)
        // Nothing anywhere: nil, the Setup screen asks.
        let lone = tempDir("lone").appendingPathComponent("bin/XRecomp")
        write(lone, "", executable: true)
        expectNil(Toolkit.resolve(setting: nil, environment: [:], infoPlistPath: nil, executable: lone))
    }

    check("child PATH: Homebrew first, inherited entries kept once") {
        let p = ChildEnvironment.path(inherited: "/usr/bin:/Users/x/bin::/opt/homebrew/bin:/bin")
        let parts = p.split(separator: ":").map(String.init)
        expectEq(parts.first, "/opt/homebrew/bin")
        expectEq(Array(parts.prefix(ChildEnvironment.fixedPath.count)), ChildEnvironment.fixedPath)
        expect(parts.contains("/Users/x/bin"))
        expectEq(parts.filter { $0 == "/usr/bin" }.count, 1)
        expect(!parts.contains(""))
        // Finder's launchd PATH still gets Homebrew.
        expect(ChildEnvironment.path(inherited: "/usr/bin:/bin:/usr/sbin:/sbin").hasPrefix("/opt/homebrew/bin:"))
        expectEq(ChildEnvironment.path(inherited: nil), ChildEnvironment.fixedPath.joined(separator: ":"))
    }

    check("child env: PATH, then defaults, then recomp.cfg, then RECOMP_SAVE_DIR") {
        let save = URL(fileURLWithPath: "/tmp/x y/save")
        let e = ChildEnvironment.game(inherited: ["PATH": "/usr/bin", "HOME": "/Users/x", "RECOMP_GPU": "1"],
                                      globalDefaults: ["RECOMP_ASPECT": "16:9", "RECOMP_GPU_SCALE": "2"],
                                      config: [("RECOMP_GPU", "0"), ("RECOMP_GPU_SCALE", "3"),
                                               ("PATH", "/evil"), ("RECOMP_SAVE_DIR", "/elsewhere")],
                                      saveDir: save)
        expect(e["PATH"]!.hasPrefix("/opt/homebrew/bin:"))
        expectEq(e["HOME"], "/Users/x")
        expectEq(e["RECOMP_GPU"], "0", "recomp.cfg replaces the inherited value")
        expectEq(e["RECOMP_GPU_SCALE"], "3", "the title's file wins over the global default")
        expectEq(e["RECOMP_ASPECT"], "16:9")
        expectEq(e["RECOMP_SAVE_DIR"], "/tmp/x y/save")
    }

    check("settings: defaults, round trip, broken file") {
        let d = tempDir("settings")
        let f = d.appendingPathComponent("settings.json")
        var s = Settings.load(from: f)
        expectEq(s, Settings())
        expectEq(s.libraryFolders, ["~/Documents/xbox"])
        s.toolkitPath = "/Users/x/xboxrecomp-android"
        s.libraryFolders = ["~/Documents/xbox", "/Volumes/Games", "~/Documents/xbox"]
        s.workRoot = "/Volumes/Big/work"
        s.globalDefaults = ["RECOMP_ASPECT": "4:3"]
        try s.save(to: f)
        expect(!FileManager.default.fileExists(atPath: f.path + ".tmp"))
        let back = Settings.load(from: f)
        expectEq(back, s)
        expectEq(back.libraryURLs.count, 2, "duplicates dropped")
        expectEq(back.libraryURLs.first?.path, NSHomeDirectory() + "/Documents/xbox")
        expect(read(f).contains("\"library_folders\""))
        write(f, "{\"toolkit_path\": ")
        expectEq(Settings.load(from: f), Settings())
        // Unknown keys are tolerated, missing ones defaulted.
        write(f, "{\"toolkit_path\": \"/t\", \"future\": 3}")
        let partial = Settings.load(from: f)
        expectEq(partial.toolkitPath, "/t")
        expectEq(partial.libraryFolders, ["~/Documents/xbox"])
    }

    check("paths: XRECOMP_HOME, title layout, work root") {
        let p = AppPaths.standard(environment: ["XRECOMP_HOME": "/tmp/xr"])
        expectEq(p.root.path, "/tmp/xr")
        let t = p.title("4d530004")
        expectEq(t.dir.path, "/tmp/xr/titles/4D530004")
        expectEq(t.work.path, "/tmp/xr/titles/4D530004/work")
        expectEq(t.recompCfg.path, "/tmp/xr/titles/4D530004/recomp.cfg")
        expectEq(t.build.path, "/tmp/xr/titles/4D530004/work/project/build")
        var s = Settings()
        s.workRoot = "/Volumes/Big/xbox-work"
        expectEq(s.paths(base: p).title("4D530004").work.path, "/Volumes/Big/xbox-work/4D530004")
        expect(AppPaths.standard(environment: [:]).root.path.hasSuffix("Library/Application Support/xboxrecomp"))
    }

    check("python: version parsing and candidates order") {
        expect(PythonEnv.parseVersion("Python 3.12.4")! == (3, 12))
        expect(PythonEnv.isRecentEnough((3, 10)))
        expect(!PythonEnv.isRecentEnough((3, 9)))
        expect(PythonEnv.isRecentEnough((4, 0)))
        expectNil(PythonEnv.parseVersion("nope"))
        let tk = fakeToolkit(tempDir("tkpy")), home = tempDir("home")
        let venvPy = tk.appendingPathComponent(".venv/bin/python3"), appPy = home.appendingPathComponent("venv/bin/python3")
        write(venvPy, "#!/bin/sh\n", executable: true)
        write(appPy, "#!/bin/sh\n", executable: true)
        let c = PythonEnv.candidates(settingPath: nil, toolkit: tk, appVenv: home.appendingPathComponent("venv"))
        expectEq(c.map(\.path), [venvPy.path, appPy.path])
        // The toolkit's venv without the packages: the app's venv is taken.
        let r = PythonEnv.resolve(settingPath: nil, toolkit: tk, appVenv: home.appendingPathComponent("venv"),
                                  check: { $0.path == appPy.path })
        expectEq(r?.path, appPy.path)
    }

    check("dependencies: missing items carry their fix") {
        let deps = DependencyCheck.run(python: nil) { exe, args in
            switch exe {
            case "cmake": return Shell.Result(status: 0, output: "cmake version 3.31.0\n\nCMake suite")
            case "pkg-config": return Shell.Result(status: args.last == "sdl2" ? 0 : 1, output: "2.32.0\n")
            case "ninja": return Shell.Result(status: 127, output: "")
            default: return Shell.Result(status: 0, output: "ok 1.0")
            }
        }
        let byId = Dictionary(uniqueKeysWithValues: deps.map { ($0.id, $0) })
        expectEq(byId["cmake"]?.detail, "cmake version 3.31.0")
        expectEq(byId["sdl2"]?.ok, true)
        expectEq(byId["epoxy"]?.ok, false)
        expectEq(byId["epoxy"]?.fix, "brew install libepoxy")
        expectEq(byId["ninja"]?.optional, true)
        expectEq(byId["python"]?.ok, false)
        expect(!DependencyCheck.allRequiredOK(deps))
        let fine = deps.map { Dependency(id: $0.id, name: $0.name, ok: $0.optional ? false : true,
                                         optional: $0.optional, detail: "", fix: "") }
        expect(DependencyCheck.allRequiredOK(fine), "a missing optional item does not block")
    }

    check("shell: output, status, timeout") {
        let r = Shell.run("/bin/sh", ["-c", "echo hi; echo err >&2; exit 3"])
        expectEq(r.status, 3)
        expect(r.output.contains("hi") && r.output.contains("err"))
        let t0 = Date()
        let slow = Shell.run("/bin/sleep", ["5"], timeout: 0.3)
        expectEq(slow.status, -2)
        expect(Date().timeIntervalSince(t0) < 4)
        expectEq(Shell.run("/nonexistent/x", []).status, -1)
    }
}
