import Foundation

/// The Manage screen's numbers and deletions for one title. A delete never follows a symbolic link: the work
/// folder's `project/build/game` points at the player's own disc folder (folder sources), which must stay untouched.
public enum Storage {
    public enum Item: String, CaseIterable, Sendable {
        case work = "Work folder"
        case extractedDisc = "Extracted disc"
        case saves = "Saves"
        case utilityCache = "Z: cache"
        case logs = "Logs"
        case art = "Title image"
    }

    public static func url(_ item: Item, _ t: TitlePaths) -> URL {
        switch item {
        case .work: return t.work
        case .extractedDisc: return t.extractedGame
        case .saves: return t.save
        case .utilityCache: return t.save.appendingPathComponent("Cache/\(t.titleId)", isDirectory: true)
        case .logs: return t.logs
        case .art: return t.art
        }
    }

    /// Bytes under url, symbolic links counted as links (their targets are not walked).
    public static func size(_ url: URL) -> Int64 {
        var st = stat()
        guard lstat(url.path, &st) == 0 else { return 0 }
        if (st.st_mode & S_IFMT) != S_IFDIR { return Int64(st.st_blocks) * 512 }
        var total = Int64(st.st_blocks) * 512
        let names = (try? FileManager.default.contentsOfDirectory(atPath: url.path)) ?? []
        for n in names { total += size(url.appendingPathComponent(n)) }
        return total
    }

    public static func sizes(_ t: TitlePaths) -> [(Item, Int64)] {
        Item.allCases.map { ($0, size(url($0, t))) }
    }

    /// Removes url and everything under it; a symbolic link inside (or url itself) is unlinked, never followed.
    public static func delete(_ url: URL) throws {
        var st = stat()
        guard lstat(url.path, &st) == 0 else { return }
        if (st.st_mode & S_IFMT) == S_IFDIR {
            let names = (try? FileManager.default.contentsOfDirectory(atPath: url.path)) ?? []
            for n in names { try delete(url.appendingPathComponent(n)) }
            guard rmdir(url.path) == 0 else { throw posixError(url) }
        } else {
            guard unlink(url.path) == 0 else { throw posixError(url) }
        }
    }

    public static func delete(_ item: Item, _ t: TitlePaths) throws {
        try delete(url(item, t))
    }

    static func posixError(_ url: URL) -> Error {
        NSError(domain: NSPOSIXErrorDomain, code: Int(errno), userInfo: [NSFilePathErrorKey: url.path])
    }

    /// Free bytes on the volume holding url (or its nearest existing parent).
    public static func freeSpace(at url: URL) -> Int64? {
        var u = url
        while !FileManager.default.fileExists(atPath: u.path) && u.path != "/" { u = u.deletingLastPathComponent() }
        let v = try? u.resourceValues(forKeys: [.volumeAvailableCapacityForImportantUsageKey])
        return v?.volumeAvailableCapacityForImportantUsage
    }

    /// What a first prepare needs: about 2 GB of analysis, generated C and build, for a folder or a disc image alike.
    /// An image is read in place (XBA-126: the launcher never passes `--extract`), so its size does not count.
    public static let prepareEstimate: Int64 = 2 << 30

    public static func format(_ bytes: Int64) -> String {
        ByteCountFormatter.string(fromByteCount: bytes, countStyle: .file)
    }
}
