import Foundation

/// titles/<TID>/state.json: what the launcher remembers about a title between runs. The `xrecomp` command line
/// writes the same file and adds `title` and `console`, which it reads back when the source is gone: a save keeps
/// every key this type does not know.
public struct TitleRecord: Codable, Equatable, Sendable {
    public var sourcePath: String?
    public var lastPlayed: Date?
    /// Seconds played in all.
    public var playSeconds: Double = 0
    public var runs: Int = 0
    public var lastPrepare: PrepareRecord?
    /// The title's optional enhancements (profiles/titles/<id>, profile-free WP6): nil or true = on, false = its
    /// prepares run with --no-title-layer (Manage › Enhancements).
    public var enhancements: Bool?

    /// The enhancements go in (the default).
    public var enhancementsOn: Bool { enhancements ?? true }

    public struct PrepareRecord: Codable, Equatable, Sendable {
        public var ok: Bool
        public var code: String?
        public var message: String?
        public var at: Date

        public init(ok: Bool, code: String? = nil, message: String? = nil, at: Date = Date()) {
            self.ok = ok
            self.code = code
            self.message = message
            self.at = at
        }
    }

    enum CodingKeys: String, CodingKey, CaseIterable {
        case sourcePath = "source_path", lastPlayed = "last_played", playSeconds = "play_seconds", runs
        case lastPrepare = "last_prepare", enhancements
    }

    public init(sourcePath: String? = nil) {
        self.sourcePath = sourcePath
    }

    static var decoder: JSONDecoder {
        let d = JSONDecoder()
        d.dateDecodingStrategy = .iso8601
        return d
    }

    static var encoder: JSONEncoder {
        let e = JSONEncoder.pretty
        e.dateEncodingStrategy = .iso8601
        return e
    }

    /// The record at url; an empty one when there is none. A file that does not decode is set aside as
    /// state.json.bad rather than overwritten (with the play time in it) by the next save.
    public static func load(from url: URL) -> TitleRecord {
        guard let d = try? Data(contentsOf: url) else { return TitleRecord() }
        guard let r = try? decoder.decode(TitleRecord.self, from: d) else {
            setAsideBroken(url)
            return TitleRecord()
        }
        return r
    }

    public func save(to url: URL) throws {
        let known = Set(CodingKeys.allCases.map(\.rawValue))
        try writeAtomically(try keepingUnknownKeys(try TitleRecord.encoder.encode(self), of: url, known: known), to: url)
    }

    /// A run that lasted `seconds` and ended at `end`.
    public mutating func recordRun(seconds: TimeInterval, end: Date = Date()) {
        playSeconds += max(0, seconds)
        runs += 1
        lastPlayed = Date(timeIntervalSince1970: end.timeIntervalSince1970.rounded(.down))   // what ISO 8601 keeps
    }

    /// « Not played yet », « 12 min played », « 3 h 05 played ».
    public var playTimeText: String {
        if runs == 0 || playSeconds < 1 { return "Not played yet" }
        let m = Int(playSeconds / 60)
        if m < 1 { return "Under a minute played" }
        if m < 60 { return "\(m) min played" }
        return String(format: "%d h %02d played", m / 60, m % 60)
    }
}
