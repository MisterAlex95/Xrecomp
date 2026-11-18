import AppKit
import SwiftUI
import XRecompCore

/// Setup: the toolkit checkout, what this Mac has (each missing item with its command), the Python environment,
/// the library folders and where work folders go.
struct SetupView: View {
    @EnvironmentObject var model: LauncherModel

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: Theme.s3) {
                PageHeader(title: "Setup", subtitle: model.setup.ready ? "Everything is in place." : "A few things are missing.") {
                    Button { model.actions?.recheckSetup() } label: {
                        if model.setup.checking { ProgressView().controlSize(.small) } else { Label("Check again", systemImage: "arrow.clockwise") }
                    }
                    .buttonStyle(PillButtonStyle())
                }
                toolkit
                dependencies
                python
                library
            }
            .padding(Theme.s4)
            .frame(maxWidth: 860, alignment: .leading)
            .frame(maxWidth: .infinity, alignment: .leading)
        }
    }

    private var toolkit: some View {
        VStack(alignment: .leading, spacing: Theme.s1) {
            SectionTitle(text: "Toolkit")
            Panel {
                HStack(spacing: Theme.s2) {
                    statusIcon(model.setup.toolkit != nil)
                    VStack(alignment: .leading, spacing: 2) {
                        Text(model.setup.toolkit ?? "Not found").foregroundStyle(Theme.text).textSelection(.enabled)
                        Text(model.setup.toolkit == nil
                             ? "Choose your xboxrecomp-android checkout (the folder with tools/ and templates/)."
                             : "Found from \(model.setup.toolkitSource ?? "?")"
                               + (model.setup.toolkitCommit.map { " · commit \($0.prefix(10))" } ?? ""))
                            .font(.caption).foregroundStyle(Theme.textFaint)
                    }
                    Spacer()
                    Button("Choose…") {
                        if let p = pickFolder("Choose the xboxrecomp-android checkout") { model.actions?.setToolkit(p) }
                    }
                    .buttonStyle(PillButtonStyle())
                }
            }
        }
    }

    private var dependencies: some View {
        VStack(alignment: .leading, spacing: Theme.s1) {
            SectionTitle(text: "This Mac")
            Panel(padding: 0) {
                VStack(spacing: 0) {
                    ForEach(Array(model.setup.dependencies.enumerated()), id: \.element.id) { i, d in
                        HStack(alignment: .top, spacing: Theme.s2) {
                            statusIcon(d.ok, optional: d.optional)
                            VStack(alignment: .leading, spacing: 3) {
                                Text(d.name + (d.optional ? " · optional" : "")).foregroundStyle(Theme.text)
                                Text(d.detail).font(.caption).foregroundStyle(Theme.textFaint).lineLimit(1)
                                if !d.ok {
                                    HStack(spacing: Theme.s1) {
                                        Text(d.fix).font(.caption.monospaced()).foregroundStyle(Theme.warn)
                                            .textSelection(.enabled)
                                        Button { copy(d.fix) } label: { Image(systemName: "doc.on.doc") }
                                            .buttonStyle(.plain).foregroundStyle(Theme.textDim).help("Copy the command")
                                    }
                                }
                            }
                            Spacer()
                        }
                        .padding(.horizontal, Theme.s2).padding(.vertical, 10)
                        if i < model.setup.dependencies.count - 1 { Divider().overlay(Theme.lineSoft) }
                    }
                    if model.setup.dependencies.isEmpty {
                        Text(model.setup.checking ? "Checking…" : "Not checked yet").foregroundStyle(Theme.textDim)
                            .padding(Theme.s2).frame(maxWidth: .infinity, alignment: .leading)
                    }
                }
            }
            Text("XRecomp never installs anything with Homebrew itself: run the commands in Terminal, then Check again.")
                .font(.caption).foregroundStyle(Theme.textFaint)
        }
    }

    private var python: some View {
        VStack(alignment: .leading, spacing: Theme.s1) {
            SectionTitle(text: "Python environment")
            Panel {
                VStack(alignment: .leading, spacing: Theme.s1) {
                    HStack(spacing: Theme.s2) {
                        statusIcon(model.setup.python != nil)
                        VStack(alignment: .leading, spacing: 2) {
                            Text(model.setup.python ?? "None with capstone, pefile and numpy yet").foregroundStyle(Theme.text)
                                .textSelection(.enabled)
                            Text("The toolkit's .venv when it has the packages, else XRecomp's own in Application Support.")
                                .font(.caption).foregroundStyle(Theme.textFaint)
                        }
                        Spacer()
                        if model.setup.python == nil {
                            Button { model.actions?.createVenv() } label: {
                                if model.setup.creatingVenv { ProgressView().controlSize(.small) } else { Text("Create it") }
                            }
                            .buttonStyle(PrimaryButtonStyle())
                            .disabled(model.setup.creatingVenv)
                        }
                    }
                    if !model.setup.venvLog.isEmpty { LogLines(lines: model.setup.venvLog, maxHeight: 140) }
                }
            }
        }
    }

    private var library: some View {
        VStack(alignment: .leading, spacing: Theme.s1) {
            SectionTitle(text: "Library folders")
            Panel(padding: 0) {
                VStack(spacing: 0) {
                    ForEach(model.setup.libraryFolders, id: \.self) { f in
                        HStack {
                            Image(systemName: "folder").foregroundStyle(Theme.textDim)
                            Text(f).foregroundStyle(Theme.text).textSelection(.enabled)
                            Spacer()
                            Button { model.actions?.reveal(URL(fileURLWithPath: (f as NSString).expandingTildeInPath)) } label: {
                                Image(systemName: "arrow.up.forward.square")
                            }
                            .buttonStyle(PillButtonStyle(compact: true))
                            Button { model.actions?.removeLibraryFolder(f) } label: { Image(systemName: "minus") }
                                .buttonStyle(PillButtonStyle(compact: true))
                                .disabled(model.setup.libraryFolders.count <= 1)
                        }
                        .padding(.horizontal, Theme.s2).padding(.vertical, 8)
                        Divider().overlay(Theme.lineSoft)
                    }
                    HStack {
                        Button { if let p = pickFolder("Add a folder XRecomp scans for games") { model.actions?.addLibraryFolder(p) } } label: {
                            Label("Add folder…", systemImage: "plus")
                        }
                        .buttonStyle(PillButtonStyle(compact: true))
                        Spacer()
                        Text("Scanned 3 levels deep for Xbox disc images and folders with default.xbe, and GBA, PS1, N64 and GameCube games.")
                            .font(.caption).foregroundStyle(Theme.textFaint)
                    }
                    .padding(Theme.s2)
                }
            }
            SectionTitle(text: "Work folders").padding(.top, Theme.s1)
            Panel {
                HStack {
                    VStack(alignment: .leading, spacing: 2) {
                        Text(model.setup.workRoot ?? (model.setup.appHome + "/titles/<title ID>/work"))
                            .foregroundStyle(Theme.text).textSelection(.enabled)
                        Text("About 2 GB per prepared game: analysis, generated code and the build. Disc images are read where they are.")
                            .font(.caption).foregroundStyle(Theme.textFaint)
                    }
                    Spacer()
                    Button("Change…") { if let p = pickFolder("Where should work folders go?") { model.actions?.setWorkRoot(p) } }
                        .buttonStyle(PillButtonStyle(compact: true))
                    if model.setup.workRoot != nil {
                        Button("Default") { model.actions?.setWorkRoot(nil) }.buttonStyle(PillButtonStyle(compact: true))
                    }
                }
            }
        }
    }

    private func statusIcon(_ ok: Bool, optional: Bool = false) -> some View {
        Image(systemName: ok ? "checkmark.circle.fill" : (optional ? "minus.circle" : "xmark.circle.fill"))
            .foregroundStyle(ok ? Theme.ok : (optional ? Theme.textFaint : Theme.error))
            .font(.title3)
    }

    private func copy(_ s: String) {
        NSPasteboard.general.clearContents()
        NSPasteboard.general.setString(s, forType: .string)
    }
}

/// A folder picker; the chosen path or nil.
@MainActor
func pickFolder(_ message: String) -> String? {
    let p = NSOpenPanel()
    p.canChooseDirectories = true
    p.canChooseFiles = false
    p.allowsMultipleSelection = false
    p.message = message
    return p.runModal() == .OK ? p.url?.path : nil
}
