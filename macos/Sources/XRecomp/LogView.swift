import AppKit
import SwiftUI
import XRecompCore

/// The live log: the running game's (or prepare's) output, newest at the bottom.
struct LogView: View {
    @EnvironmentObject var model: LauncherModel
    let gameId: String

    var body: some View {
        VStack(alignment: .leading, spacing: Theme.s2) {
            PageHeader(title: (model.game(gameId)?.name ?? "Game") + " · Log",
                       subtitle: model.liveLogTitle.isEmpty ? "Nothing ran yet in this session" : model.liveLogTitle,
                       back: { model.route = .home(gameId) }) {
                HStack(spacing: Theme.s1) {
                    Button {
                        NSPasteboard.general.clearContents()
                        NSPasteboard.general.setString(model.liveLog.joined(separator: "\n"), forType: .string)
                    } label: { Label("Copy", systemImage: "doc.on.doc") }
                    .buttonStyle(PillButtonStyle(compact: true))
                    if let p = model.actions?.titlePaths(gameId) {
                        Button { model.actions?.reveal(RunLog(dir: p.logs).latest ?? p.logs) } label: {
                            Label("Reveal", systemImage: "folder")
                        }
                        .buttonStyle(PillButtonStyle(compact: true))
                    }
                    Button { model.liveLog.removeAll() } label: { Label("Clear", systemImage: "trash") }
                        .buttonStyle(PillButtonStyle(compact: true))
                }
            }
            LogLines(lines: model.liveLog, maxHeight: .infinity)
        }
        .padding(Theme.s4)
    }
}
