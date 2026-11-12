import Foundation

/// A small assert harness: XCTest and swift-testing need Xcode, which this Mac does not have
/// (docs/macos/launcher.md « Checks »). Each check is a named closure; a failed expectation is reported with its
/// file and line, and the run exits 1 when anything failed.
final class Harness {
    private(set) var failures = 0
    private(set) var passed = 0
    private var current = ""
    private var currentFailed = false
    private var checks: [(String, () throws -> Void)] = []

    func add(_ name: String, _ body: @escaping () throws -> Void) {
        checks.append((name, body))
    }

    func run(filter: String?) -> Int32 {
        let t0 = Date()
        var ran = 0
        for (name, body) in checks where filter == nil || name.contains(filter!) {
            current = name
            currentFailed = false
            ran += 1
            do { try body() } catch { fail("threw \(error)", #file, #line) }
            if currentFailed {
                print("FAIL  \(name)")
            } else {
                passed += 1
                print("ok    \(name)")
            }
        }
        let failedChecks = ran - passed
        print(String(format: "\n%d checks, %d passed, %d failed (%d failed expectations) in %.1f s",
                     ran, passed, failedChecks, failures, Date().timeIntervalSince(t0)))
        return failedChecks == 0 && ran > 0 ? 0 : 1
    }

    func fail(_ msg: String, _ file: String, _ line: Int) {
        failures += 1
        currentFailed = true
        print("      \((file as NSString).lastPathComponent):\(line): \(msg)")
    }
}

let H = Harness()

func check(_ name: String, _ body: @escaping () throws -> Void) { H.add(name, body) }

func expect(_ cond: @autoclosure () -> Bool, _ msg: @autoclosure () -> String = "expectation failed",
            file: String = #file, line: Int = #line) {
    if !cond() { H.fail(msg(), file, line) }
}

func expectEq<T: Equatable>(_ a: T, _ b: T, _ msg: String = "", file: String = #file, line: Int = #line) {
    if a != b { H.fail("\(msg.isEmpty ? "" : msg + ": ")\(String(describing: a)) != \(String(describing: b))", file, line) }
}

func expectNil<T>(_ a: T?, _ msg: String = "", file: String = #file, line: Int = #line) {
    if let a { H.fail("\(msg.isEmpty ? "" : msg + ": ")expected nil, got \(a)", file, line) }
}

@discardableResult
func expectNotNil<T>(_ a: T?, _ msg: String = "", file: String = #file, line: Int = #line) -> T? {
    if a == nil { H.fail("\(msg.isEmpty ? "" : msg + ": ")unexpected nil", file, line) }
    return a
}

/// A fresh temporary directory, removed at exit.
func tempDir(_ tag: String = "x") -> URL {
    let base = URL(fileURLWithPath: NSTemporaryDirectory()).appendingPathComponent("xrecomp-check", isDirectory: true)
    let d = base.appendingPathComponent("\(tag)-\(UUID().uuidString.prefix(8))", isDirectory: true)
    try! FileManager.default.createDirectory(at: d, withIntermediateDirectories: true)
    tempDirs.append(d)
    return d
}

var tempDirs: [URL] = []

func write(_ url: URL, _ text: String, executable: Bool = false) {
    try! FileManager.default.createDirectory(at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
    try! text.write(to: url, atomically: true, encoding: .utf8)
    if executable { chmod(url.path, 0o755) }
}

func write(_ url: URL, _ data: Data) {
    try! FileManager.default.createDirectory(at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
    try! data.write(to: url)
}

func read(_ url: URL) -> String { (try? String(contentsOf: url, encoding: .utf8)) ?? "" }

/// Waits for cond up to timeout seconds, polling.
@discardableResult
func waitFor(_ timeout: TimeInterval, _ cond: () -> Bool) -> Bool {
    let end = Date().addingTimeInterval(timeout)
    while Date() < end {
        if cond() { return true }
        usleep(20_000)
    }
    return cond()
}
