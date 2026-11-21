import AppKit
import SwiftUI
import XRecompCore

/// A prepare in progress: step i of 5 with its name, the overall bar, the step's percent and message, elapsed time,
/// Cancel and the log's tail.
struct PreparingView: View {
    @EnvironmentObject var model: LauncherModel
    let game: GameItem
    let progress: PrepareProtocol.Progress
    let started: Date
    @Local private var showLog = false

    var body: some View {
        Panel(padding: Theme.s3) {
            VStack(alignment: .leading, spacing: Theme.s2) {
                HStack(alignment: .firstTextBaseline) {
                    VStack(alignment: .leading, spacing: 4) {
                        Text("Step \(progress.step) of \(progress.n)")
                            .font(.caption.weight(.semibold)).tracking(1).foregroundStyle(Theme.textFaint)
                        Text(PrepareProtocol.stepTitle(progress.id))
                            .font(.title3.weight(.semibold)).foregroundStyle(Theme.text)
                    }
                    Spacer()
                    Text("\(Int((progress.overall * 100).rounded())) %")
                        .font(.system(size: 30, weight: .semibold, design: .rounded))
                        .monospacedDigit()
                        .foregroundStyle(game.accentColor)
                }
                ProgressView(value: progress.overall).progressViewStyle(ThinBar(color: game.accentColor, height: 8))
                StepDots(current: progress.step, steps: PrepareProtocol.steps, color: game.accentColor)
                HStack(spacing: Theme.s2) {
                    Text(progress.msg.isEmpty ? " " : progress.msg)
                        .font(.callout).foregroundStyle(Theme.textDim).lineLimit(1).truncationMode(.middle)
                    Spacer()
                    Text(elapsed).font(.callout.monospacedDigit()).foregroundStyle(Theme.textFaint).id(model.tick)
                    if progress.eta >= 0 {
                        Text("about \(PrepareProtocol.formatEta(progress.eta)) left")
                            .font(.callout).foregroundStyle(Theme.textFaint)
                    }
                }
                HStack(spacing: Theme.s1) {
                    Button { withAnimation { showLog.toggle() } } label: {
                        Label(showLog ? "Hide log" : "Show log", systemImage: "text.alignleft")
                    }
                    .buttonStyle(PillButtonStyle())
                    Spacer()
                    Text("The first prepare of a game takes many minutes. Cancel keeps what is done.")
                        .font(.caption).foregroundStyle(Theme.textFaint)
                }
                if showLog {
                    LogLines(lines: Array(model.liveLog.suffix(400)), maxHeight: 220)
                }
            }
        }
    }

    private var elapsed: String { PrepareProtocol.formatClock(Int(Date().timeIntervalSince(started))) }
}

/// The five steps as dots: done, current, to come.
struct StepDots: View {
    static let short = ["identify": "Read", "analyse": "Analyse", "project": "Project", "lift": "Translate",
                        "build": "Build"]
    let current: Int
    let steps: [String]
    let color: Color

    var body: some View {
        HStack(spacing: 0) {
            ForEach(Array(steps.enumerated()), id: \.offset) { i, id in
                let n = i + 1
                HStack(spacing: 6) {
                    ZStack {
                        Circle().fill(n < current ? color : (n == current ? color.opacity(0.25) : Theme.track))
                        if n < current {
                            Image(systemName: "checkmark").font(.system(size: 8, weight: .bold)).foregroundStyle(Theme.ink)
                        } else if n == current {
                            Circle().fill(color).frame(width: 6, height: 6)
                        }
                    }
                    .frame(width: 16, height: 16)
                    Text(StepDots.short[id] ?? id)
                        .font(.caption)
                        .foregroundStyle(n == current ? Theme.text : (n < current ? Theme.textDim : Theme.textFaint))
                        .lineLimit(1)
                }
                if n < steps.count {
                    Rectangle().fill(n < current ? color.opacity(0.6) : Theme.line).frame(height: 1)
                        .frame(minWidth: 8).padding(.horizontal, 6)
                }
            }
        }
    }
}

/// A failed prepare: what failed, the log's last lines, the ways out.
struct FailedView: View {
    @EnvironmentObject var model: LauncherModel
    let game: GameItem
    let code: String
    let message: String
    let tail: [String]
    let log: URL?

    var body: some View {
        Panel(padding: Theme.s3) {
            VStack(alignment: .leading, spacing: Theme.s2) {
                HStack(alignment: .firstTextBaseline, spacing: Theme.s1) {
                    Image(systemName: "exclamationmark.octagon.fill").foregroundStyle(Theme.error)
                    Text(PrepareProtocol.errorMessages[code] ?? "Prepare failed.")
                        .font(.headline).foregroundStyle(Theme.text)
                    Text(code).font(.caption.monospaced()).foregroundStyle(Theme.error)
                        .padding(.horizontal, 6).padding(.vertical, 2)
                        .background(Capsule().fill(Theme.error.opacity(0.12)))
                    Spacer()
                }
                if !message.isEmpty {
                    Text(message).font(.callout).foregroundStyle(Theme.textDim).textSelection(.enabled)
                }
                if !tail.isEmpty { LogLines(lines: tail, maxHeight: 260) }
                HStack(spacing: Theme.s1) {
                    if let log {
                        Button { model.actions?.reveal(log) } label: { Label("Reveal log in Finder", systemImage: "folder") }
                            .buttonStyle(PillButtonStyle())
                        Button {
                            let text = (try? String(contentsOf: log, encoding: .utf8)) ?? tail.joined(separator: "\n")
                            NSPasteboard.general.clearContents()
                            NSPasteboard.general.setString(text, forType: .string)
                        } label: { Label("Copy log", systemImage: "doc.on.doc") }
                        .buttonStyle(PillButtonStyle())
                    }
                    if model.reportsAvailable {
                        Button { model.actions?.draftReport(.prepare(game.id)) } label: {
                            Label("Send report", systemImage: "paperplane")
                        }
                        .buttonStyle(PillButtonStyle())
                        .help("Send this failure and its log to the XRecomp project (you see it first)")
                    }
                    Spacer()
                }
            }
        }
    }
}
