import SwiftUI
import XRecompCore

/// « Send a report » (XBA-342): where it goes and what it holds, in four lines, before anything leaves. Send posts
/// it (AppStore.sendReport), Cancel or Escape sends nothing.
struct ReportSheet: View {
    @EnvironmentObject var model: LauncherModel
    let draft: ReportDraft

    var body: some View {
        VStack(alignment: .leading, spacing: Theme.s2) {
            Text("Send a report").font(.title2.bold()).foregroundStyle(Theme.textBright)
            Text("It goes to the XRecomp project's crash tracker (\(draft.server)) to help fix the problem. This is "
                 + "all it holds: no name, no e-mail, no identifier, and the folders in the log are removed. Nothing "
                 + "leaves until you press Send.")
                .foregroundStyle(Theme.textDim)
                .fixedSize(horizontal: false, vertical: true)
            Text(draft.report.summary())
                .font(.system(size: 12, design: .monospaced))
                .foregroundStyle(Theme.text)
                .textSelection(.enabled)
                .frame(maxWidth: .infinity, alignment: .leading)
                .padding(Theme.s1 + 4)
                .background(RoundedRectangle(cornerRadius: 10).fill(Theme.bgBottom))
                .overlay(RoundedRectangle(cornerRadius: 10).strokeBorder(Theme.lineSoft))
            HStack(spacing: Theme.s1) {
                Spacer()
                Button("Cancel") { model.report = nil }
                    .keyboardShortcut(.cancelAction)
                Button("Send") { model.actions?.sendReport(draft.report) }
                    .keyboardShortcut(.defaultAction)
            }
        }
        .padding(Theme.s3)
        .frame(width: 560)
        .background(Theme.bgTop)
        .preferredColorScheme(.dark)
    }
}
