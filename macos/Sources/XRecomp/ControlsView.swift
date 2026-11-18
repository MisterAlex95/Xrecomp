import GameController
import SwiftUI
import XRecompCore

/// What the game gets from a pad (SDL's game controller mapping, src/input/xinput_device.c) and the keyboard, and
/// the pad connected now. Informational: the game reads the pad itself.
struct ControlsView: View {
    @EnvironmentObject var model: LauncherModel
    let gameId: String
    @Local private var pads: [String] = []

    private var console: Console { model.game(gameId)?.console ?? .xbox }

    /// The pad's buttons on the console's (src/input/console_pad.c console_pad_defaults; a game's own is
    /// RECOMP_<C>_PAD_MAP in Settings › Advanced, as the Android Controls screen writes it).
    private var mapping: [(String, String)] {
        switch console {
        case .xbox: return xboxMapping
        case .gba: return [("A or Y", "A"), ("B or X", "B"), ("Shoulder or trigger", "L / R"),
                           ("Menu / Start", "Start"), ("View / Select", "Select"), ("D-pad, left stick", "D-pad")]
        case .ps1: return [("A  B  X  Y", "Cross  Circle  Square  Triangle"), ("Shoulders", "L1 / R1"),
                           ("Triggers", "L2 / R2"), ("Menu / View", "Start / Select"), ("D-pad", "D-pad"),
                           ("Sticks (analog games)", "Sticks"), ("Press a stick", "L3 / R3")]
        case .n64: return [("A", "A"), ("B or X", "B"), ("Right stick (Y: C up)", "C buttons"),
                           ("Either trigger", "Z"), ("Shoulders", "L / R"), ("Menu / Start", "Start"),
                           ("D-pad", "D-pad"), ("Left stick", "Control stick")]
        case .gc: return [("A  B  X  Y", "A  B  X  Y"), ("Right shoulder", "Z"), ("Triggers", "L / R"),
                          ("Menu / Start", "Start"), ("D-pad", "D-pad"), ("Left stick", "Control stick"),
                          ("Right stick", "C stick")]
        }
    }

    /// The hotkeys (src/platform/recomp_hotkeys.c defaults; an Xbox title's state ones need its save states).
    private let hotkeys: [(String, String, String)] = [
        ("Fast-forward (hold)", "View + right trigger", "Tab"),
        ("Rewind (hold)", "View + left trigger", "R"),
        ("Quick save / quick load", "View + right / left shoulder", "F1 / Shift+F1"),
        ("Save / load slot 1 to 4", "", "F1-F4 / Shift+F1-F4"),
        ("Fullscreen", "", "F11 or Alt+Enter"),
        ("Screenshot", "", "F12"),
    ]

    private let xboxMapping: [(String, String)] = [
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
                SectionTitle(text: "Hotkeys")
                Panel(padding: 0) {
                    VStack(spacing: 0) {
                        ForEach(Array(hotkeys.enumerated()), id: \.offset) { i, h in
                            HStack {
                                Text(h.0).foregroundStyle(Theme.textDim)
                                Spacer()
                                Text([h.1, h.2].filter { !$0.isEmpty }.joined(separator: "  ·  "))
                                    .foregroundStyle(Theme.text).fontWeight(.medium)
                            }
                            .padding(.horizontal, Theme.s2).padding(.vertical, 10)
                            if i < hotkeys.count - 1 { Divider().overlay(Theme.lineSoft) }
                        }
                    }
                }
                SectionTitle(text: "Keyboard")
                Panel {
                    Text(console == .xbox
                         ? "The game gets the Mac's keyboard as an Xbox keyboard: only games that support one use it "
                           + "(typing names, for example). Play with a controller."
                         : "The keyboard plays too: the arrow keys, Z and X, Enter for Start (the console runtime's "
                           + "keys). A controller is better.")
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
