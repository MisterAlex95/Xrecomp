import Foundation
import SwiftUI
import XRecompCore

// Command-line modes, handled before the app starts: --smoke (no window, JSON on stdout) and --demo (mock data).
let args = Array(CommandLine.arguments.dropFirst())
if args.contains("--smoke") {
    exit(Smoke.run(args))
}
XRecompApp.main()
