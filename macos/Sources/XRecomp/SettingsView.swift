import AppKit
import SwiftUI
import UniformTypeIdentifiers
import XRecompCore

/// A title's settings (its recomp.cfg over Default settings, GameSettings: « As in Default settings » or the title's
/// own value, XBA-188), or the defaults every title starts from (gameId nil): four tabs of curated options, the
/// Advanced tab with the hand-written lines, frame generation's Lossless.dll under Graphics. Every change is saved at
/// once.
struct SettingsView: View {
    @EnvironmentObject var model: LauncherModel
    let gameId: String?
    @Local private var config = RecompConfig()
    /// Default settings, for a title's rows (« As in Default settings (value) »).
    @Local private var defaults = RecompConfig()
    @Local private var tab: ConfigOption.Section = SettingsView.initialTab
    /// The tab Settings opens on (--demo settings shows Graphics).
    static var initialTab: ConfigOption.Section = .general
    @Local private var advancedText = ""
    @Local private var loaded = false

    private var game: GameItem? { gameId.flatMap { model.game($0) } }

    private var subtitle: String {
        if gameId == nil { return "Every game starts from these; a game's own settings win." }
        return "Each setting follows Default settings or has this game's own value. Saved to this game's recomp.cfg; "
            + "used at the next Play."
    }

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: Theme.s3) {
                PageHeader(title: game.map { "\($0.name) · Settings" } ?? "Default settings",
                           subtitle: subtitle,
                           back: gameId.map { id in { model.route = .home(id) } }) {
                    if gameId == nil {
                        Button("Reset to defaults") {
                            config.resetAll()
                            save()
                        }
                        .buttonStyle(PillButtonStyle())
                        .disabled(config.allDefault())
                    } else {
                        Button("Reset to Default settings") {
                            var g = GameSettings(game: config, defaults: defaults)
                            g.followAll()
                            config = g.game
                            save()
                        }
                        .buttonStyle(PillButtonStyle())
                        .disabled(GameSettings(game: config, defaults: defaults).allFollow())
                    }
                }
                Picker("", selection: $tab) {
                    ForEach(ConfigOption.Section.allCases, id: \.self) { Text($0.rawValue).tag($0) }
                }
                .pickerStyle(.segmented)
                .labelsHidden()
                .frame(maxWidth: 420)

                Panel(padding: 0) {
                    VStack(spacing: 0) {
                        let opts = OptionTable.options(in: tab)
                        ForEach(Array(opts.enumerated()), id: \.element.id) { i, o in
                            OptionRow(option: o, config: $config, defaults: gameId == nil ? nil : defaults,
                                      forced: forcedValue(o), onChange: save)
                            if i < opts.count - 1 { Divider().overlay(Theme.lineSoft).padding(.leading, Theme.s2) }
                        }
                    }
                }
                if tab == .graphics { FrameGenPanel() }
                if tab == .advanced { advanced }
            }
            .padding(Theme.s4)
            .frame(maxWidth: 820, alignment: .leading)
            .frame(maxWidth: .infinity, alignment: .leading)
        }
        .onAppear(perform: load)
    }

    private var advanced: some View {
        VStack(alignment: .leading, spacing: Theme.s1) {
            SectionTitle(text: "Your own lines")
            Text("Any other RECOMP_* variable, one KEY=VALUE per line (docs/runtime/environment-variables.md in the "
                 + "toolkit). Lines starting with # are comments. Reset keeps these.")
                .font(.callout).foregroundStyle(Theme.textDim)
            TextEditor(text: $advancedText)
                .font(Theme.mono)
                .scrollContentBackground(.hidden)
                .padding(8)
                .frame(minHeight: 160)
                .background(RoundedRectangle(cornerRadius: 10).fill(Theme.bgBottom))
                .overlay(RoundedRectangle(cornerRadius: 10).strokeBorder(Theme.line))
            HStack {
                Button("Save lines") {
                    config.setUnmanagedText(advancedText)
                    save()
                }
                .buttonStyle(PillButtonStyle())
                .disabled(advancedText == config.unmanagedText())
                if let id = gameId, let p = model.actions?.titlePaths(id) {
                    Button("Reveal recomp.cfg") { model.actions?.reveal(p.recompCfg) }.buttonStyle(PillButtonStyle())
                }
            }
        }
    }

    private func forcedValue(_ o: ConfigOption) -> String? {
        guard let g = game else { return nil }
        let vals = o.keys.compactMap { k in g.envForce[k].map { "\(k)=\($0)" } }
        return vals.isEmpty ? nil : vals.joined(separator: " ")
    }

    private func load() {
        guard !loaded else { return }
        config = model.actions?.loadConfig(gameId) ?? RecompConfig()
        if gameId != nil { defaults = model.actions?.loadConfig(nil) ?? RecompConfig() }
        advancedText = config.unmanagedText()
        loaded = true
    }

    private func save() {
        model.actions?.saveConfig(config, for: gameId)
        advancedText = config.unmanagedText()
    }
}

/// « Label     [value ▾] » with the option's description; a value the title forces shows « set by the title ».
/// For a title (defaults given): « As in Default settings (value) » first, then each choice as the title's own,
/// with a « This game » badge when the row is the title's own.
struct OptionRow: View {
    let option: ConfigOption
    @Binding var config: RecompConfig
    /// Default settings when this row is a title's; nil on the Default settings screen.
    let defaults: RecompConfig?
    let forced: String?
    let onChange: () -> Void

    /// The picker's tag for « As in Default settings ».
    private static let follow = -2
    /// The picker's tag for values the list does not offer.
    private static let custom = -1

    private var layered: GameSettings? { defaults.map { GameSettings(game: config, defaults: $0) } }

    var body: some View {
        HStack(alignment: .center, spacing: Theme.s2) {
            VStack(alignment: .leading, spacing: 3) {
                HStack(spacing: 6) {
                    Text(option.title).foregroundStyle(enabled ? Theme.text : Theme.textFaint)
                    if let l = layered, !l.follows(option) {
                        Text("This game")
                            .font(.caption2.weight(.semibold))
                            .foregroundStyle(Theme.accent)
                            .padding(.horizontal, 6)
                            .padding(.vertical, 1)
                            .overlay(Capsule().strokeBorder(Theme.accent.opacity(0.6)))
                    }
                }
                Text(forced.map { "Set by the title (\($0)): this setting has no effect." } ?? option.detail)
                    .font(.caption)
                    .foregroundStyle(forced != nil ? Theme.warn : Theme.textFaint)
                    .fixedSize(horizontal: false, vertical: true)
            }
            Spacer(minLength: Theme.s2)
            Picker("", selection: selection) {
                if let l = layered {
                    Text("As in Default settings (\(l.defaultsLabel(option)))").tag(OptionRow.follow)
                    Divider()
                }
                ForEach(Array(option.choices.enumerated()), id: \.offset) { i, c in Text(c.label).tag(i) }
                if showsCustom {
                    Text("Custom: \(config.rawValue(option))").tag(OptionRow.custom)
                }
            }
            .labelsHidden()
            .pickerStyle(.menu)
            .frame(width: layered == nil ? 210 : 260)
            .disabled(!enabled || forced != nil)
        }
        .padding(.horizontal, Theme.s2)
        .padding(.vertical, 12)
    }

    private var showsCustom: Bool {
        if let l = layered { return !l.follows(option) && l.ownChoiceIndex(option) == nil }
        return config.choiceIndex(option) == nil
    }

    /// The row matters (onlyWhen: the value the game will get, Default settings' when the title follows them).
    private var enabled: Bool {
        guard let w = option.onlyWhen else { return true }
        if let l = layered { return l.effective(w.key) == w.value }
        return config.get(w.key) == w.value
    }

    private var selection: Binding<Int> {
        Binding(get: {
            if let l = layered { return l.follows(option) ? OptionRow.follow : (l.ownChoiceIndex(option) ?? OptionRow.custom) }
            return config.choiceIndex(option) ?? OptionRow.custom
        }, set: { i in
            guard i >= 0 || (i == OptionRow.follow && layered != nil) else { return }
            if var l = layered {
                if i == OptionRow.follow { l.follow(option) } else { l.setOwn(option, option.choices[i]) }
                config = l.game
            } else {
                config.set(option, option.choices[i])
            }
            onChange()
        })
    }
}

/// Settings › Graphics: the shader cache frame generation needs, filled from the player's own Lossless.dll
/// (LsfgCache: lsfg_translate, built once from the toolkit). Shared by every game.
struct FrameGenPanel: View {
    @EnvironmentObject var model: LauncherModel
    @Local private var showLog = false

    private var explanation: String {
        let base = model.lsfg.base.isEmpty ? "the cache" : model.lsfg.base
        return "Frame generation uses the shaders of your own copy of Lossless Scaling (Steam): pick its Lossless.dll. "
            + "They are translated once into \(base) and never leave this Mac. The first time, the translator is "
            + "built from the toolkit (a few minutes)."
    }

    var body: some View {
        VStack(alignment: .leading, spacing: Theme.s1) {
            SectionTitle(text: "Frame generation · Lossless.dll")
            Panel {
                VStack(alignment: .leading, spacing: Theme.s1) {
                    HStack(spacing: 8) {
                        Image(systemName: model.lsfg.status.ready ? "checkmark.circle.fill" : "minus.circle")
                            .foregroundStyle(model.lsfg.status.ready ? Theme.ok : Theme.textFaint)
                        Text(model.lsfg.status.summary).foregroundStyle(Theme.text)
                    }
                    Text(explanation)
                        .font(.caption).foregroundStyle(Theme.textFaint)
                        .fixedSize(horizontal: false, vertical: true)
                    if model.lsfg.vulkan == nil {
                        Text("No Vulkan loader found: frame generation stays off until MoltenVK is installed "
                             + "(brew install molten-vk vulkan-loader).")
                            .font(.caption).foregroundStyle(Theme.warn)
                            .fixedSize(horizontal: false, vertical: true)
                    }
                    if let busy = model.lsfg.busy {
                        HStack(spacing: 8) {
                            ProgressView().controlSize(.small)
                            Text(busy).font(.callout).foregroundStyle(Theme.textDim)
                        }
                    }
                    if let e = model.lsfg.error {
                        Text(e).font(.caption).foregroundStyle(Theme.error)
                            .textSelection(.enabled)
                            .fixedSize(horizontal: false, vertical: true)
                    }
                    HStack {
                        Button("Choose Lossless.dll…") {
                            if let url = pickLosslessDll() { model.actions?.importLossless(url) }
                        }
                        .buttonStyle(PillButtonStyle())
                        .disabled(model.lsfg.busy != nil)
                        if let dir = model.lsfg.status.dir {
                            Button("Reveal cache") { model.actions?.reveal(dir) }.buttonStyle(PillButtonStyle())
                        }
                        if !model.lsfg.log.isEmpty {
                            Button(showLog ? "Hide log" : "Show log") { showLog.toggle() }
                                .buttonStyle(PillButtonStyle())
                        }
                    }
                    if showLog && !model.lsfg.log.isEmpty {
                        ScrollView {
                            Text(model.lsfg.log.joined(separator: "\n"))
                                .font(Theme.mono)
                                .foregroundStyle(Theme.textDim)
                                .textSelection(.enabled)
                                .frame(maxWidth: .infinity, alignment: .leading)
                        }
                        .frame(maxHeight: 200)
                    }
                }
            }
        }
        .onAppear { model.actions?.refreshLsfg() }
    }
}

/// A file picker for Lossless.dll; the chosen file or nil.
@MainActor
func pickLosslessDll() -> URL? {
    let p = NSOpenPanel()
    p.canChooseFiles = true
    p.canChooseDirectories = false
    p.allowsMultipleSelection = false
    if let dll = UTType(filenameExtension: "dll") { p.allowedContentTypes = [dll] }
    p.message = "Choose Lossless.dll from your Lossless Scaling install"
    return p.runModal() == .OK ? p.url : nil
}
