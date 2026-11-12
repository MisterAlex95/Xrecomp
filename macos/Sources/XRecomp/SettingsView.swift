import SwiftUI
import XRecompCore

/// A title's settings (its recomp.cfg), or the defaults every title starts from (gameId nil): four tabs of curated
/// options, the Advanced tab with the hand-written lines. Every change is saved at once.
struct SettingsView: View {
    @EnvironmentObject var model: LauncherModel
    let gameId: String?
    @Local private var config = RecompConfig()
    @Local private var tab: ConfigOption.Section = SettingsView.initialTab
    /// The tab Settings opens on (--demo settings shows Graphics).
    static var initialTab: ConfigOption.Section = .general
    @Local private var advancedText = ""
    @Local private var loaded = false

    private var game: GameItem? { gameId.flatMap { model.game($0) } }

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: Theme.s3) {
                PageHeader(title: game.map { "\($0.name) · Settings" } ?? "Default settings",
                           subtitle: gameId == nil ? "Every game starts from these; a game's own settings win."
                                                   : "Saved to this game's recomp.cfg; used at the next Play.",
                           back: gameId.map { id in { model.route = .home(id) } }) {
                    Button("Reset to defaults") {
                        config.resetAll()
                        save()
                    }
                    .buttonStyle(PillButtonStyle())
                    .disabled(config.allDefault())
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
                            OptionRow(option: o, config: $config, forced: forcedValue(o), onChange: save)
                            if i < opts.count - 1 { Divider().overlay(Theme.lineSoft).padding(.leading, Theme.s2) }
                        }
                    }
                }
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
                 + "toolkit). Lines starting with # are comments. Reset to defaults keeps these.")
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
        advancedText = config.unmanagedText()
        loaded = true
    }

    private func save() {
        model.actions?.saveConfig(config, for: gameId)
        advancedText = config.unmanagedText()
    }
}

/// « Label     [value ▾] » with the option's description; a value the title forces shows « set by the title ».
struct OptionRow: View {
    let option: ConfigOption
    @Binding var config: RecompConfig
    let forced: String?
    let onChange: () -> Void

    var body: some View {
        HStack(alignment: .center, spacing: Theme.s2) {
            VStack(alignment: .leading, spacing: 3) {
                Text(option.title).foregroundStyle(enabled ? Theme.text : Theme.textFaint)
                Text(forced.map { "Set by the title (\($0)): this setting has no effect." } ?? option.detail)
                    .font(.caption)
                    .foregroundStyle(forced != nil ? Theme.warn : Theme.textFaint)
                    .fixedSize(horizontal: false, vertical: true)
            }
            Spacer(minLength: Theme.s2)
            Picker("", selection: selection) {
                ForEach(Array(option.choices.enumerated()), id: \.offset) { i, c in Text(c.label).tag(i) }
                if config.choiceIndex(option) == nil {
                    Text("Custom: \(config.rawValue(option))").tag(-1)
                }
            }
            .labelsHidden()
            .pickerStyle(.menu)
            .frame(width: 210)
            .disabled(!enabled || forced != nil)
        }
        .padding(.horizontal, Theme.s2)
        .padding(.vertical, 12)
    }

    private var enabled: Bool {
        guard let w = option.onlyWhen else { return true }
        return config.get(w.key) == w.value
    }

    private var selection: Binding<Int> {
        Binding(get: { config.choiceIndex(option) ?? -1 }, set: { i in
            guard i >= 0 else { return }
            config.set(option, option.choices[i])
            onChange()
        })
    }
}
