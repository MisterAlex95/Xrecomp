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
    /// Sizes being measured, or a delete / import running (off the main thread, AppStore.storageWork).
    @Local private var measuring = false
    @Local private var working = false

    private var game: GameItem? { model.game(gameId) }
    /// A GBA / PS1 / N64 / GameCube game (XBA-278): its pack, its own save folder, no Xbox save import.
    private var isConsole: Bool { game?.entry.isConsole == true }

    /// The rows shown: the extracted disc only when there is one (a work folder made before XBA-126; an image is read
    /// in place now).
    private var rows: [(Storage.Item, Int64)] {
        sizes.filter { $0.0 != .extractedDisc || $0.1 > 0 }
    }

    /// The extracted disc and the Z: cache are inside the work folder and the saves: not counted twice.
    private var total: Int64 {
        sizes.filter { $0.0 != .extractedDisc && $0.0 != .utilityCache }.map(\.1).reduce(0, +)
    }

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: Theme.s3) {
                PageHeader(title: (game?.name ?? "Game") + " · Manage",
                           subtitle: (sizes.isEmpty && measuring ? "Measuring…" : "Total \(Storage.format(total))")
                                     + " · the game's own files are never touched",
                           back: { model.route = .home(gameId) }) {
                    Button { refresh() } label: { Image(systemName: "arrow.clockwise") }
                        .buttonStyle(PillButtonStyle(compact: true))
                        .disabled(measuring || working)
                }
                Panel(padding: 0) {
                    VStack(spacing: 0) {
                        if sizes.isEmpty && measuring {
                            HStack(spacing: Theme.s2) {
                                ProgressView().controlSize(.small)
                                Text("Measuring…").foregroundStyle(Theme.textDim)
                                Spacer()
                            }
                            .padding(.horizontal, Theme.s2)
                            .padding(.vertical, 10)
                        }
                        ForEach(Array(rows.enumerated()), id: \.offset) { i, s in
                            row(s.0, s.1)
                            if i < rows.count - 1 { Divider().overlay(Theme.lineSoft) }
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
                                .disabled(game?.runningSince != nil || game?.prepare != nil || working)
                        }
                        if !isConsole {
                        Divider().overlay(Theme.lineSoft)
                        HStack {
                            VStack(alignment: .leading, spacing: 2) {
                                Text("Import saves from command-line runs").foregroundStyle(Theme.text)
                                Text("Copies this game's saves from ~/.local/share/xboxrecomp; nothing is overwritten.")
                                    .font(.caption).foregroundStyle(Theme.textFaint)
                            }
                            Spacer()
                            Button("Import") {
                                working = true
                                model.actions?.importSaves(gameId) { n in
                                    working = false
                                    if let n { model.notice = n == 0 ? "No saves to import." : "Imported \(n) save folder(s)." }
                                    refresh()
                                }
                            }
                            .buttonStyle(PillButtonStyle())
                            .disabled(working || game?.runningSince != nil)
                        }
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
                if let c = confirm {
                    working = true
                    model.actions?.delete(c, of: gameId) {
                        working = false
                        refresh()
                    }
                }
                confirm = nil
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
                Text(game?.playable == true
                     ? "An app with the game's logo as its icon, for the Dock or Launchpad, that opens straight on "
                       + "this game. It holds no game data: it uses this library, its settings and saves."
                     : "Prepare this game first (Home › Prepare): its app opens straight on the game.")
                    .font(.caption).foregroundStyle(Theme.textFaint)
                    .fixedSize(horizontal: false, vertical: true)
            }
            Spacer()
            if model.creatingApp == gameId { ProgressView().controlSize(.small) }
            Button("Create app…") { pickAppDestination() }
                .buttonStyle(PillButtonStyle())
                .disabled(model.creatingApp != nil || game?.playable != true)
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
            Text(measuring ? "…" : size == 0 ? "—" : Storage.format(size)).monospacedDigit().foregroundStyle(Theme.textDim)
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
                .disabled(size == 0 || game?.runningSince != nil || isPreparing || working)
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
        case .pack: return "shippingbox"
        }
    }

    private func note(_ i: Storage.Item) -> String {
        switch i {
        case .work: return "Analysis, generated C and the build. A disc image is read where it is, not copied."
        case .extractedDisc: return "A disc copied out of its image by an older prepare; part of the work folder."
        case .saves: return isConsole ? "Battery saves, memory cards and save states."
            : "Saved games and title data (RECOMP_SAVE_DIR)."
        case .utilityCache: return "The game's scratch drive, part of the saves."
        case .logs: return "The last 10 runs."
        case .art: return isConsole ? "The box art downloaded for this game." : "Decoded from the game's default.xbe."
        case .pack: return "The game prepared as native code. Without it the game plays in the interpreter."
        }
    }

    private func consequence(_ i: Storage.Item) -> String {
        switch i {
        case .work, .extractedDisc:
            return isConsole ? "The next prepare starts from scratch. Your game file stays."
                : "The game must be prepared again before you can play. Your disc image or folder stays."
        case .pack: return "The game plays in the interpreter until it is prepared again. Your game file stays."
        case .saves, .utilityCache: return "Saved games of this title are lost for good."
        case .logs, .art: return "It is made again when needed."
        }
    }

    /// Measured off the main thread: opening Manage on a large title no longer stalls the window.
    private func refresh() {
        guard let actions = model.actions else { return }
        measuring = true
        actions.storage(gameId) { s in
            sizes = s
            measuring = false
        }
    }
}
