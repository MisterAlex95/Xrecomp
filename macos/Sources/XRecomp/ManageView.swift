import AppKit
import SwiftUI
import UniformTypeIdentifiers
import XRecompCore

/// A title's storage: what each folder takes, each deletable after a confirmation, revealable in Finder; a forced
/// re-prepare, the one-shot import of command-line saves, and « Create an app for this game » (not inside a game
/// app).
struct ManageView: View {
    @EnvironmentObject var model: LauncherModel
    let gameId: String
    @Local private var sizes: [(Storage.Item, Int64)] = []
    @Local private var confirm: Storage.Item?
    @Local private var confirmForce = false

    private var game: GameItem? { model.game(gameId) }

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: Theme.s3) {
                PageHeader(title: (game?.name ?? "Game") + " · Manage",
                           subtitle: "Total \(Storage.format(sizes.filter { $0.0 != .extractedDisc && $0.0 != .utilityCache }.map(\.1).reduce(0, +)))"
                                     + " · the game's own files are never touched",
                           back: { model.route = .home(gameId) }) {
                    Button { refresh() } label: { Image(systemName: "arrow.clockwise") }
                        .buttonStyle(PillButtonStyle(compact: true))
                }
                Panel(padding: 0) {
                    VStack(spacing: 0) {
                        ForEach(Array(sizes.enumerated()), id: \.offset) { i, s in
                            row(s.0, s.1)
                            if i < sizes.count - 1 { Divider().overlay(Theme.lineSoft) }
                        }
                    }
                }
                SectionTitle(text: "More")
                Panel {
                    VStack(alignment: .leading, spacing: Theme.s2) {
                        if game?.titleLayer != nil {
                            enhancementsRow
                            Divider().overlay(Theme.lineSoft)
                        }
                        HStack {
                            VStack(alignment: .leading, spacing: 2) {
                                Text("Prepare from scratch").foregroundStyle(Theme.text)
                                Text("Redoes every step, even those that are up to date.")
                                    .font(.caption).foregroundStyle(Theme.textFaint)
                            }
                            Spacer()
                            Button("Re-prepare…") { confirmForce = true }.buttonStyle(PillButtonStyle())
                                .disabled(game?.runningSince != nil || game?.prepare != nil)
                        }
                        Divider().overlay(Theme.lineSoft)
                        HStack {
                            VStack(alignment: .leading, spacing: 2) {
                                Text("Import saves from command-line runs").foregroundStyle(Theme.text)
                                Text("Copies this game's saves from ~/.local/share/xboxrecomp; nothing is overwritten.")
                                    .font(.caption).foregroundStyle(Theme.textFaint)
                            }
                            Spacer()
                            Button("Import") {
                                let n = model.actions?.importSaves(gameId) ?? 0
                                model.notice = n == 0 ? "No saves to import." : "Imported \(n) save folder(s)."
                                refresh()
                            }
                            .buttonStyle(PillButtonStyle())
                        }
                        if model.singleTitle == nil && game?.titleId != nil {
                            Divider().overlay(Theme.lineSoft)
                            titleAppRow
                        }
                    }
                }
            }
            .padding(Theme.s4)
            .frame(maxWidth: 820, alignment: .leading)
            .frame(maxWidth: .infinity, alignment: .leading)
        }
        .onAppear(perform: refresh)
        .confirmationDialog(confirm.map { "Delete the \($0.rawValue.lowercased())?" } ?? "",
                            isPresented: Binding(get: { confirm != nil }, set: { if !$0 { confirm = nil } }),
                            titleVisibility: .visible) {
            Button("Delete", role: .destructive) {
                if let c = confirm { model.actions?.delete(c, of: gameId) }
                confirm = nil
                refresh()
            }
        } message: {
            Text(confirm.map(consequence) ?? "")
        }
        .confirmationDialog("Prepare this game from scratch?", isPresented: $confirmForce, titleVisibility: .visible) {
            Button("Re-prepare") {
                model.actions?.prepare(gameId, force: true)
                model.route = .home(gameId)
            }
        } message: {
            Text("Every step runs again; the first prepare of a game takes many minutes.")
        }
    }

    /// Profile-free WP6: the toolkit's optional enhancements for this title (a 16:9 mod, a front-end rate...), which it
    /// does not need to run. Off: its prepares run with --no-title-layer; the build made the other way turns stale.
    private var enhancementsRow: some View {
        HStack {
            VStack(alignment: .leading, spacing: 2) {
                Text("Enhancements").foregroundStyle(Theme.text)
                Text("Extras the toolkit has for this game, such as a widescreen picture. The game runs without "
                     + "them. Applies at the next prepare.")
                    .font(.caption).foregroundStyle(Theme.textFaint)
            }
            Spacer()
            Toggle("", isOn: Binding(get: { game?.record.enhancementsOn ?? true },
                                     set: { model.actions?.setEnhancements(gameId, on: $0) }))
                .toggleStyle(.switch).labelsHidden()
                .disabled(game?.runningSince != nil || isPreparing)
        }
    }

    /// docs/macos/launcher.md « Game apps »: a copy of XRecomp.app named after the game, its logo as the icon, that
    /// opens straight on this game. It points at the library entry; nothing of the game goes in it.
    private var titleAppRow: some View {
        HStack {
            VStack(alignment: .leading, spacing: 2) {
                Text("Create an app for this game").foregroundStyle(Theme.text)
                Text("An app with the game's logo as its icon, for the Dock or Launchpad, that opens straight on "
                     + "this game. It holds no game data: it uses this library, its settings and saves.")
                    .font(.caption).foregroundStyle(Theme.textFaint)
                    .fixedSize(horizontal: false, vertical: true)
            }
            Spacer()
            if model.creatingApp == gameId { ProgressView().controlSize(.small) }
            Button("Create app…") { pickAppDestination() }
                .buttonStyle(PillButtonStyle())
                .disabled(model.creatingApp != nil)
        }
    }

    /// The save panel: `<Game title>.app` in ~/Applications by default (made when missing).
    private func pickAppDestination() {
        guard let g = game else { return }
        let fm = FileManager.default
        let apps = fm.homeDirectoryForCurrentUser.appendingPathComponent("Applications", isDirectory: true)
        if !model.demo { try? fm.createDirectory(at: apps, withIntermediateDirectories: true) }
        let p = NSSavePanel()
        p.directoryURL = apps
        p.nameFieldStringValue = TitleApp.fileName(title: g.name)
        p.allowedContentTypes = [.applicationBundle]
        p.canCreateDirectories = true
        p.isExtensionHidden = false
        p.prompt = "Create"
        p.message = "Where to put the app for \(g.name)"
        if p.runModal() == .OK, let u = p.url { model.actions?.createTitleApp(gameId, at: u) }
    }

    private func row(_ item: Storage.Item, _ size: Int64) -> some View {
        HStack(spacing: Theme.s2) {
            Image(systemName: icon(item)).frame(width: 20).foregroundStyle(Theme.textDim)
            VStack(alignment: .leading, spacing: 2) {
                Text(item.rawValue).foregroundStyle(Theme.text)
                Text(note(item)).font(.caption).foregroundStyle(Theme.textFaint)
            }
            Spacer()
            Text(size == 0 ? "—" : Storage.format(size)).monospacedDigit().foregroundStyle(Theme.textDim)
                .frame(minWidth: 70, alignment: .trailing)
            Button { if let p = model.actions?.titlePaths(gameId) { model.actions?.reveal(Storage.url(item, p)) } } label: {
                Image(systemName: "folder")
            }
            .buttonStyle(PillButtonStyle(compact: true))
            .help("Reveal in Finder")
            .disabled(size == 0)
            Button { confirm = item } label: { Image(systemName: "trash") }
                .buttonStyle(PillButtonStyle(compact: true, tint: Theme.error))
                .help("Delete")
                .disabled(size == 0 || game?.runningSince != nil || isPreparing)
        }
        .padding(.horizontal, Theme.s2)
        .padding(.vertical, 10)
    }

    private var isPreparing: Bool {
        if case .running = game?.prepare { return true }
        return false
    }

    private func icon(_ i: Storage.Item) -> String {
        switch i {
        case .work: return "hammer"
        case .extractedDisc: return "opticaldisc"
        case .saves: return "externaldrive"
        case .utilityCache: return "tray"
        case .logs: return "doc.text"
        case .art: return "photo"
        }
    }

    private func note(_ i: Storage.Item) -> String {
        switch i {
        case .work: return "Analysis, generated C and the build; the disc extracted from an image is inside."
        case .extractedDisc: return "Part of the work folder (image sources only)."
        case .saves: return "Saved games and title data (RECOMP_SAVE_DIR)."
        case .utilityCache: return "The game's scratch drive, part of the saves."
        case .logs: return "The last 10 runs."
        case .art: return "Decoded from the game's default.xbe."
        }
    }

    private func consequence(_ i: Storage.Item) -> String {
        switch i {
        case .work, .extractedDisc: return "The game must be prepared again before you can play. Your disc image or folder stays."
        case .saves, .utilityCache: return "Saved games of this title are lost for good."
        case .logs, .art: return "It is made again when needed."
        }
    }

    private func refresh() {
        sizes = model.actions?.storage(gameId) ?? []
    }
}
