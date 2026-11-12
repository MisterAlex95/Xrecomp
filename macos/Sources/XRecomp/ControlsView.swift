import GameController
import SwiftUI
import XRecompCore

/// What the game gets from a pad (SDL's game controller mapping, src/input/xinput_device.c) and the keyboard, and
/// the pad connected now. Informational: the game reads the pad itself.
struct ControlsView: View {
    @EnvironmentObject var model: LauncherModel
    let gameId: String
    @Local private var pads: [String] = []

    private let mapping: [(String, String)] = [
        ("A  B  X  Y", "A  B  X  Y"),
        ("Left shoulder (L1 / LB)", "Black"),
        ("Right shoulder (R1 / RB)", "White"),
        ("Left / right trigger", "Left / right trigger"),
        ("Menu / Start", "Start"),
        ("View / Select / Share", "Back"),
        ("D-pad", "D-pad"),
        ("Sticks", "Sticks"),
        ("Press a stick", "Its thumbstick button"),
    ]

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: Theme.s3) {
                PageHeader(title: (model.game(gameId)?.name ?? "Game") + " · Controls",
                           subtitle: "Any controller macOS knows works: Xbox, PlayStation, Switch Pro and more.",
                           back: { model.route = .home(gameId) })
                Panel {
                    HStack(spacing: Theme.s2) {
                        Image(systemName: pads.isEmpty ? "gamecontroller" : "gamecontroller.fill")
                            .font(.title2)
                            .foregroundStyle(pads.isEmpty ? Theme.textFaint : Theme.accent)
                        VStack(alignment: .leading, spacing: 2) {
                            Text(pads.isEmpty ? "No controller connected" : pads.joined(separator: ", "))
                                .font(.headline).foregroundStyle(Theme.text)
                            Text(pads.isEmpty ? "Connect one by USB or Bluetooth (System Settings › Bluetooth)."
                                              : "Connected. The first one is player 1.")
                                .font(.callout).foregroundStyle(Theme.textDim)
                        }
                    }
                }
                SectionTitle(text: "Controller")
                Panel(padding: 0) {
                    VStack(spacing: 0) {
                        ForEach(Array(mapping.enumerated()), id: \.offset) { i, m in
                            HStack {
                                Text(m.0).foregroundStyle(Theme.textDim)
                                Spacer()
                                Text(m.1).foregroundStyle(Theme.text).fontWeight(.medium)
                            }
                            .padding(.horizontal, Theme.s2).padding(.vertical, 10)
                            if i < mapping.count - 1 { Divider().overlay(Theme.lineSoft) }
                        }
                    }
                }
                SectionTitle(text: "Keyboard")
                Panel {
                    Text("The game gets the Mac's keyboard as an Xbox keyboard: only games that support one use it "
                         + "(typing names, for example). Play with a controller.")
                        .foregroundStyle(Theme.textDim)
                        .fixedSize(horizontal: false, vertical: true)
                }
            }
            .padding(Theme.s4)
            .frame(maxWidth: 720, alignment: .leading)
            .frame(maxWidth: .infinity, alignment: .leading)
        }
        .onAppear(perform: refresh)
        .onReceive(NotificationCenter.default.publisher(for: .GCControllerDidConnect)) { _ in refresh() }
        .onReceive(NotificationCenter.default.publisher(for: .GCControllerDidDisconnect)) { _ in refresh() }
    }

    private func refresh() {
        if model.demo {
            pads = ["Xbox Wireless Controller"]
            return
        }
        pads = GCController.controllers().map { $0.vendorName ?? "Controller" }
    }
}
