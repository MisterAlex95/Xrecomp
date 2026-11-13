// swift-tools-version:5.9
// The macOS launcher (docs/macos/launcher.md). Builds with the Command Line Tools alone: no Xcode project, and the
// checks are an executable (xrecomp-check) because XCTest and swift-testing are not available without Xcode.
import PackageDescription

let package = Package(
    name: "XRecomp",
    platforms: [.macOS(.v14)],
    products: [
        .executable(name: "XRecomp", targets: ["XRecomp"]),
        .executable(name: "xrecomp-check", targets: ["xrecomp-check"]),
    ],
    targets: [
        // All logic, Foundation (and CoreGraphics / ImageIO / CoreText for the title art and game-app icons) only: no SwiftUI.
        .target(name: "XRecompCore"),
        // The SwiftUI app: views, theme, the store that drives XRecompCore.
        .executableTarget(name: "XRecomp", dependencies: ["XRecompCore"]),
        // The checks: `swift run xrecomp-check`.
        .executableTarget(name: "xrecomp-check", dependencies: ["XRecompCore"]),
    ]
)
