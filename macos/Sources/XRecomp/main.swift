import AppKit
import Foundation
import XRecompCore

// Command-line modes, handled before the app starts:
//   --smoke [--toolkit DIR] [--library DIR]...   no window, the library as JSON on stdout
//   --demo [screen]                              every screen from mock data (PreviewStore.screens)
let args = Array(CommandLine.arguments.dropFirst())
if args.contains("--smoke") {
    exit(Smoke.run(args))
}
if let i = args.firstIndex(of: "--demo") {
    let next = i + 1 < args.count ? args[i + 1] : "library"
    LaunchOptions.demoScreen = next.hasPrefix("-") ? "library" : next
}
// Started from a terminal (swift run, the bare binary) the process is not a bundle: make it a regular app anyway.
NSApplication.shared.setActivationPolicy(.regular)
XRecompApp.main()
