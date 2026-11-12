import Foundation

/// The lines `python -m tools.prepare SOURCE --work DIR --progress` prints on stdout (docs/macos/launcher.md
/// « Prepare interface »): the same grammar as the Android prepare (PrepareProtocol.java, tools/prepare/progress.py),
/// with the desktop's step ids and result.
///
///     XBA_PROGRESS step=<i>/<n> id=<identify|analyse|project|lift|build> pct=<0-100> [eta=<s>] msg=<rest>
///     XBA_RESULT ok exe=<path>
///     XBA_RESULT ok project=<dir>          (with --no-build)
///     XBA_RESULT error code=<UPPER_SNAKE> msg=<text>
///
/// msg is last and takes the rest of the line; unknown keys are ignored. exe (like Android's pack) takes the rest of
/// the line too, so a path with spaces (Application Support) survives. Exit 0 ok, 1 error, 130 cancelled.
public enum PrepareProtocol {
    public static let progressWord = "XBA_PROGRESS"
    public static let resultWord = "XBA_RESULT"

    /// The desktop prepare's steps, in order.
    public static let steps = ["identify", "analyse", "project", "lift", "build"]
    /// The error codes of progress.py's ERROR_CODES the launcher has words for.
    public static let errorMessages: [String: String] = [
        "BAD_ARGS": "The launcher passed a wrong option to prepare.",
        "NO_XBE": "No default.xbe in this game's folder or disc image.",
        "BAD_XBE": "The game's default.xbe could not be read.",
        "BAD_IMAGE": "The disc image could not be read (still being copied?).",
        "BAD_PROFILE": "The title's profile in the toolkit is broken.",
        "ANALYSE_FAILED": "The analysis of the game's code failed.",
        "LIFT_FAILED": "Turning the game's code into C failed.",
        "COMPILE_FAILED": "The build failed.",
        "LINK_FAILED": "Linking the game failed.",
        "NO_SPACE": "The disk is full.",
        "CANCELLED": "Cancelled.",
        "INTERNAL": "Prepare hit a bug.",
    ]

    public struct Progress: Equatable, Sendable {
        public var step = 0, n = 0, pct = 0
        public var id = ""
        /// Seconds left, or -1 when not given.
        public var eta = -1
        public var msg = ""

        public init(step: Int = 0, n: Int = 0, pct: Int = 0, id: String = "", eta: Int = -1, msg: String = "") {
            self.step = step
            self.n = n
            self.pct = pct
            self.id = id
            self.eta = eta
            self.msg = msg
        }

        /// 0...1 over the whole prepare.
        public var overall: Double { PrepareProtocol.overall(step: step, n: n, pct: pct) }
    }

    public struct Result: Equatable, Sendable {
        public var ok = false
        public var exe = "", project = "", pack = "", code = "", msg = ""

        public init() {}
    }

    /// Splits "key=value key=value msg=rest" after the line's first word; a key in greedy takes the rest of the line.
    /// Bare words (no '=') come with a nil value.
    static func fields(_ rest: Substring, greedy: Set<String>, _ sink: (String, String?) -> Bool) {
        var i = rest.startIndex
        while i < rest.endIndex {
            while i < rest.endIndex, rest[i] == " " { i = rest.index(after: i) }
            if i >= rest.endIndex { break }
            let end = rest[i...].firstIndex(of: " ") ?? rest.endIndex
            let tok = rest[i..<end]
            guard let eq = tok.firstIndex(of: "="), eq != tok.startIndex else {
                if !sink(String(tok), nil) { return }
                i = end
                continue
            }
            let key = String(tok[..<eq])
            if greedy.contains(key) {
                _ = sink(key, String(rest[rest.index(after: eq)...]))
                return
            }
            if !sink(key, String(tok[tok.index(after: eq)...])) { return }
            i = end
        }
    }

    static func afterWord(_ line: String?, _ word: String) -> Substring? {
        guard let line else { return nil }
        var l = Substring(line)
        if l.hasSuffix("\r") { l = l.dropLast() }
        if l.hasSuffix("\n") { l = l.dropLast() }
        guard l.hasPrefix(word) else { return nil }
        let after = l.dropFirst(word.count)
        if after.isEmpty { return after }
        guard after.first == " " else { return nil }
        return after.dropFirst()
    }

    /// A progress line, or nil when the line is not one (or is malformed: no step i/n, no id, no pct).
    public static func parseProgress(_ line: String?) -> Progress? {
        guard let rest = afterWord(line, progressWord) else { return nil }
        var p = Progress()
        var seenStep = false, seenId = false, seenPct = false, bad = false
        fields(rest, greedy: ["msg"]) { k, v in
            guard let v else { return true }
            switch k {
            case "step":
                let parts = v.split(separator: "/", omittingEmptySubsequences: false)
                if parts.count == 2, let s = Int(parts[0]), let n = Int(parts[1]), n > 0, s >= 0, s <= n {
                    p.step = s
                    p.n = n
                    seenStep = true
                } else {
                    bad = true
                }
            case "id":
                p.id = v
                seenId = !v.isEmpty
            case "pct":
                if let d = Double(v), !d.isNaN {
                    p.pct = Int((max(0, min(100, d))).rounded())
                    seenPct = true
                } else {
                    bad = true
                }
            case "eta":
                if let d = Double(v), !d.isNaN, d >= 0 { p.eta = Int(d.rounded()) } else { p.eta = -1 }
            case "msg":
                p.msg = v
            default:
                break   // unknown keys are ignored
            }
            return true
        }
        return bad || !seenStep || !seenId || !seenPct ? nil : p
    }

    /// A result line, or nil when the line is not one.
    public static func parseResult(_ line: String?) -> Result? {
        guard let rest = afterWord(line, resultWord) else { return nil }
        var r = Result()
        var words = 0
        var valid = false
        fields(rest, greedy: ["msg", "pack", "exe", "project"]) { k, v in
            guard let v else {
                if words == 0 {
                    if k == "ok" { r.ok = true; valid = true } else if k == "error" { valid = true }
                }
                words += 1
                return true
            }
            switch k {
            case "exe": r.exe = v
            case "project": r.project = v
            case "pack": r.pack = v
            case "code": r.code = v
            case "msg": r.msg = v
            default: break
            }
            return true
        }
        guard valid else { return nil }
        if !r.ok && r.code.isEmpty { r.code = "UNKNOWN" }
        return r
    }

    /// Overall progress 0...1 from step i of n at pct (step 0 counts as 0).
    public static func overall(step: Int, n: Int, pct: Int) -> Double {
        guard n > 0, step > 0 else { return 0 }
        return max(0, min(1, (Double(step - 1) + Double(pct) / 100) / Double(n)))
    }

    /// Index of a step id, nil for an unknown one.
    public static func stepIndex(_ id: String) -> Int? { steps.firstIndex(of: id) }

    /// The step's words for the Preparing screen.
    public static func stepTitle(_ id: String) -> String {
        switch id {
        case "identify": return "Reading the game"
        case "analyse": return "Analysing the code"
        case "project": return "Setting up the project"
        case "lift": return "Translating to C"
        case "build": return "Building for this Mac"
        default: return id.isEmpty ? "Starting" : id
        }
    }

    /// « < 1 min », « 12 min », « 1 h 05 » from seconds.
    public static func formatEta(_ s: Int) -> String {
        if s < 60 { return "< 1 min" }
        let m = (s + 59) / 60
        if m < 60 { return "\(m) min" }
        return String(format: "%d h %02d", m / 60, m % 60)
    }

    /// « 0:42 », « 12:03 », « 1:02:03 » for elapsed and play times.
    public static func formatClock(_ seconds: Int) -> String {
        let s = max(0, seconds)
        if s >= 3600 { return String(format: "%d:%02d:%02d", s / 3600, s / 60 % 60, s % 60) }
        return String(format: "%d:%02d", s / 60, s % 60)
    }
}
