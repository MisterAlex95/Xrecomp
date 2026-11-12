import Foundation

/// The Python prepare runs with: one that imports the pipeline's packages (capstone, pefile, numpy).
///
/// Order: the player's choice (Settings.pythonPath); `<toolkit>/.venv/bin/python3`; the launcher's own venv in
/// Application Support. Setup creates that venv from the first Python 3.10 or later it finds and pip-installs the
/// packages; the system's /usr/bin/python3 (3.9) is too old.
public enum PythonEnv {
    public static let packages = ["capstone", "pefile", "numpy"]
    public static let minimum = (3, 10)

    /// The candidates, in order, that exist on disk.
    public static func candidates(settingPath: String?, toolkit: URL?, appVenv: URL) -> [URL] {
        var out: [URL] = []
        if let s = settingPath, !s.isEmpty { out.append(URL(fileURLWithPath: (s as NSString).expandingTildeInPath)) }
        if let t = toolkit { out.append(t.appendingPathComponent(".venv/bin/python3")) }
        out.append(appVenv.appendingPathComponent("bin/python3"))
        return out.filter { FileManager.default.isExecutableFile(atPath: $0.path) }
    }

    /// True when `python` imports every package.
    public static func hasPackages(_ python: URL) -> Bool {
        Shell.run(python.path, ["-c", "import " + packages.joined(separator: ", ")], timeout: 60).ok
    }

    /// The first candidate that imports the packages, or nil (Setup then offers to make the venv).
    public static func resolve(settingPath: String?, toolkit: URL?, appVenv: URL,
                               check: (URL) -> Bool = hasPackages) -> URL? {
        candidates(settingPath: settingPath, toolkit: toolkit, appVenv: appVenv).first(where: check)
    }

    /// "Python 3.12.4" -> (3, 12).
    public static func parseVersion(_ text: String) -> (Int, Int)? {
        guard let r = text.range(of: #"(\d+)\.(\d+)"#, options: .regularExpression) else { return nil }
        let parts = text[r].split(separator: ".").compactMap { Int($0) }
        return parts.count == 2 ? (parts[0], parts[1]) : nil
    }

    public static func isRecentEnough(_ v: (Int, Int)) -> Bool {
        v.0 > minimum.0 || (v.0 == minimum.0 && v.1 >= minimum.1)
    }

    /// Interpreters to make the venv from: Homebrew's versioned ones (newest first), /usr/local, then PATH.
    public static func baseCandidates(searchDirs: [String] = ["/opt/homebrew/bin", "/usr/local/bin"],
                                      path: String = ProcessInfo.processInfo.environment["PATH"] ?? "") -> [String] {
        var out: [String] = []
        for dir in searchDirs {
            for minor in stride(from: 14, through: 10, by: -1) {
                let p = "\(dir)/python3.\(minor)"
                if FileManager.default.isExecutableFile(atPath: p) { out.append(p) }
            }
            let p = "\(dir)/python3"
            if FileManager.default.isExecutableFile(atPath: p) { out.append(p) }
        }
        for dir in path.split(separator: ":") {
            let p = "\(dir)/python3"
            if FileManager.default.isExecutableFile(atPath: p), !out.contains(p) { out.append(p) }
        }
        return out
    }

    /// The first Python 3.10 or later, or nil.
    public static func findBase() -> String? {
        for p in baseCandidates() {
            let r = Shell.run(p, ["--version"], timeout: 10)
            if r.ok, let v = parseVersion(r.output), isRecentEnough(v) { return p }
        }
        return nil
    }

    /// Makes `venv` from `base` and installs the packages; `log` gets each line of output. Blocking (minutes).
    public static func createVenv(at venv: URL, base: String, log: (String) -> Void) -> Bool {
        log("$ \(base) -m venv \(venv.path)")
        let r1 = Shell.run(base, ["-m", "venv", venv.path], timeout: 300)
        r1.output.split(separator: "\n").forEach { log(String($0)) }
        guard r1.ok else { return false }
        let py = venv.appendingPathComponent("bin/python3").path
        log("$ \(py) -m pip install \(packages.joined(separator: " "))")
        let r2 = Shell.run(py, ["-m", "pip", "install", "--disable-pip-version-check"] + packages, timeout: 1200)
        r2.output.split(separator: "\n").forEach { log(String($0)) }
        return r2.ok
    }
}
