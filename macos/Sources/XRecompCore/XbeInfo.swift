import Foundation

/// The little the library needs from a default.xbe: title ID, name, and its sections (for the title image). Public
/// format documentation only (xboxdevwiki « Xbe », docs/formats/xbe.md): "XBEH"; base address at 0x104, size of
/// headers at 0x108, certificate address at 0x118, section count at 0x11C, section headers at 0x120 (56 bytes each:
/// raw address +0x0C, raw size +0x10, name address +0x14). The certificate holds the title ID at +0x08 and the name,
/// 40 UTF-16LE characters, at +0x0C. Addresses minus the base are file offsets in the header area.
/// A short, corrupt or foreign file gives nil, never a crash.
public struct XbeInfo: Equatable, Sendable {
    public let titleId: String   // 8 upper-case hex digits
    public let title: String
    public let size: Int64

    /// The largest header area read, and the largest section returned.
    static let maxHeaders = 4 << 20, maxSection = 4 << 20

    public static func read(_ url: URL) -> XbeInfo? {
        guard let h = FileHandle(forReadingAtPath: url.path) else { return nil }
        defer { try? h.close() }
        let size = (try? h.seekToEnd()).map(Int64.init) ?? 0
        try? h.seek(toOffset: 0)
        guard let data = try? h.read(upToCount: 0x1000), let head = parseHeader(data) else { return nil }
        return XbeInfo(titleId: head.titleId, title: head.title, size: size)
    }

    /// Title ID and name from the first bytes of an XBE (the certificate must be inside them).
    public static func parseHeader(_ d: Data) -> (titleId: String, title: String)? {
        let b = [UInt8](d)
        guard b.count >= 0x178, b[0] == 0x58, b[1] == 0x42, b[2] == 0x45, b[3] == 0x48 else { return nil }
        let base = Int64(le32(b, 0x104)), cert = Int64(le32(b, 0x118)) - base
        guard cert >= 0x178, cert + 0x0C + 80 <= Int64(b.count) else { return nil }
        let c = Int(cert)
        let tid = String(format: "%08X", le32(b, c + 8))
        var units: [UInt16] = []
        for i in 0..<40 {
            let u = UInt16(b[c + 0x0C + 2 * i]) | UInt16(b[c + 0x0C + 2 * i + 1]) << 8
            if u == 0 { break }
            units.append(u)
        }
        let name = String(decoding: units, as: UTF16.self).trimmingCharacters(in: .whitespacesAndNewlines)
        return (tid, name)
    }

    /// The raw bytes of the section called `name`, or nil (no such section, not an XBE, unreadable).
    /// TitleArt.xbeSection in the Android launcher.
    public static func section(_ url: URL, named name: String) -> [UInt8]? {
        guard let h = FileHandle(forReadingAtPath: url.path) else { return nil }
        defer { try? h.close() }
        guard let len = try? h.seekToEnd(), len >= 0x178 else { return nil }
        try? h.seek(toOffset: 0)
        guard let hd = try? h.read(upToCount: 0x178) else { return nil }
        let hdr = [UInt8](hd)
        guard hdr.count == 0x178, hdr[0] == 0x58, hdr[1] == 0x42, hdr[2] == 0x45, hdr[3] == 0x48 else { return nil }
        let base = Int64(le32(hdr, 0x104)), headers = Int64(le32(hdr, 0x108))
        let n = Int64(Int32(bitPattern: le32(hdr, 0x11C)))
        let secs = Int64(le32(hdr, 0x120)) - base
        let fileLen = Int64(len)
        guard n > 0, n <= 512, secs >= 0, headers <= maxHeaders, secs + 56 * n <= min(fileLen, headers) else {
            return nil
        }
        try? h.seek(toOffset: 0)
        guard let hb = try? h.read(upToCount: Int(min(fileLen, headers))) else { return nil }
        let head = [UInt8](hb)
        let want = Array(name.utf8)
        for i in 0..<Int(n) {
            let sh = Int(secs) + 56 * i
            let nameOff = Int64(le32(head, sh + 0x14)) - base
            guard nameOff >= 0, nameOff + Int64(want.count) + 1 <= Int64(head.count) else { continue }
            let o = Int(nameOff)
            guard head[o + want.count] == 0, Array(head[o..<o + want.count]) == want else { continue }
            let raw = Int64(le32(head, sh + 0x0C)), sz = Int64(le32(head, sh + 0x10))
            guard sz > 0, sz <= maxSection, raw + sz <= fileLen else { return nil }
            try? h.seek(toOffset: UInt64(raw))
            guard let out = try? h.read(upToCount: Int(sz)), out.count == Int(sz) else { return nil }
            return [UInt8](out)
        }
        return nil
    }
}

@inline(__always)
func le32(_ b: [UInt8], _ o: Int) -> UInt32 {
    UInt32(b[o]) | UInt32(b[o + 1]) << 8 | UInt32(b[o + 2]) << 16 | UInt32(b[o + 3]) << 24
}
