import Foundation

/// library.json: the scan cache. An identified source is stored by path with its size and modification time; the
/// next scan reuses it while both match, so a rescan does not run identify on every ISO again. Sources that failed
/// are never stored (an ISO still being copied is retried).
public struct LibraryIndex: Codable, Equatable, Sendable {
    public var version = 1
    public private(set) var entries: [String: LibraryEntry] = [:]

    enum CodingKeys: String, CodingKey { case version = "v", entries }

    public init() {}

    public func lookup(path: String, size: Int64, mtime: Double) -> LibraryEntry? {
        guard let e = entries[path], e.size == size, abs(e.mtime - mtime) < 0.001, e.titleId != nil else { return nil }
        return e
    }

    public mutating func store(_ e: LibraryEntry) {
        var c = e
        c.duplicates = []
        entries[e.path] = c
    }

    /// Drops the sources that are gone.
    public mutating func prune(keeping paths: Set<String>) {
        entries = entries.filter { paths.contains($0.key) }
    }

    public static func load(from url: URL) -> LibraryIndex {
        guard let d = try? Data(contentsOf: url), let i = try? JSONDecoder().decode(LibraryIndex.self, from: d),
              i.version == 1 else { return LibraryIndex() }
        return i
    }

    public func save(to url: URL) throws {
        try writeAtomically(try JSONEncoder.pretty.encode(self), to: url)
    }
}
