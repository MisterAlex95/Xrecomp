import AppKit
import SwiftUI
import XRecompCore

/// About: version, toolkit commit, what is (not) included, and the licence texts read from the toolkit.
struct AboutView: View {
    @EnvironmentObject var model: LauncherModel
    @Local private var licences: [(String, String)] = []
    @Local private var open: String?

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: Theme.s3) {
                HStack(spacing: Theme.s3) {
                    Image(nsImage: NSApp.applicationIconImage).resizable().frame(width: 88, height: 88)
                    VStack(alignment: .leading, spacing: 4) {
                        Text("XRecomp").font(.system(size: 30, weight: .bold)).foregroundStyle(Theme.textBright)
                        Text("Version \(version)").foregroundStyle(Theme.textDim)
                        Text("Toolkit " + (model.setup.toolkitCommit.map { String($0.prefix(12)) } ?? "not found"))
                            .font(.callout.monospaced()).foregroundStyle(Theme.textFaint).textSelection(.enabled)
                    }
                }
                Panel {
                    VStack(alignment: .leading, spacing: Theme.s1) {
                        Text("No game data included").font(.headline).foregroundStyle(Theme.text)
                        Text("XRecomp prepares games from your own copies. Title images, extracted discs, generated "
                             + "code and logs stay on this Mac, in ~/Library/Application Support/xboxrecomp. Nothing is "
                             + "uploaded, except a report you choose to send: one log, its folders removed. The app "
                             + "bundles no third-party code.")
                            .foregroundStyle(Theme.textDim).fixedSize(horizontal: false, vertical: true)
                    }
                }
                if model.reportsAvailable {
                    Panel {
                        VStack(alignment: .leading, spacing: Theme.s1) {
                            Text("A game crashed?").font(.headline).foregroundStyle(Theme.text)
                            Text("Send its last run's log to the XRecomp project. You see what it holds before it "
                                 + "leaves.")
                                .foregroundStyle(Theme.textDim).fixedSize(horizontal: false, vertical: true)
                            HStack(spacing: Theme.s1) {
                                Button { model.actions?.draftReport(.lastGame) } label: {
                                    Label("Send a report about the last game", systemImage: "paperplane")
                                }
                                .buttonStyle(PillButtonStyle())
                                .disabled(model.runningGame != nil)
                                if let g = model.runningGame {
                                    Text("\(g.name) is running: quit it first.")
                                        .font(.caption).foregroundStyle(Theme.textFaint)
                                }
                            }
                        }
                    }
                }
                SectionTitle(text: "Licences")
                Panel(padding: 0) {
                    VStack(spacing: 0) {
                        if licences.isEmpty {
                            Text("The licence texts are read from the toolkit, which was not found.")
                                .foregroundStyle(Theme.textDim).padding(Theme.s2)
                                .frame(maxWidth: .infinity, alignment: .leading)
                        }
                        ForEach(licences, id: \.0) { name, text in
                            Button { withAnimation { open = open == name ? nil : name } } label: {
                                HStack {
                                    Image(systemName: open == name ? "chevron.down" : "chevron.right").frame(width: 14)
                                    Text(name).foregroundStyle(Theme.text)
                                    Spacer()
                                    Text("\(text.split(separator: "\n").count) lines").font(.caption)
                                        .foregroundStyle(Theme.textFaint)
                                }
                                .contentShape(Rectangle())
                            }
                            .buttonStyle(.plain)
                            .padding(.horizontal, Theme.s2).padding(.vertical, 10)
                            if open == name {
                                ScrollView {
                                    Text(text).font(.system(size: 11, design: .monospaced)).foregroundStyle(Theme.textDim)
                                        .textSelection(.enabled).frame(maxWidth: .infinity, alignment: .leading)
                                        .padding(Theme.s2)
                                }
                                .frame(maxHeight: 280)
                                .background(Theme.bgBottom)
                            }
                            Divider().overlay(Theme.lineSoft)
                        }
                    }
                }
            }
            .padding(Theme.s4)
            .frame(maxWidth: 820, alignment: .leading)
            .frame(maxWidth: .infinity, alignment: .leading)
        }
        .onAppear { licences = model.actions?.licences() ?? [] }
    }

    private var version: String {
        (Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String) ?? "dev"
    }
}
