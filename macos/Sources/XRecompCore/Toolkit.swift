import Foundation

/// The repository checkout the launcher drives. It is not bundled: prepare needs the whole tree (tools, templates,
/// profiles, src/ for CMake) and stamps titles with its git commit.
public struct Toolkit: Equatable, Sendable {
    public let root: URL
    public let source: Source

    public enum Source: String, Sendable, Codable {
        case setting, environment, infoPlist, bundle
    }

    /// A folder is a toolkit when it holds `tools/prepare/__main__.py` and `templates/new-game/`.
    public static func isValid(_ url: URL) -> Bool {
        let fm = FileManager.default
        return fm.fileExists(atPath: url.appendingPathComponent("tools/prepare/__main__.py").path)
            && fm.isDirectory(url.appendingPathComponent("templates/new-game"))
    }

    /// Finds the toolkit, in order: the setting, `$XBA_TOOLKIT`, the `XBAToolkitPath` that build-app.sh writes into
    /// Info.plist, then the folders above the executable (a `swift run` from macos/, or an app inside the
    /// checkout). An invalid candidate is skipped. nil: the Setup screen asks.
    public static func resolve(setting: String?, environment: [String: String], infoPlistPath: String?,
                               executable: URL?) -> Toolkit? {
        func url(_ s: String?) -> URL? {
            guard let s, !s.isEmpty else { return nil }
            return URL(fileURLWithPath: ((s as NSString).expandingTildeInPath as NSString).standardizingPath,
                       isDirectory: true)
        }
        let ordered: [(URL?, Source)] = [(url(setting), .setting), (url(environment["XBA_TOOLKIT"]), .environment),
                                         (url(infoPlistPath), .infoPlist)]
        for (u, src) in ordered {
            if let u, isValid(u) { return Toolkit(root: u, source: src) }
        }
        if var dir = executable?.resolvingSymlinksInPath().deletingLastPathComponent() {
            for _ in 0..<8 {
                if isValid(dir) { return Toolkit(root: dir, source: .bundle) }
                let up = dir.deletingLastPathComponent()
                if up.path == dir.path { break }
                dir = up
            }
        }
        return nil
    }

    /// The toolkit in this process: Settings, environment, main bundle.
    public static func resolveForApp(settings: Settings) -> Toolkit? {
        resolve(setting: settings.toolkitPath, environment: ProcessInfo.processInfo.environment,
                infoPlistPath: Bundle.main.object(forInfoDictionaryKey: "XBAToolkitPath") as? String,
                executable: Bundle.main.executableURL)
    }

    /// `git rev-parse HEAD` of the checkout (what prepare stamps titles with), else tools/VERSION, else nil.
    public func commit() -> String? {
        let r = Shell.run("/usr/bin/env", ["git", "rev-parse", "HEAD"], cwd: root,
                          environment: ChildEnvironment.base(), timeout: 10)
        if r.status == 0 {
            let s = r.output.trimmingCharacters(in: .whitespacesAndNewlines)
            if !s.isEmpty { return s }
        }
        if let v = try? String(contentsOf: root.appendingPathComponent("tools/VERSION"), encoding: .utf8) {
            let s = v.trimmingCharacters(in: .whitespacesAndNewlines)
            return s.isEmpty ? nil : s
        }
        return nil
    }

    /// The licence texts the About screen lists: LICENSE, NOTICE, LICENSES/*.txt.
    public func licenceFiles() -> [URL] {
        var out: [URL] = []
        for n in ["LICENSE", "NOTICE"] {
            let u = root.appendingPathComponent(n)
            if FileManager.default.fileExists(atPath: u.path) { out.append(u) }
        }
        let dir = root.appendingPathComponent("LICENSES")
        let names = (try? FileManager.default.contentsOfDirectory(atPath: dir.path)) ?? []
        out += names.filter { $0.hasSuffix(".txt") }.sorted().map { dir.appendingPathComponent($0) }
        return out
    }
}
