import Foundation

/// A title's recomp.cfg: the same file and semantics as the Android launcher's (RecompConfig.java, XBA-41) and the
/// runtime's reader (src/platform/recomp_cfg.c). One KEY=VALUE per line, blanks and # comments skipped, keys and
/// values trimmed, the first line for a key wins. Writing keeps every line the player wrote by hand: a managed key's
/// first line is rewritten in place (its later duplicates removed), a default value removes the key's lines, a new
/// key is appended at the end under the launcher's header.
///
/// The desktop main.c does not read the file: the launcher passes its lines to the game as environment variables
/// (ChildEnvironment.game).
public struct RecompConfig: Equatable, Sendable {
    public static let fileName = "recomp.cfg"
    public static let header = "# Written by the launcher's Settings screen; lines you add yourself are kept."

    public let file: URL?
    public private(set) var lines: [String]
    private var existed: Bool

    public init(lines: [String] = [], file: URL? = nil) {
        self.lines = lines
        self.file = file
        self.existed = false
    }

    /// Reads dir/recomp.cfg; a missing or unreadable file reads as empty (every option at its default).
    public static func load(dir: URL) -> RecompConfig {
        let f = dir.appendingPathComponent(fileName)
        var c = RecompConfig(file: f)
        if let data = try? Data(contentsOf: f) {
            // "\r\n" is one Character in Swift: normalise line ends first.
            var text = String(decoding: data, as: UTF8.self).replacingOccurrences(of: "\r\n", with: "\n")
            if text.hasSuffix("\n") { text.removeLast() }
            c.lines = text.isEmpty ? [] : text.components(separatedBy: "\n")
            c.existed = true
        }
        return c
    }

    /// The key of an active line ("KEY=VALUE", not blank or a comment), trimmed; nil otherwise.
    public static func keyOf(_ line: String) -> String? {
        let s = line.trimmingCharacters(in: .whitespaces)
        guard let first = s.first, first != "#", let eq = s.firstIndex(of: "=") else { return nil }
        return s[..<eq].trimmingCharacters(in: .whitespaces)
    }

    /// The value the runtime sees for key (its first line), or nil when the file does not set it.
    public func get(_ key: String) -> String? {
        for line in lines where RecompConfig.keyOf(line) == key {
            let s = line.trimmingCharacters(in: .whitespaces)
            return s[s.index(after: s.firstIndex(of: "=")!)...].trimmingCharacters(in: .whitespaces)
        }
        return nil
    }

    /// Every key the file sets with its effective value (first line wins), in file order: the game's environment.
    public var activePairs: [(key: String, value: String)] {
        var seen = Set<String>()
        var out: [(key: String, value: String)] = []
        for line in lines {
            guard let k = RecompConfig.keyOf(line), !k.isEmpty, seen.insert(k).inserted, let v = get(k) else { continue }
            out.append((k, v))
        }
        return out
    }

    /// Sets key to value (nil removes it), in place of its first line.
    public mutating func set(_ key: String, _ value: String?) {
        var placed = false
        var i = 0
        while i < lines.count {
            if RecompConfig.keyOf(lines[i]) != key { i += 1; continue }
            if let value, !placed {
                lines[i] = "\(key)=\(value)"
                placed = true
                i += 1
            } else {
                lines.remove(at: i)
            }
        }
        if let value, !placed {
            if !lines.contains(RecompConfig.header) {
                if let last = lines.last, !last.trimmingCharacters(in: .whitespaces).isEmpty { lines.append("") }
                lines.append(RecompConfig.header)
            }
            lines.append("\(key)=\(value)")
        }
    }

    public mutating func set(_ option: ConfigOption, _ choice: ConfigOption.Choice) {
        for (k, key) in option.keys.enumerated() { set(key, choice.values[k]) }
    }

    /// The index of the option's current choice, or nil when the file holds a value the list does not offer.
    public func choiceIndex(_ option: ConfigOption) -> Int? {
        option.choices.firstIndex { c in
            option.keys.enumerated().allSatisfy { k, key in c.values[k] == get(key) }
        }
    }

    /// The option's lines as the file holds them, "KEY=VALUE" joined by two spaces; "" when none is set.
    public func rawValue(_ option: ConfigOption) -> String {
        option.keys.compactMap { k in get(k).map { "\(k)=\($0)" } }.joined(separator: "  ")
    }

    /// Every option of the table back to its default; the player's own lines stay.
    public mutating func resetAll(_ options: [ConfigOption] = OptionTable.all) {
        for o in options { set(o, o.choices[o.defaultIndex]) }
    }

    /// True when no option of the table is away from its default.
    public func allDefault(_ options: [ConfigOption] = OptionTable.all) -> Bool {
        options.allSatisfy { choiceIndex($0) == $0.defaultIndex }
    }

    /// The lines no option of the table manages (the Advanced editor's text), the launcher's header left out.
    public func unmanagedText(_ options: [ConfigOption] = OptionTable.all) -> String {
        let managed = Set(options.flatMap(\.keys))
        return lines.filter { l in
            l != RecompConfig.header && !(RecompConfig.keyOf(l).map(managed.contains) ?? false)
        }.joined(separator: "\n")
    }

    /// Replaces the unmanaged lines by `text`, keeping the managed ones (and the header) after it. A managed key
    /// typed into the text is dropped: the option rows own it.
    public mutating func setUnmanagedText(_ text: String, _ options: [ConfigOption] = OptionTable.all) {
        let managed = Set(options.flatMap(\.keys))
        var head = text.components(separatedBy: "\n").filter { !(RecompConfig.keyOf($0).map(managed.contains) ?? false) }
        while let l = head.last, l.trimmingCharacters(in: .whitespaces).isEmpty { head.removeLast() }
        let kept = lines.filter { l in l == RecompConfig.header || (RecompConfig.keyOf(l).map(managed.contains) ?? false) }
        var out = head
        if !kept.isEmpty {
            if !out.isEmpty { out.append("") }
            if !kept.contains(RecompConfig.header) { out.append(RecompConfig.header) }
            out += kept
        }
        lines = out
    }

    /// Writes the file through recomp.cfg.tmp and a rename. Nothing is created when there was no file and only the
    /// header (or nothing) would be written.
    public mutating func save() throws {
        guard let file else { return }
        let onlyHeader = lines.allSatisfy { $0.trimmingCharacters(in: .whitespaces).isEmpty || $0 == RecompConfig.header }
        if !existed && onlyHeader { return }
        let text = lines.map { $0 + "\n" }.joined()
        try writeAtomically(Data(text.utf8), to: file)
        existed = true
    }
}
