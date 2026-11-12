import Foundation
import XRecompCore

/// `XRecomp --smoke [--toolkit DIR] [--library DIR]`: resolves the toolkit, scans, prints JSON, exits.
enum Smoke {
    static func value(_ args: [String], _ flag: String) -> String? {
        guard let i = args.firstIndex(of: flag), i + 1 < args.count else { return nil }
        return args[i + 1]
    }

    static func run(_ args: [String]) -> Int32 {
        var settings = Settings.load(from: AppPaths.standard().settingsFile)
        if let t = value(args, "--toolkit") { settings.toolkitPath = t }
        let tk = Toolkit.resolveForApp(settings: settings)
        let out: [String: Any] = ["toolkit": tk?.root.path ?? NSNull(), "toolkit_source": tk?.source.rawValue ?? NSNull()]
        let data = try! JSONSerialization.data(withJSONObject: out, options: [.prettyPrinted, .sortedKeys])
        print(String(decoding: data, as: UTF8.self))
        return tk == nil ? 2 : 0
    }
}
