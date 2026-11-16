import Foundation
import XRecompCore

/// XBA-196: per-game settings over Default settings (GameSettings, the Android XBA-188 semantics), the options the
/// desktop Xbox runtime reads, frame generation's shader cache (LsfgCache) and its Vulkan loader.

/// A manifest as lsfg_cache.c writes it: version 1, a 64-digit hash, the given ids per list.
func lsfgManifest(sha: String = String(repeating: "ab", count: 32), spv: [Int] = [], fp16: [Int] = [],
                  dxbc: [Int] = []) -> String {
    func ids(_ a: [Int]) -> String { a.map(String.init).joined(separator: " ") }
    return "lsfg-shader-cache 1\nextractor 1\ndll_sha256 \(sha)\ndll_size 1234567\nrcdata 202\n"
        + "dxbc \(ids(dxbc))\nfp16 \(ids(fp16))\nfp32 \nspv \(ids(spv))\ntranslator dxvk-2.6.2\n"
}

func registerSettingsParityChecks() {
    check("game settings: follow Default settings, own value, KEY= is the runtime default") {
        let gpu = OptionTable.option("gpu")!, shader = OptionTable.option("shader")!
        var defaults = RecompConfig()
        defaults.set("RECOMP_GPU", "1")
        defaults.set("RECOMP_SHADER", "crt")
        var g = GameSettings(game: RecompConfig(), defaults: defaults)
        expect(g.follows(gpu) && g.allFollow())
        expectEq(g.effective("RECOMP_GPU"), "1")
        expectEq(g.effectiveChoiceIndex(gpu), 1)
        expectNil(g.ownChoiceIndex(gpu), "following: no own choice")
        expectEq(g.defaultsLabel(gpu), "GPU (OpenGL)")
        // The runtime default (CPU) as the game's own: KEY=, even though Default settings say GPU.
        g.setOwn(gpu, gpu.choices[0])
        expectEq(g.game.get("RECOMP_GPU"), "")
        expect(!g.follows(gpu) && !g.allFollow())
        expectNil(g.effective("RECOMP_GPU"))
        expectEq(g.ownChoiceIndex(gpu), 0)
        g.setOwn(shader, shader.choices[2])
        expectEq(g.game.get("RECOMP_SHADER"), "scanlines")
        expectEq(g.ownChoiceIndex(shader), 2)
        g.follow(gpu)
        expectNil(g.game.get("RECOMP_GPU"), "follow removes the line")
        expectEq(g.effective("RECOMP_GPU"), "1")
        g.followAll()
        expect(g.allFollow())
        expectEq(g.game.lines.filter { RecompConfig.keyOf($0) != nil }, [])
        // An unknown value in Default settings: « Custom ».
        var odd = RecompConfig()
        odd.set("RECOMP_GPU_SCALE", "1.5")
        expectEq(GameSettings(game: RecompConfig(), defaults: odd).defaultsLabel(OptionTable.option("gpu-scale")!),
                 "Custom")
    }

    check("game settings: several keys, own when any is set, the others keep Default settings'") {
        let fg = OptionTable.option("framegen")!
        var defaults = RecompConfig()
        defaults.set(fg, fg.choices[2])   // 2x performance
        var g = GameSettings(game: RecompConfig(), defaults: defaults)
        expectEq(g.effectiveChoiceIndex(fg), 2)
        g.setOwn(fg, fg.choices[1])       // 2x: RECOMP_LSFG_PERF= over Default settings' 1
        expectEq(g.game.get("RECOMP_LSFG"), "1")
        expectEq(g.game.get("RECOMP_LSFG_MULT"), "2")
        expectEq(g.game.get("RECOMP_LSFG_PERF"), "")
        expectEq(g.ownChoiceIndex(fg), 1)
        var partial = RecompConfig()
        partial.set("RECOMP_LSFG", "")    // off for this game; MULT / PERF unset: Default settings'
        let p = GameSettings(game: partial, defaults: defaults)
        expect(!p.follows(fg))
        expectEq(p.effective("RECOMP_LSFG_PERF"), "1")
        expectNil(p.ownChoiceIndex(fg), "off + 2 + perf is no choice of the list: Custom")
        g.setOwn(fg, fg.choices[0])
        expectEq(g.game.get("RECOMP_LSFG_MULT"), "")
        expectEq(g.ownChoiceIndex(fg), 0)
    }

    check("cfg: an empty line counts as the default choice") {
        var c = RecompConfig()
        c.set("RECOMP_GPU", "")
        expectEq(c.choiceIndex(OptionTable.option("gpu")!), 0)
        expect(c.allDefault())
        expectEq(c.activePairs.map { "\($0.key)=\($0.value)" }, ["RECOMP_GPU="])
    }

    check("child env: KEY= unsets, also an inherited or Default settings value") {
        let e = ChildEnvironment.game(inherited: ["PATH": "/usr/bin", "RECOMP_GPU": "1", "RECOMP_SHADER": "crt"],
                                      globalDefaults: ["RECOMP_ASPECT": "16:9", "RECOMP_GPU_SCALE": "2"],
                                      config: [("RECOMP_GPU", ""), ("RECOMP_GPU_SCALE", ""), ("RECOMP_SHADER", ""),
                                               ("PATH", "")],
                                      saveDir: nil)
        expectNil(e["RECOMP_GPU"])
        expectNil(e["RECOMP_GPU_SCALE"])
        expectNil(e["RECOMP_SHADER"])
        expectEq(e["RECOMP_ASPECT"], "16:9")
        expect(e["PATH"]!.hasPrefix("/opt/homebrew/bin:"), "PATH cannot be unset from the file")
        expectNil(e["DYLD_FALLBACK_LIBRARY_PATH"], "no frame generation: no dyld path")
    }

    check("child env: frame generation adds Homebrew's lib folders to dyld's fallback, never over the player's") {
        let on = ChildEnvironment.game(inherited: ["PATH": "/usr/bin", "HOME": "/Users/x"], globalDefaults: [:],
                                       config: [("RECOMP_LSFG", "1"), ("RECOMP_LSFG_MULT", "2")], saveDir: nil)
        expectEq(on["DYLD_FALLBACK_LIBRARY_PATH"], "/Users/x/lib:/opt/homebrew/lib:/usr/local/lib:/usr/lib")
        let mine = ChildEnvironment.game(inherited: ["DYLD_FALLBACK_LIBRARY_PATH": "/my/lib"],
                                         globalDefaults: ["RECOMP_LSFG": "1"], config: [], saveDir: nil)
        expectEq(mine["DYLD_FALLBACK_LIBRARY_PATH"], "/my/lib")
        let off = ChildEnvironment.game(inherited: [:], globalDefaults: ["RECOMP_LSFG": "1"],
                                        config: [("RECOMP_LSFG", "")], saveDir: nil)
        expectNil(off["RECOMP_LSFG"])
        expectNil(off["DYLD_FALLBACK_LIBRARY_PATH"], "the game's own Off wins")
    }

    check("options: the desktop Xbox rows, keys and values as the runtime and the Android table") {
        let byId = Dictionary(uniqueKeysWithValues: OptionTable.all.map { ($0.id, $0) })
        expectEq(byId["framegen"]?.keys, ["RECOMP_LSFG", "RECOMP_LSFG_MULT", "RECOMP_LSFG_PERF"])
        expectEq(byId["framegen"]?.choices.map(\.values), [[nil, nil, nil], ["1", "2", nil], ["1", "2", "1"]])
        expectEq(byId["widescreen"]?.keys, ["RECOMP_WIDESCREEN"])
        expectEq(byId["480p"]?.keys, ["RECOMP_480P"])
        expectEq(byId["rumble"]?.choices.map(\.values), [[nil], ["0"]])
        // present_shader_parse's names; LCD grid left out (it draws nothing on the Xbox picture).
        expectEq(byId["shader"]?.choices.compactMap { $0.values[0] }, ["sharp", "scanlines", "crt", "fxaa", "cas"])
        expectEq(byId["shader-curvature"]?.onlyWhen?.value, "crt")
        expectEq(Set(OptionTable.all.map(\.id)).count, OptionTable.all.count, "ids unique")
        let keys = OptionTable.all.flatMap(\.keys)
        expectEq(Set(keys).count, keys.count, "each key in one row")
        for k in ["RECOMP_BIG_CORES", "RECOMP_SHELL_SELFTEST", "RECOMP_UPSCALE", "RECOMP_LSFG_DIR"] {
            expect(!keys.contains(k), k)
        }
    }

    // ---- frame generation's shader cache ---------------------------------------------------------------------------
    check("lsfg: manifest, sets complete in one form") {
        expectNil(LsfgCache.parseManifest("lsfg-shader-cache 2\ndll_sha256 \(String(repeating: "0", count: 64))\n"))
        expectNil(LsfgCache.parseManifest("lsfg-shader-cache 1\ndll_sha256 abc\n"))
        let all = LsfgCache.parseManifest(lsfgManifest(spv: LsfgCache.ids31 + LsfgCache.idsPerf))!
        expect(all.ready31 && all.readyPerf && all.ready)
        expectEq(all.key, String(repeating: "ab", count: 8))
        expectEq(all.dllSize, 1234567)
        expectEq(all.translator, "dxvk-2.6.2")
        let only31 = LsfgCache.parseManifest(lsfgManifest(spv: LsfgCache.ids31))!
        expect(only31.ready31 && !only31.readyPerf)
        // Mixed forms do not make a set: 3.1 half translated, half FP16.
        let mixed = LsfgCache.parseManifest(lsfgManifest(spv: Array(LsfgCache.ids31.prefix(10)),
                                                         fp16: Array(LsfgCache.ids31.dropFirst(10))))!
        expect(!mixed.ready)
        let dxbcOnly = LsfgCache.parseManifest(lsfgManifest(dxbc: LsfgCache.ids31 + LsfgCache.idsPerf))!
        expect(!dxbcOnly.ready, "DXBC alone needs a translation")
        expect(dxbcOnly.summary.contains("incomplete"))
        expectEq(LsfgCache.ids31.count, 25)
        expectEq(Set(LsfgCache.idsPerf).count, 25)
        expect(LsfgCache.Status().summary.contains("No Lossless.dll"))
    }

    check("lsfg: base, current, status") {
        expectEq(LsfgCache.defaultBase(environment: ["HOME": "/Users/x"]).path, "/Users/x/Library/Caches/xboxrecomp/lsfg")
        expectEq(LsfgCache.defaultBase(environment: ["HOME": "/Users/x", "RECOMP_LSFG_CACHE": "/c/lsfg"]).path, "/c/lsfg")
        let base = tempDir("lsfg")
        expectEq(LsfgCache.status(base: base), LsfgCache.Status())
        let key = String(repeating: "ab", count: 8)
        write(base.appendingPathComponent("\(key)/manifest.txt"), lsfgManifest(spv: LsfgCache.idsPerf))
        expectNil(LsfgCache.status(base: base).key, "no current: nothing active")
        write(base.appendingPathComponent("current"), key + "\n")
        let s = LsfgCache.status(base: base)
        expect(s.readyPerf && !s.ready31)
        expectEq(s.dir?.lastPathComponent, key)
        write(base.appendingPathComponent("current"), "../elsewhere\n")
        expectNil(LsfgCache.status(base: base).key)
    }

    check("lsfg: tool lookup and the cmake commands") {
        let tk = tempDir("tk"), app = tempDir("app")
        expectNil(LsfgCache.findTool(toolkit: tk, appRoot: app))
        write(app.appendingPathComponent("lsfg-tool/lsfg_translate"), "#!/bin/sh\n", executable: true)
        expectEq(LsfgCache.findTool(toolkit: tk, appRoot: app)?.path, app.appendingPathComponent("lsfg-tool/lsfg_translate").path)
        write(tk.appendingPathComponent("build/lsfg/lsfg_translate"), "#!/bin/sh\n", executable: true)
        expectEq(LsfgCache.findTool(toolkit: tk, appRoot: app)?.path, tk.appendingPathComponent("build/lsfg/lsfg_translate").path,
                 "the toolkit's own build first")
        let cmds = LsfgCache.buildCommands(toolkit: URL(fileURLWithPath: "/t"), buildDir: URL(fileURLWithPath: "/b"),
                                           ninja: true)
        expectEq(cmds, [["-S", "/t/tools/lsfg", "-B", "/b", "-DCMAKE_BUILD_TYPE=Release", "-G", "Ninja"],
                        ["--build", "/b", "--target", "lsfg_translate"]])
        expectEq(LsfgCache.translateArguments(dll: URL(fileURLWithPath: "/d/Lossless.dll"), base: URL(fileURLWithPath: "/c")),
                 ["-o", "/c", "/d/Lossless.dll"])
        expectEq(LsfgCache.errorLine("cache x\nlsfg_translate: not a PE file\n\n"), "lsfg_translate: not a PE file")
        expectEq(LsfgCache.errorLine("a\nb\n"), "b")
    }

    check("lsfg: import through a fake lsfg_translate") {
        let tk = tempDir("tk"), app = tempDir("app"), base = tempDir("base").appendingPathComponent("lsfg")
        let dll = tk.appendingPathComponent("Lossless.dll")
        write(dll, "not really a DLL")
        let key = String(repeating: "cd", count: 8)
        let manifest = tk.appendingPathComponent("manifest.in")
        write(manifest, lsfgManifest(sha: String(repeating: "cd", count: 32), spv: LsfgCache.ids31 + LsfgCache.idsPerf))
        // The fake: lsfg_translate -o BASE DLL writes BASE/<key>/manifest.txt and BASE/current.
        write(tk.appendingPathComponent("build/lsfg/lsfg_translate"), """
            #!/bin/sh
            [ "$1" = "-o" ] || { echo "lsfg_translate: bad args" >&2; exit 2; }
            mkdir -p "$2/\(key)" && cp "\(manifest.path)" "$2/\(key)/manifest.txt" && echo \(key) > "$2/current"
            echo "cache      $2/\(key)"
            """, executable: true)
        var steps: [String] = []
        let r = LsfgCache.importDll(dll, toolkit: tk, appRoot: app, base: base) { steps.append($0) }
        expect(r.ok, r.message ?? "")
        expect(r.status.ready31 && r.status.readyPerf)
        expectEq(r.status.key, key)
        expectEq(steps, ["Translating the shaders…"], "an existing tool is not rebuilt")
        expect(r.log.contains { $0.hasPrefix("cache") })

        write(tk.appendingPathComponent("build/lsfg/lsfg_translate"),
              "#!/bin/sh\necho 'lsfg_translate: this Lossless.dll version is not supported' >&2\nexit 1\n",
              executable: true)
        let bad = LsfgCache.importDll(dll, toolkit: tk, appRoot: app, base: base)
        expect(!bad.ok)
        expectEq(bad.message, "lsfg_translate: this Lossless.dll version is not supported")
        expect(bad.status.ready, "the earlier cache stays current")

        let missing = LsfgCache.importDll(tk.appendingPathComponent("nope.dll"), toolkit: tk, appRoot: app, base: base)
        expect(!missing.ok && missing.message!.contains("does not exist"))
        let noTools = LsfgCache.importDll(dll, toolkit: tempDir("empty"), appRoot: tempDir("app2"), base: base)
        expect(!noTools.ok && noTools.message!.contains("tools/lsfg"))
    }

    check("lsfg: Vulkan loader lookup, optional in Setup") {
        let d = tempDir("vk")
        expectNil(VulkanLoader.find(dirs: [d.path], home: nil))
        write(d.appendingPathComponent("libMoltenVK.dylib"), "")
        expectEq(VulkanLoader.find(dirs: [d.path], home: nil)?.lastPathComponent, "libMoltenVK.dylib")
        write(d.appendingPathComponent("libvulkan.1.dylib"), "")
        expectEq(VulkanLoader.find(dirs: [d.path], home: nil)?.lastPathComponent, "libvulkan.1.dylib", "the loader first")
        let deps = DependencyCheck.run(python: nil, vulkan: nil) { _, _ in Shell.Result(status: 0, output: "ok") }
        let vk = deps.first { $0.id == "vulkan" }
        expectEq(vk?.ok, false)
        expectEq(vk?.optional, true)
        expectEq(vk?.fix, "brew install molten-vk vulkan-loader")
    }
}
