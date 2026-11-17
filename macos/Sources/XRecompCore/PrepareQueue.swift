import Foundation

/// The prepares waiting behind the running one, in order, each with its own `--force`. A title is queued once; asking
/// again keeps its place, and a forced request (Manage › Re-prepare…) upgrades a plain one, never the other way round.
public struct PrepareQueue: Equatable, Sendable {
    public struct Entry: Equatable, Sendable {
        public let id: String
        public var force: Bool

        public init(id: String, force: Bool) {
            self.id = id
            self.force = force
        }
    }

    public private(set) var entries: [Entry] = []

    public init() {}

    public var isEmpty: Bool { entries.isEmpty }

    /// Queues id, or upgrades the queued request to `force` when it is already there.
    public mutating func enqueue(_ id: String, force: Bool) {
        if let i = entries.firstIndex(where: { $0.id == id }) {
            if force { entries[i].force = true }
        } else {
            entries.append(Entry(id: id, force: force))
        }
    }

    /// The next prepare to start, removed from the queue.
    public mutating func next() -> Entry? {
        entries.isEmpty ? nil : entries.removeFirst()
    }

    /// Takes id out of the queue; false when it was not queued.
    @discardableResult
    public mutating func remove(_ id: String) -> Bool {
        guard let i = entries.firstIndex(where: { $0.id == id }) else { return false }
        entries.remove(at: i)
        return true
    }

    /// id's place in the queue, from 1; nil when it is not queued.
    public func position(of id: String) -> Int? {
        entries.firstIndex(where: { $0.id == id }).map { $0 + 1 }
    }
}
