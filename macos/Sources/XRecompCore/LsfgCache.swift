import Foundation

/// The LSFG shader cache frame generation reads (docs/technical/framegen.md « Shader cache »,
/// src/framegen/lsfg_cache.h), filled from the player's own Lossless.dll by the toolkit's `lsfg_translate`
/// (tools/lsfg). The launcher builds that tool once, runs it on the DLL the player picks and reads the manifest for
/// the Settings screen; the runtime finds the cache by itself (`<base>/current`, RECOMP_LSFG_DIR unset).
///
/// Nothing of the DLL or the cache is ever copied anywhere else: the files belong to the player.
public enum LsfgCache {
    /// The Lossless.dll resource IDs each shader set needs (lsfg_spv.c's table): LSFG 3.1, 3.1 performance.
    public static let ids31: [Int] = Array(255...279)
    public static let idsPerf: [Int] = [255, 256] + Array(280...302)

    /// What a cache directory holds, from its manifest.txt.
    public struct Status: Equatable, Sendable {
        public var dir: URL?
        /// The first 16 hex digits of the DLL's SHA-256 (the directory's name).
        public var key: String?
        public var dllSize: Int64 = 0
        /// The shader set is complete in one form (translated, or SPIR-V the DLL carries).
        public var ready31 = false
        public var readyPerf = false
        public var translator: String?

        public init() {}

        public var ready: Bool { ready31 || readyPerf }

        /// One line for Settings.
        public var summary: String {
            guard let key else { return "No Lossless.dll imported yet." }
            let sets = [ready31 ? "LSFG 3.1" : nil, readyPerf ? "3.1 performance" : nil].compactMap { $0 }
            if sets.isEmpty { return "Lossless.dll \(key)…: shaders incomplete (choose it again)." }
            return "Lossless.dll \(key)… ready: \(sets.joined(separator: " and "))."
        }
    }

    /// The desktop cache base, as lsfg_cache_default_base: $RECOMP_LSFG_CACHE, else ~/Library/Caches/xboxrecomp/lsfg.
    public static func defaultBase(environment: [String: String] = ProcessInfo.processInfo.environment) -> URL {
        if let c = environment["RECOMP_LSFG_CACHE"], !c.isEmpty {
            return URL(fileURLWithPath: (c as NSString).expandingTildeInPath, isDirectory: true)
        }
        let home = environment["HOME"].flatMap { $0.isEmpty ? nil : $0 } ?? NSHomeDirectory()
        return URL(fileURLWithPath: home, isDirectory: true)
            .appendingPathComponent("Library/Caches/xboxrecomp/lsfg", isDirectory: true)
    }

    /// Reads a manifest (lsfg_cache_read_manifest): nil when it is not version 1 or has no DLL hash.
    public static func parseManifest(_ text: String) -> Status? {
        var version = 0
        var sha = ""
        var lists: [String: Set<Int>] = [:]
        var s = Status()
        for raw in text.split(whereSeparator: { $0 == "\n" || $0 == "\r" }) {
            let line = String(raw)
            guard let sp = line.firstIndex(of: " ") else { continue }
            let k = String(line[..<sp])
            let v = String(line[line.index(after: sp)...])
            switch k {
            case "lsfg-shader-cache": version = Int(v.trimmingCharacters(in: .whitespaces)) ?? 0
            case "dll_sha256": sha = v.trimmingCharacters(in: .whitespaces)
            case "dll_size": s.dllSize = Int64(v.trimmingCharacters(in: .whitespaces)) ?? 0
            case "translator": s.translator = v.trimmingCharacters(in: .whitespaces)
            case "dxbc", "fp16", "fp32", "spv":
                lists[k] = Set(v.split(separator: " ").compactMap { Int($0) })
            default: break
            }
        }
        guard version == 1, sha.count == 64 else { return nil }
        s.key = String(sha.prefix(16))
        func complete(_ ids: [Int]) -> Bool {
            ["fp16", "fp32", "spv"].contains { form in
                let have = lists[form] ?? []
                return ids.allSatisfy { have.contains($0) }
            }
        }
        s.ready31 = complete(ids31)
        s.readyPerf = complete(idsPerf)
        return s
    }

    /// The active cache of a base (`<base>/current` names it); an empty Status when there is none.
    public static func status(base: URL) -> Status {
        guard let cur = try? String(contentsOf: base.appendingPathComponent("current"), encoding: .utf8) else {
            return Status()
        }
        let key = cur.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !key.isEmpty, !key.contains("/") else { return Status() }
        let dir = base.appendingPathComponent(key, isDirectory: true)
        guard let text = try? String(contentsOf: dir.appendingPathComponent("manifest.txt"), encoding: .utf8),
              var s = parseManifest(text) else { return Status() }
        s.dir = dir
        return s
    }

    // ---- the translator ------------------------------------------------------------------------------------------

    /// Where the launcher builds `lsfg_translate` (Application Support, outside the toolkit).
    public static func toolBuildDir(appRoot: URL) -> URL {
        appRoot.appendingPathComponent("lsfg-tool", isDirectory: true)
    }

    /// Where a `lsfg_translate` may already be: the documented manual build in the toolkit
    /// (`cmake -S tools/lsfg -B build/lsfg`), then the launcher's own.
    public static func toolCandidates(toolkit: URL, appRoot: URL) -> [URL] {
        [toolkit.appendingPathComponent("build/lsfg/lsfg_translate"),
         toolBuildDir(appRoot: appRoot).appendingPathComponent("lsfg_translate")]
    }

    public static func findTool(toolkit: URL, appRoot: URL) -> URL? {
        toolCandidates(toolkit: toolkit, appRoot: appRoot).first { FileManager.default.isExecutableFile(atPath: $0.path) }
    }

    /// The two cmake runs that build the tool (arguments after `cmake`). The pinned DXVK archive comes from the
    /// toolkit's third_party/lsfg-sources (the published toolkit carries it) or is downloaded once by CMake.
    public static func buildCommands(toolkit: URL, buildDir: URL, ninja: Bool) -> [[String]] {
        var configure = ["-S", toolkit.appendingPathComponent("tools/lsfg").path, "-B", buildDir.path,
                         "-DCMAKE_BUILD_TYPE=Release"]
        if ninja { configure += ["-G", "Ninja"] }
        return [configure, ["--build", buildDir.path, "--target", "lsfg_translate"]]
    }

    /// The tool's arguments for a DLL: extract and translate into base, made current.
    public static func translateArguments(dll: URL, base: URL) -> [String] {
        ["-o", base.path, dll.path]
    }

    /// The tool's error line (`lsfg_translate: ...`), else the output's last non-empty line.
    public static func errorLine(_ output: String) -> String {
        let lines = output.split(separator: "\n").map { $0.trimmingCharacters(in: .whitespaces) }.filter { !$0.isEmpty }
        return lines.last { $0.hasPrefix("lsfg_translate:") } ?? lines.last ?? "no output"
    }

    public struct ImportResult: Sendable {
        public let ok: Bool
        public let status: Status
        /// What went wrong, in words (nil when ok).
        public let message: String?
        /// The commands' output, for the Settings screen's log.
        public let log: [String]
    }

    /// Builds the tool when needed, then translates the DLL into base. Blocking (minutes the first time: CMake and
    /// DXVK's compiler); `progress` gets « Building the shader translator… » / « Translating the shaders… ».
    public static func importDll(_ dll: URL, toolkit: URL, appRoot: URL, base: URL,
                                 environment: [String: String] = ChildEnvironment.base(),
                                 progress: (String) -> Void = { _ in }) -> ImportResult {
        var log: [String] = []
        func fail(_ m: String) -> ImportResult {
            ImportResult(ok: false, status: status(base: base), message: m, log: log)
        }
        guard FileManager.default.fileExists(atPath: dll.path) else { return fail("\(dll.path) does not exist.") }
        var tool = findTool(toolkit: toolkit, appRoot: appRoot)
        if tool == nil {
            guard FileManager.default.fileExists(atPath: toolkit.appendingPathComponent("tools/lsfg/CMakeLists.txt").path)
            else { return fail("This toolkit has no tools/lsfg: update it.") }
            progress("Building the shader translator (once, a few minutes)…")
            let dir = toolBuildDir(appRoot: appRoot)
            let ninja = Shell.which("ninja", path: environment["PATH"] ?? ChildEnvironment.path(inherited: nil)) != nil
            for args in buildCommands(toolkit: toolkit, buildDir: dir, ninja: ninja) {
                log.append("$ cmake " + args.joined(separator: " "))
                let r = Shell.run("/usr/bin/env", ["cmake"] + args, cwd: toolkit, environment: environment,
                                  timeout: 1800)
                log += r.output.split(separator: "\n").map(String.init)
                if !r.ok {
                    return fail("Building lsfg_translate failed (exit \(r.status)): \(errorLine(r.output))")
                }
            }
            tool = findTool(toolkit: toolkit, appRoot: appRoot)
            guard tool != nil else { return fail("The build ended without lsfg_translate.") }
        }
        progress("Translating the shaders…")
        try? FileManager.default.ensureDirectory(base)
        let args = translateArguments(dll: dll, base: base)
        log.append("$ \(tool!.path) " + args.joined(separator: " "))
        let r = Shell.run(tool!.path, args, environment: environment, timeout: 600)
        log += r.output.split(separator: "\n").map(String.init)
        let s = status(base: base)
        if !r.ok || !s.ready {
            let line = errorLine(r.output)
            let m = line.hasPrefix("lsfg_translate:") ? line : "lsfg_translate failed (exit \(r.status)): \(line)"
            return ImportResult(ok: false, status: s, message: m, log: log)
        }
        return ImportResult(ok: true, status: s, message: nil, log: log)
    }
}

/// The Vulkan loader frame generation dlopens on macOS (MoltenVK, directly or through the Khronos loader).
public enum VulkanLoader {
    public static let names = ["libvulkan.1.dylib", "libvulkan.dylib", "libMoltenVK.dylib"]
    public static let dirs = ["/opt/homebrew/lib", "/usr/local/lib"]

    /// The first loader found in dirs (and ~/lib), or nil.
    public static func find(dirs: [String] = VulkanLoader.dirs, home: String? = NSHomeDirectory()) -> URL? {
        let all = (home.map { ["\($0)/lib"] } ?? []) + dirs
        for d in all {
            for n in names where FileManager.default.fileExists(atPath: "\(d)/\(n)") {
                return URL(fileURLWithPath: "\(d)/\(n)")
            }
        }
        return nil
    }

    /// DYLD_FALLBACK_LIBRARY_PATH for a frame-generation run: dyld's default with Homebrew's folders added.
    public static func fallbackPath(home: String?) -> String {
        var parts: [String] = []
        if let home, !home.isEmpty { parts.append("\(home)/lib") }
        parts += ["/opt/homebrew/lib", "/usr/local/lib", "/usr/lib"]
        return parts.joined(separator: ":")
    }
}
