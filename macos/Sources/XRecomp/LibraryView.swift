import SwiftUI
import UniformTypeIdentifiers
import XRecompCore

/// The window: a sidebar (library, games, setup) and the detail pane the route picks; in a game app
/// (single-game mode) that game's screens alone (SingleTitleRoot).
struct RootView: View {
    @EnvironmentObject var model: LauncherModel

    var body: some View {
        Group {
            if model.singleTitle != nil {
                SingleTitleRoot()
            } else {
                splitView
            }
        }
        .preferredColorScheme(.dark)
        .tint(Theme.accent)
        .alert("XRecomp", isPresented: Binding(get: { model.notice != nil }, set: { if !$0 { model.notice = nil } })) {
            Button("OK") { model.notice = nil }
        } message: {
            Text(model.notice ?? "")
        }
    }

    private var splitView: some View {
        NavigationSplitView {
            Sidebar()
                .navigationSplitViewColumnWidth(min: 200, ideal: 230, max: 300)
        } detail: {
            ZStack {
                BackdropView(glow: glowColor)
                detail
                    .transition(.opacity)
            }
            .frame(minWidth: 640, minHeight: 520)
        }
        .onDrop(of: [.fileURL], isTargeted: nil) { providers in
            for p in providers {
                _ = p.loadObject(ofClass: URL.self) { url, _ in
                    if let url { Task { @MainActor in model.actions?.addGame(url) } }
                }
            }
            return true
        }
    }

    private var glowColor: Color {
        if let id = model.route.gameId, let g = model.game(id) { return g.accentColor }
        return Theme.accent
    }

    @ViewBuilder private var detail: some View {
        switch model.route {
        case .library: LibraryView()
        case .home(let id): if let g = model.game(id) { HomeView(game: g) } else { LibraryView() }
        case .settings(let id): SettingsView(gameId: id).id(id)
        case .controls(let id): ControlsView(gameId: id)
        case .manage(let id): ManageView(gameId: id).id(id)
        case .log(let id): LogView(gameId: id)
        case .setup: SetupView()
        case .defaults: SettingsView(gameId: nil)
        case .about: AboutView()
        }
    }
}

/// A game app's window (single-game mode, docs/macos/launcher.md « Game apps »): the game's Home, Settings,
/// Controls, Manage and Log, without the sidebar or the Library. Setup, Default settings and About open from Home's
/// « … » menu, each with a way back to Home. When the library does not have the title (any more): MissingTitleView.
struct SingleTitleRoot: View {
    @EnvironmentObject var model: LauncherModel

    var body: some View {
        ZStack {
            BackdropView(glow: model.singleGame?.accentColor ?? Theme.accent)
            detail
                .transition(.opacity)
        }
        .frame(minWidth: 640, minHeight: 520)
        .navigationTitle(model.singleGame?.name ?? "XRecomp")
    }

    @ViewBuilder private var detail: some View {
        switch model.route {
        case .settings(let id): SettingsView(gameId: id).id(id)
        case .controls(let id): ControlsView(gameId: id)
        case .manage(let id): ManageView(gameId: id).id(id)
        case .log(let id): LogView(gameId: id)
        case .setup: withBack { SetupView() }
        case .defaults: withBack { SettingsView(gameId: nil) }
        case .about: withBack { AboutView() }
        case .library, .home:
            if let g = model.singleGame { HomeView(game: g) } else { MissingTitleView() }
        }
    }

    private func withBack<V: View>(@ViewBuilder _ content: () -> V) -> some View {
        VStack(alignment: .leading, spacing: 0) {
            Button { model.route = model.homeRoute } label: {
                Label(model.singleGame?.name ?? "Back", systemImage: "chevron.left")
            }
            .buttonStyle(PillButtonStyle(compact: true))
            .keyboardShortcut("[", modifiers: .command)
            .padding([.horizontal, .top], Theme.s4)
            content()
        }
    }
}

/// A game app whose title the library does not have: removed, its disc image moved, or on a disk that is not
/// connected. The app holds nothing of the game, so it can only point back to the main XRecomp.
struct MissingTitleView: View {
    @EnvironmentObject var model: LauncherModel

    var body: some View {
        VStack {
            Spacer()
            Panel(padding: Theme.s4) {
                VStack(alignment: .leading, spacing: Theme.s2) {
                    if !model.scannedOnce || model.scanning {
                        HStack(spacing: Theme.s2) {
                            ProgressView().controlSize(.small)
                            Text("Looking for the game in your library…").foregroundStyle(Theme.textDim)
                        }
                    } else {
                        Image(systemName: "questionmark.folder").font(.system(size: 34)).foregroundStyle(Theme.warn)
                        Text("This game is not in your library").font(.title3.weight(.semibold))
                            .foregroundStyle(Theme.text)
                        Text("XRecomp found no game with title ID \(model.singleTitle ?? "?") in "
                             + folders + ". Its disc image or folder was moved or removed, or it is on a disk that "
                             + "is not connected. Add it again in XRecomp, then open this app again.")
                            .foregroundStyle(Theme.textDim)
                            .fixedSize(horizontal: false, vertical: true)
                        HStack(spacing: Theme.s1) {
                            Button("Open XRecomp") { model.actions?.openMainApp() }
                                .buttonStyle(PrimaryButtonStyle())
                            Button("Scan again") { model.actions?.rescan() }.buttonStyle(PillButtonStyle())
                            Button("Setup") { model.route = .setup }.buttonStyle(PillButtonStyle())
                        }
                    }
                }
            }
            .frame(maxWidth: 560)
            Spacer()
        }
        .padding(Theme.s4)
        .frame(maxWidth: .infinity)
    }

    private var folders: String {
        let f = model.setup.libraryFolders
        return f.isEmpty ? Settings.defaultLibrary : f.joined(separator: ", ")
    }
}

struct Sidebar: View {
    @EnvironmentObject var model: LauncherModel

    var body: some View {
        List(selection: Binding<Route?>(get: { sidebarRoute }, set: { if let r = $0 { model.route = r } })) {
            Section {
                Label("Library", systemImage: "square.grid.2x2").tag(Route.library)
            }
            Section("Games") {
                ForEach(model.games) { g in
                    HStack(spacing: 10) {
                        Thumb(game: g, size: 22, corner: 5)
                        Text(g.name).lineLimit(1)
                        Spacer(minLength: 4)
                        Circle().fill(g.statusLine.1).frame(width: 6, height: 6)
                    }
                    .tag(Route.home(g.id))
                }
            }
            Section("XRecomp") {
                Label("Setup", systemImage: setupIcon).tag(Route.setup)
                Label("Default settings", systemImage: "slider.horizontal.3").tag(Route.defaults)
                Label("About", systemImage: "info.circle").tag(Route.about)
            }
        }
        .listStyle(.sidebar)
        .safeAreaInset(edge: .bottom) {
            HStack(spacing: Theme.s1) {
                Button { pickGame() } label: { Label("Add game…", systemImage: "plus") }
                    .buttonStyle(PillButtonStyle(compact: true))
                Button { model.actions?.rescan() } label: {
                    if model.scanning { ProgressView().controlSize(.small) } else { Image(systemName: "arrow.clockwise") }
                }
                .buttonStyle(PillButtonStyle(compact: true))
                .help("Scan the library folders again")
                Spacer()
            }
            .padding(Theme.s1 + 4)
        }
    }

    private var setupIcon: String { model.setup.ready ? "checkmark.seal" : "exclamationmark.triangle" }

    /// A game's sub-screens keep its row selected.
    private var sidebarRoute: Route {
        if let id = model.route.gameId { return .home(id) }
        return model.route
    }

    private func pickGame() {
        let p = NSOpenPanel()
        p.canChooseDirectories = true
        p.canChooseFiles = true
        p.allowsMultipleSelection = false
        p.message = "Choose a disc image (.iso, .xiso, .cso, .zso, .cci) or a folder holding default.xbe"
        if p.runModal() == .OK, let u = p.url { model.actions?.addGame(u) }
    }
}

/// The grid of every game.
struct LibraryView: View {
    @EnvironmentObject var model: LauncherModel
    private let columns = [GridItem(.adaptive(minimum: 176, maximum: 230), spacing: Theme.s3)]

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: Theme.s3) {
                PageHeader(title: "Library", subtitle: subtitle) {
                    if !model.setup.ready {
                        Button { model.route = .setup } label: {
                            Label("Finish setup", systemImage: "exclamationmark.triangle.fill")
                        }
                        .buttonStyle(PillButtonStyle(tint: Theme.warn))
                    }
                }
                if model.games.isEmpty {
                    EmptyLibrary()
                } else {
                    LazyVGrid(columns: columns, alignment: .leading, spacing: Theme.s3) {
                        ForEach(model.games) { g in
                            Button { model.route = .home(g.id) } label: { GameCard(game: g) }
                                .buttonStyle(.plain)
                        }
                    }
                }
            }
            .padding(Theme.s4)
        }
    }

    private var subtitle: String {
        let n = model.games.count
        let ready = model.games.filter { $0.status == .ready }.count
        if model.scanning { return "Scanning…" }
        return n == 0 ? "No games yet" : "\(n) game\(n == 1 ? "" : "s") · \(ready) ready to play"
    }
}

struct EmptyLibrary: View {
    @EnvironmentObject var model: LauncherModel

    var body: some View {
        Panel(padding: Theme.s4) {
            VStack(alignment: .leading, spacing: Theme.s2) {
                Image(systemName: "opticaldisc").font(.system(size: 34)).foregroundStyle(Theme.accent)
                Text("No games found").font(.title3.weight(.semibold)).foregroundStyle(Theme.text)
                Text("XRecomp looks for disc images (.iso, .cso…) and folders holding default.xbe in "
                     + (model.setup.libraryFolders.first ?? Settings.defaultLibrary)
                     + ". Add a folder in Setup, drop a game on this window, or use Add game….")
                    .foregroundStyle(Theme.textDim)
                    .fixedSize(horizontal: false, vertical: true)
                Button("Open Setup") { model.route = .setup }.buttonStyle(PillButtonStyle())
            }
        }
        .frame(maxWidth: 560)
    }
}

/// The title image, else the monogram on the game's hue.
struct Thumb: View {
    let game: GameItem
    var size: CGFloat
    var corner: CGFloat = 10

    var body: some View {
        ZStack {
            if let art = game.art {
                Image(nsImage: art).resizable().interpolation(.high).aspectRatio(contentMode: .fill)
            } else {
                let c = Color(argb: TitleArt.monogramColor(game.titleId))
                LinearGradient(colors: [c.opacity(0.55), c.opacity(0.18)], startPoint: .topLeading, endPoint: .bottomTrailing)
                Text(TitleArt.initials(game.name))
                    .font(.system(size: size * 0.38, weight: .bold, design: .rounded))
                    .foregroundStyle(Theme.textBright)
            }
        }
        .frame(width: size, height: size)
        .clipShape(RoundedRectangle(cornerRadius: corner, style: .continuous))
    }
}

/// One game in the grid: art, title, status.
struct GameCard: View {
    @EnvironmentObject var model: LauncherModel
    let game: GameItem
    @Local private var hover = false

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            ZStack(alignment: .bottomLeading) {
                GeometryReader { geo in
                    Thumb(game: game, size: geo.size.width, corner: 10)
                }
                .aspectRatio(1, contentMode: .fit)
                if case .running(let p, _) = game.prepare {
                    ProgressView(value: p.overall)
                        .progressViewStyle(ThinBar(color: game.accentColor))
                        .padding(10)
                }
            }
            VStack(alignment: .leading, spacing: 4) {
                Text(game.name).font(.headline).foregroundStyle(Theme.text).lineLimit(1)
                StatusChip(text: game.statusLine.0, color: game.statusLine.1)
            }
        }
        .padding(10)
        .background(RoundedRectangle(cornerRadius: Theme.radius, style: .continuous)
            .fill(hover ? Theme.panelHi : Theme.panel))
        .overlay(RoundedRectangle(cornerRadius: Theme.radius, style: .continuous)
            .strokeBorder(hover ? game.accentColor.opacity(0.6) : Theme.line, lineWidth: 1))
        .scaleEffect(hover ? 1.015 : 1)
        .animation(.easeOut(duration: 0.15), value: hover)
        .onHover { hover = $0 }
        .contextMenu {
            Button("Open") { model.route = .home(game.id) }
            Button("Settings") { model.route = .settings(game.id) }
            Button("Manage storage") { model.route = .manage(game.id) }
            Divider()
            Button("Show source in Finder") { model.actions?.reveal(URL(fileURLWithPath: game.entry.path)) }
        }
    }
}

/// A thin capsule progress bar.
struct ThinBar: ProgressViewStyle {
    var color: Color = Theme.accent
    var height: CGFloat = 5

    func makeBody(configuration: Configuration) -> some View {
        GeometryReader { geo in
            ZStack(alignment: .leading) {
                Capsule().fill(Theme.track)
                Capsule().fill(color).frame(width: max(height, geo.size.width * (configuration.fractionCompleted ?? 0)))
            }
        }
        .frame(height: height)
    }
}
