import SwiftUI
import XRecompCore

/// A game's Home: blurred hero backdrop, title, one status line, the solid Play / Prepare / Stop and three quiet
/// pills (Settings, Controls, Manage); below, the prepare progress or failure, then the facts.
struct HomeView: View {
    @EnvironmentObject var model: LauncherModel
    let game: GameItem

    var body: some View {
        ZStack(alignment: .top) {
            Hero(game: game)
            ScrollView {
                VStack(alignment: .leading, spacing: Theme.s3) {
                    HStack {
                        if model.singleTitle == nil {
                            Button { model.route = .library } label: { Image(systemName: "chevron.left") }
                                .buttonStyle(PillButtonStyle(compact: true))
                                .keyboardShortcut("[", modifiers: .command)
                                .help("Library")
                        } else {
                            singleTitleMenu
                        }
                        Spacer()
                    }
                    header
                    switch game.prepare {
                    case .running(let p, let started)?:
                        PreparingView(game: game, progress: p, started: started)
                    case .queued(let pos)?:
                        Panel {
                            HStack {
                                StatusChip(text: pos == 1 ? "Waiting for the current prepare to finish"
                                                          : "Queued: \(pos - 1) prepare(s) before this one",
                                           color: Theme.warn)
                                Spacer()
                                Button("Remove from queue") { model.actions?.cancelPrepare(game.id) }
                                    .buttonStyle(PillButtonStyle(compact: true))
                            }
                        }
                    case .failed(let code, let message, let tail, let log)?:
                        FailedView(game: game, code: code, message: message, tail: tail, log: log)
                    case nil:
                        EmptyView()
                    }
                    if game.runningSince != nil {
                        VStack(alignment: .leading, spacing: Theme.s1) {
                            HStack {
                                SectionTitle(text: "Game output")
                                Spacer()
                                Button("Full log") { model.route = .log(game.id) }.buttonStyle(PillButtonStyle(compact: true))
                            }
                            LogLines(lines: Array(model.liveLog.suffix(14)), maxHeight: 220)
                        }
                    }
                    if game.prepare == nil, let e = game.lastExit {
                        ExitProblemView(game: game, problem: e)
                    }
                    facts
                }
                .padding(Theme.s4)
                .frame(maxWidth: 980, alignment: .leading)
                .frame(maxWidth: .infinity, alignment: .leading)
            }
        }
    }

    /// A game app has no Library: what the sidebar gives the main app is in this menu.
    private var singleTitleMenu: some View {
        Menu {
            Button("Setup") { model.route = .setup }
            Button("Default settings") { model.route = .defaults }
            Button("About XRecomp") { model.route = .about }
            Divider()
            Button("Open XRecomp (all games)") { model.actions?.openMainApp() }
        } label: {
            Image(systemName: "ellipsis.circle")
        }
        .menuStyle(.button)
        .buttonStyle(.borderless)
        .menuIndicator(.hidden)
        .fixedSize()
        .help("Setup, default settings, the whole library")
    }

    private var header: some View {
        HStack(alignment: .bottom, spacing: Theme.s3) {
            Thumb(game: game, size: 148, corner: 18)
                .shadow(color: .black.opacity(0.5), radius: 18, y: 8)
            VStack(alignment: .leading, spacing: Theme.s1) {
                Text(game.name)
                    .font(.system(size: 32, weight: .bold))
                    .foregroundStyle(Theme.textBright)
                    .lineLimit(2)
                StatusChip(text: game.statusLine.0, color: game.statusLine.1)
                    .font(.callout)
                    .id(model.tick)
                HStack(spacing: Theme.s1) {
                    primaryButton
                    Button { model.route = .settings(game.id) } label: { Label("Settings", systemImage: "slider.horizontal.3") }
                        .buttonStyle(PillButtonStyle())
                    Button { model.route = .controls(game.id) } label: { Label("Controls", systemImage: "gamecontroller") }
                        .buttonStyle(PillButtonStyle())
                    Button { model.route = .manage(game.id) } label: { Label("Manage", systemImage: "internaldrive") }
                        .buttonStyle(PillButtonStyle())
                }
                .padding(.top, Theme.s1)
                if game.playable && game.runningSince == nil, let other = model.runningGame {
                    Text("\(other.name) is running: stop it to play this one.")
                        .font(.caption).foregroundStyle(Theme.textFaint)
                }
            }
            Spacer(minLength: 0)
        }
    }

    @ViewBuilder private var primaryButton: some View {
        if game.runningSince != nil {
            Button { model.actions?.stop(game.id) } label: { Label("Stop", systemImage: "stop.fill") }
                .buttonStyle(PrimaryButtonStyle(color: Theme.error))
                .keyboardShortcut(".", modifiers: .command)
        } else if case .running = game.prepare {
            Button { model.actions?.cancelPrepare(game.id) } label: { Label("Cancel", systemImage: "xmark") }
                .buttonStyle(PrimaryButtonStyle(color: Theme.textDim))
        } else if game.playable {
            Button { model.actions?.play(game.id) } label: { Label("Play", systemImage: "play.fill") }
                .buttonStyle(PrimaryButtonStyle(color: game.accentColor))
                .keyboardShortcut(.return, modifiers: .command)
                .disabled(model.runningGame != nil)
            if case .stale = game.status {
                Button { model.actions?.prepare(game.id, force: false) } label: {
                    Label("Prepare again", systemImage: "arrow.triangle.2.circlepath")
                }
                .buttonStyle(PillButtonStyle(tint: Theme.warn))
            }
        } else {
            Button { model.actions?.prepare(game.id, force: false) } label: { Label("Prepare", systemImage: "hammer.fill") }
                .buttonStyle(PrimaryButtonStyle(color: game.accentColor))
                .keyboardShortcut(.return, modifiers: .command)
                .disabled(!canPrepare)
        }
    }

    private var canPrepare: Bool {
        if case .unreadable = game.status { return false }
        if case .queued = game.prepare { return false }
        return model.setup.toolkit != nil && model.setup.python != nil
    }

    private var facts: some View {
        VStack(alignment: .leading, spacing: Theme.s1) {
            SectionTitle(text: "About this copy")
            Panel {
                Grid(alignment: .leading, horizontalSpacing: Theme.s3, verticalSpacing: 10) {
                    fact("Title ID", game.titleId ?? "—")
                    fact("Source", game.entry.path)
                    fact("Kind", game.entry.kind == .image ? "Disc image · \(Storage.format(game.entry.size))" : "Folder")
                    fact("Played", game.record.playTimeText
                         + (game.record.lastPlayed.map { " · last " + $0.formatted(date: .abbreviated, time: .shortened) } ?? ""))
                    if !game.entry.duplicates.isEmpty {
                        fact("Also at", game.entry.duplicates.joined(separator: "\n"))
                    }
                    if !game.envForce.isEmpty {
                        fact("Set by the title", game.envForce.sorted { $0.key < $1.key }
                            .map { "\($0.key)=\($0.value)" }.joined(separator: "  "))
                    }
                }
            }
        }
    }

    private func fact(_ k: String, _ v: String) -> some View {
        GridRow {
            Text(k).foregroundStyle(Theme.textFaint).gridColumnAlignment(.trailing)
            Text(v).foregroundStyle(Theme.text).textSelection(.enabled).lineLimit(4)
        }
        .font(.callout)
    }
}

/// The blurred title image behind Home's header, kept dark so text stays readable.
struct Hero: View {
    let game: GameItem

    var body: some View {
        ZStack {
            if let art = game.art {
                Image(nsImage: art).resizable().aspectRatio(contentMode: .fill)
                    .blur(radius: 60)
                    .saturation(1.2)
                    .opacity(0.55)
            } else {
                RadialGradient(colors: [Color(argb: TitleArt.monogramColor(game.titleId)).opacity(0.35), .clear],
                               center: .topLeading, startRadius: 10, endRadius: 600)
            }
            LinearGradient(colors: [Theme.bgTop.opacity(0.35), Theme.bgTop.opacity(0.85), Theme.bgBottom],
                           startPoint: .top, endPoint: .bottom)
        }
        .frame(height: 380)
        .frame(maxWidth: .infinity)
        .clipped()
        .allowsHitTesting(false)
    }
}

/// A run that ended badly: the problem and the log's tail.
struct ExitProblemView: View {
    @EnvironmentObject var model: LauncherModel
    let game: GameItem
    let problem: String

    var body: some View {
        Panel {
            VStack(alignment: .leading, spacing: Theme.s1) {
                Label(problem, systemImage: "exclamationmark.octagon.fill").foregroundStyle(Theme.error).font(.headline)
                if !game.lastExitTail.isEmpty {
                    LogLines(lines: game.lastExitTail, maxHeight: 160)
                }
                HStack {
                    Button("Show log") { model.route = .log(game.id) }.buttonStyle(PillButtonStyle(compact: true))
                    Button("Dismiss") { model.update(game.id) { $0.lastExit = nil } }.buttonStyle(PillButtonStyle(compact: true))
                }
            }
        }
    }
}

/// Monospaced lines in a scroll view, newest at the bottom.
struct LogLines: View {
    let lines: [String]
    var maxHeight: CGFloat = 240

    var body: some View {
        ScrollViewReader { proxy in
            ScrollView {
                LazyVStack(alignment: .leading, spacing: 1) {
                    ForEach(Array(lines.enumerated()), id: \.offset) { i, l in
                        Text(l.isEmpty ? " " : l)
                            .font(.system(size: 11, design: .monospaced))
                            .foregroundStyle(lineColor(l))
                            .frame(maxWidth: .infinity, alignment: .leading)
                            .id(i)
                    }
                }
                .textSelection(.enabled)
                .padding(10)
            }
            .frame(maxHeight: maxHeight)
            .background(RoundedRectangle(cornerRadius: 10).fill(Theme.bgBottom))
            .overlay(RoundedRectangle(cornerRadius: 10).strokeBorder(Theme.lineSoft))
            .onAppear { proxy.scrollTo(lines.count - 1, anchor: .bottom) }
            .onChange(of: lines.count) { _, n in proxy.scrollTo(n - 1, anchor: .bottom) }
        }
    }

    private func lineColor(_ l: String) -> Color {
        let s = l.lowercased()
        if s.contains("error") || s.contains("failed") || s.contains("traceback") { return Theme.error }
        if s.contains("warning") || s.hasPrefix("warn") { return Theme.warn }
        if l.hasPrefix("#") || l.hasPrefix("$") { return Theme.textFaint }
        return Theme.textDim
    }
}
