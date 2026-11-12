import Foundation

/// « Import saves from command-line runs »: a title run from the terminal kept its saves in the runtime's default
/// save dir (`$XDG_DATA_HOME/xboxrecomp`, else `~/.local/share/xboxrecomp`: UDATA/<TID>, TDATA/<TID>). The launcher
/// gives each title its own RECOMP_SAVE_DIR, so those are copied in once. Nothing is overwritten.
public enum SaveImport {
    public static func defaultSaveDir(environment: [String: String] = ProcessInfo.processInfo.environment) -> URL {
        if let x = environment["XDG_DATA_HOME"], !x.isEmpty {
            return URL(fileURLWithPath: x).appendingPathComponent("xboxrecomp", isDirectory: true)
        }
        return URL(fileURLWithPath: NSHomeDirectory()).appendingPathComponent(".local/share/xboxrecomp", isDirectory: true)
    }

    /// The folders that would be imported (those that exist and are not in the title's save folder yet).
    public static func candidates(titleId: String, from: URL, to save: URL) -> [(URL, URL)] {
        ["UDATA", "TDATA"].compactMap { kind in
            let src = from.appendingPathComponent("\(kind)/\(titleId.uppercased())")
            let alt = from.appendingPathComponent("\(kind)/\(titleId.lowercased())")
            let s = FileManager.default.isDirectory(src) ? src : (FileManager.default.isDirectory(alt) ? alt : nil)
            guard let s else { return nil }
            let dst = save.appendingPathComponent("\(kind)/\(s.lastPathComponent)")
            return FileManager.default.fileExists(atPath: dst.path) ? nil : (s, dst)
        }
    }

    /// Copies the candidates; the number of folders copied.
    @discardableResult
    public static func run(titleId: String, from: URL = defaultSaveDir(), to save: URL) throws -> Int {
        let c = candidates(titleId: titleId, from: from, to: save)
        for (s, d) in c {
            try FileManager.default.ensureDirectory(d.deletingLastPathComponent())
            try FileManager.default.copyItem(at: s, to: d)
        }
        return c.count
    }
}
