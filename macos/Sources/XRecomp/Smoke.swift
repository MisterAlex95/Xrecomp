import Foundation
import XRecompCore

/// `XRecomp --smoke [--toolkit DIR] [--library DIR]... [--prepare [--no-build]]`: resolves the toolkit and Python,
/// scans the library (the given folders, else the settings'), prints it as JSON and exits without a window; 0 when
/// the toolkit was found. With --prepare it also runs the app's prepare path on every title found (PrepareRunner into
/// the title's work folder under XRECOMP_HOME, then prepare --check), the end-to-end test of docs/macos/launcher.md.
enum Smoke {
    static func values(_ args: [String], _ flag: String) -> [String] {
        var out: [String] = []
        var i = 0
        while i < args.count {
            if args[i] == flag, i + 1 < args.count { out.append(args[i + 1]); i += 1 }
            i += 1
        }
        return out
    }

    struct Report: Encodable {
        var toolkit: String?
        var toolkitSource: String?
        var python: String?
        var home: String
        var libraryFolders: [String]
        var library: [Item]
        var truncated: Bool
        var prepared: [Prepared]?

        struct Prepared: Encodable {
            var titleId: String
            var outcome: String
            var progressLines: Int
            var lastStep: String
            var state: String?
            var reasons: [String]
            var exe: String?
            var work: String

            enum CodingKeys: String, CodingKey {
                case titleId = "title_id", outcome, progressLines = "progress_lines", lastStep = "last_step", state
                case reasons, exe, work
            }
        }

        struct Item: Encodable {
            var titleId: String?
            var title: String
            var kind: String
            var path: String
            var problem: String?
            var duplicates: [String]
            var hasArt: Bool

            enum CodingKeys: String, CodingKey {
                case titleId = "title_id", title, kind, path, problem, duplicates, hasArt = "has_art"
            }
        }

        enum CodingKeys: String, CodingKey {
            case toolkit, toolkitSource = "toolkit_source", python, home, libraryFolders = "library_folders", library
            case truncated, prepared
        }
    }

    static func run(_ args: [String]) -> Int32 {
        let paths = AppPaths.standard()
        var settings = Settings.load(from: paths.settingsFile)
        if let t = values(args, "--toolkit").last { settings.toolkitPath = t }
        let libs = values(args, "--library")
        if !libs.isEmpty { settings.libraryFolders = libs }
        let tk = Toolkit.resolveForApp(settings: settings)
        let py = PythonEnv.resolve(settingPath: settings.pythonPath, toolkit: tk?.root, appVenv: paths.venv)

        var scanner = LibraryScanner { _ in .failure(.init("no Python with the pipeline's packages yet")) }
        if let tk, let py {
            scanner.identifyImage = LibraryScanner.pythonIdentify(python: py.path, toolkit: tk.root,
                                                                  scanDir: paths.scanDir)
        }
        var index = LibraryIndex.load(from: paths.libraryFile)
        let entries = scanner.scan(settings.libraryURLs, index: &index)
        try? index.save(to: paths.libraryFile)

        let report = Report(
            toolkit: tk?.root.path, toolkitSource: tk?.source.rawValue, python: py?.path, home: paths.root.path,
            libraryFolders: settings.libraryURLs.map(\.path),
            library: entries.map { e in
                Report.Item(titleId: e.titleId, title: e.displayName, kind: e.kind.rawValue, path: e.path,
                            problem: e.problem, duplicates: e.duplicates,
                            hasArt: e.xbe.map { TitleArt.decode(xbe: URL(fileURLWithPath: $0)) != nil } ?? false)
            },
            truncated: scanner.truncated)
        var report2 = report
        if args.contains("--prepare"), let tk, let py {
            report2.prepared = entries.compactMap { e in
                e.titleId.map { prepare(e, tid: $0, toolkit: tk.root, python: py, paths: paths,
                                        noBuild: args.contains("--no-build")) }
            }
        }
        let data = try! JSONEncoder.pretty.encode(report2)
        print(String(decoding: data, as: UTF8.self))
        return tk == nil ? 2 : (report2.prepared?.contains { !$0.outcome.hasPrefix("ok") } == true ? 1 : 0)
    }

    final class Collect: @unchecked Sendable {
        let lock = NSLock()
        var progress: [PrepareProtocol.Progress] = []
        var outcome: PrepareRunner.Outcome?
        let done = DispatchSemaphore(value: 0)
    }

    static func prepare(_ e: LibraryEntry, tid: String, toolkit: URL, python: URL, paths: AppPaths,
                        noBuild: Bool) -> Report.Prepared {
        let t = paths.title(tid)
        var req = PrepareRunner.Request(python: python.path, toolkit: toolkit, source: URL(fileURLWithPath: e.path),
                                        work: t.work)
        req.noBuild = noBuild
        let c = Collect()
        let runner = PrepareRunner(req) { ev in
            c.lock.lock()
            defer { c.lock.unlock() }
            switch ev {
            case .progress(let p): c.progress.append(p)
            case .line(let l): FileHandle.standardError.write(Data(("  " + l + "\n").utf8))
            case .finished(let o):
                c.outcome = o
                c.done.signal()
            }
        }
        do { try runner.start() } catch {
            return Report.Prepared(titleId: tid, outcome: "error: \(error)", progressLines: 0, lastStep: "", state: nil,
                                   reasons: [], exe: nil, work: t.work.path)
        }
        c.done.wait()
        let st = TitleState.check(python: python.path, toolkit: toolkit, source: URL(fileURLWithPath: e.path), work: t.work)
        let outcome: String
        switch c.outcome {
        case .ok(let exe)?: outcome = "ok" + (exe.map { " exe=\($0)" } ?? "")
        case .failed(let code, let msg)?: outcome = "failed \(code): \(msg)"
        case .cancelled?: outcome = "cancelled"
        case nil: outcome = "none"
        }
        return Report.Prepared(titleId: tid, outcome: outcome, progressLines: c.progress.count,
                               lastStep: c.progress.last.map { "\($0.step)/\($0.n) \($0.id) \($0.pct)" } ?? "",
                               state: st?.state.rawValue, reasons: st?.reasons ?? [], exe: st?.exe, work: t.work.path)
    }
}
