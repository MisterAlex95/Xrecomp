import SwiftUI
import XRecompCore

/// The quiet capsule button: panel fill, a hairline, text colour; brighter on hover.
struct PillButtonStyle: ButtonStyle {
    var compact = false
    var tint: Color = Theme.text

    func makeBody(configuration: Configuration) -> some View {
        StyledBody(configuration: configuration, compact: compact, tint: tint)
    }

    struct StyledBody: View {
        let configuration: Configuration
        let compact: Bool
        let tint: Color
        @Local private var hover = false
        @Environment(\.isEnabled) private var enabled

        var body: some View {
            configuration.label
                .font(.callout.weight(.medium))
                .foregroundStyle(enabled ? tint : Theme.textFaint)
                .padding(.horizontal, compact ? 10 : Theme.s2)
                .padding(.vertical, compact ? 6 : 8)
                .background(Capsule().fill(hover && enabled ? Theme.line : Theme.panelHi))
                .overlay(Capsule().strokeBorder(Theme.line, lineWidth: 1))
                .opacity(configuration.isPressed ? 0.75 : 1)
                .contentShape(Capsule())
                .onHover { hover = $0 }
        }
    }
}

/// The solid action: Play / Prepare. The game's accent, the ink colour on it (contrast checked: Accent).
struct PrimaryButtonStyle: ButtonStyle {
    var color: Color = Theme.accent

    func makeBody(configuration: Configuration) -> some View {
        StyledBody(configuration: configuration, color: color)
    }

    struct StyledBody: View {
        let configuration: Configuration
        let color: Color
        @Local private var hover = false
        @Environment(\.isEnabled) private var enabled

        var body: some View {
            configuration.label
                .font(.headline)
                .foregroundStyle(Theme.ink)
                .padding(.horizontal, Theme.s3)
                .padding(.vertical, 10)
                .background(Capsule().fill(enabled ? color : Theme.textFaint))
                .brightness(hover && enabled ? 0.06 : 0)
                .shadow(color: color.opacity(enabled ? 0.35 : 0), radius: hover ? 14 : 8, y: 2)
                .opacity(configuration.isPressed ? 0.85 : 1)
                .contentShape(Capsule())
                .onHover { hover = $0 }
        }
    }
}

/// A small status chip: a dot and a word.
struct StatusChip: View {
    let text: String
    let color: Color

    var body: some View {
        HStack(spacing: 6) {
            Circle().fill(color).frame(width: 7, height: 7)
            Text(text).font(.caption.weight(.medium)).foregroundStyle(Theme.textDim).lineLimit(1)
        }
    }
}
