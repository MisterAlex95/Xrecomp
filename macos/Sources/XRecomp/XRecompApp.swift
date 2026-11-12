import AppKit
import SwiftUI
import XRecompCore

enum LaunchOptions {
    /// Set by --demo: the screen to open on.
    static var demoScreen: String?
}

/// Keeps the store alive and makes the window come forward when started from a terminal.
final class AppDelegate: NSObject, NSApplicationDelegate {
    func applicationDidFinishLaunching(_ notification: Notification) {
        NSApp.activate(ignoringOtherApps: true)
        if LaunchOptions.demoScreen != nil {
            // For screenshots (screencapture -l <id>): the window's number once it is on screen.
            DispatchQueue.main.asyncAfter(deadline: .now() + 1.5) {
                if let w = NSApp.windows.first(where: { $0.isVisible && $0.contentView != nil }) {
                    print("XRECOMP_WINDOW \(w.windowNumber)")
                    fflush(stdout)
                }
            }
        }
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { true }
}

struct XRecompApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) var delegate
    @StateObject private var holder = StoreHolder()

    var body: some Scene {
        WindowGroup("XRecomp") {
            RootView()
                .environmentObject(holder.model)
                .frame(minWidth: 900, minHeight: 600)
        }
        .defaultSize(width: 1180, height: 780)
        .windowToolbarStyle(.unifiedCompact)
        .commands {
            CommandGroup(replacing: .newItem) {}
            CommandMenu("Library") {
                Button("Scan again") { holder.model.actions?.rescan() }.keyboardShortcut("r", modifiers: .command)
                Button("Library") { holder.model.route = .library }.keyboardShortcut("1", modifiers: .command)
                Button("Setup") { holder.model.route = .setup }.keyboardShortcut(",", modifiers: [.command, .shift])
            }
            CommandGroup(replacing: .appInfo) {
                Button("About XRecomp") { holder.model.route = .about }
            }
        }
    }
}

/// The model and whoever does its actions: the real store, or the demo one.
@MainActor
final class StoreHolder: ObservableObject {
    let model: LauncherModel
    private let store: AnyObject

    init() {
        if let screen = LaunchOptions.demoScreen {
            let s = PreviewStore(screen: screen)
            model = s.model
            store = s
        } else {
            let s = AppStore()
            model = s.model
            store = s
            s.start()
        }
    }
}
