import Foundation
import XRecompCore

/// `XRecomp --smoke [--toolkit DIR] [--library DIR]...`: resolves the toolkit and Python, scans the library (the
/// given folders, else the settings'), prints it as JSON and exits without a window. 0 when the toolkit was found.
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
            case truncated
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
        let data = try! JSONEncoder.pretty.encode(report)
        print(String(decoding: data, as: UTF8.self))
        return tk == nil ? 2 : 0
    }
}
