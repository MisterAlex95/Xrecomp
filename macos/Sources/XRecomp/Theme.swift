import SwiftUI
import XRecompCore

extension Color {
    /// An ARGB token (Palette) as a SwiftUI colour.
    init(argb: UInt32) {
        self.init(.sRGB, red: Double((argb >> 16) & 0xFF) / 255, green: Double((argb >> 8) & 0xFF) / 255,
                  blue: Double(argb & 0xFF) / 255, opacity: Double(argb >> 24) / 255)
    }
}

/// The launcher's look: every colour from Palette (GamePalette.STOCK of the Android launcher), dark only, an 8-pt
/// grid, the system font, capsule buttons.
enum Theme {
    static let bgTop = Color(argb: Palette.bgTop)
    static let bgBottom = Color(argb: Palette.bgBottom)
    static let panel = Color(argb: Palette.panel)
    static let panelHi = Color(argb: Palette.panelHi)
    static let line = Color(argb: Palette.line)
    static let lineSoft = Color(argb: Palette.lineSoft)
    static let text = Color(argb: Palette.text)
    static let textBright = Color(argb: Palette.textBright)
    static let textDim = Color(argb: Palette.textDim)
    static let textFaint = Color(argb: Palette.textFaint)
    static let accent = Color(argb: Palette.accent)
    static let accentDeep = Color(argb: Palette.accentDeep)
    static let accentFace = Color(argb: Palette.accentFace)
    static let track = Color(argb: Palette.track)
    static let ok = Color(argb: Palette.ok)
    static let warn = Color(argb: Palette.warn)
    static let error = Color(argb: Palette.error)
    static let glyphFace = Color(argb: Palette.glyphFace)
    static let ink = Color(argb: Palette.glyphInk)

    /// The 8-pt grid.
    static let s1: CGFloat = 8, s2: CGFloat = 16, s3: CGFloat = 24, s4: CGFloat = 32, s6: CGFloat = 48
    static let radius: CGFloat = 14

    static var background: LinearGradient {
        LinearGradient(colors: [bgTop, bgBottom], startPoint: .top, endPoint: .bottom)
    }

    static let mono = Font.system(.callout, design: .monospaced)
}

/// The app's background: the stock gradient, with an optional soft glow of the game's accent.
struct BackdropView: View {
    var glow: Color = Theme.accent
    var body: some View {
        ZStack {
            Theme.background
            RadialGradient(colors: [glow.opacity(0.10), .clear], center: .topLeading, startRadius: 0, endRadius: 700)
        }
        .ignoresSafeArea()
    }
}

/// A rounded glass panel.
struct Panel<Content: View>: View {
    var padding: CGFloat = Theme.s2
    @ViewBuilder var content: Content

    var body: some View {
        content
            .padding(padding)
            .frame(maxWidth: .infinity, alignment: .leading)
            .background(RoundedRectangle(cornerRadius: Theme.radius, style: .continuous).fill(Theme.panel))
            .overlay(RoundedRectangle(cornerRadius: Theme.radius, style: .continuous).strokeBorder(Theme.line, lineWidth: 1))
    }
}

/// A section title above a panel.
struct SectionTitle: View {
    let text: String
    var body: some View {
        Text(text.uppercased())
            .font(.caption.weight(.semibold))
            .tracking(1.2)
            .foregroundStyle(Theme.textFaint)
    }
}

/// The page header of a detail screen: back button, title, subtitle.
struct PageHeader<Trailing: View>: View {
    let title: String
    var subtitle: String? = nil
    var back: (() -> Void)? = nil
    @ViewBuilder var trailing: Trailing

    var body: some View {
        HStack(alignment: .center, spacing: Theme.s2) {
            if let back {
                Button(action: back) { Image(systemName: "chevron.left") }
                    .buttonStyle(PillButtonStyle(compact: true))
                    .keyboardShortcut("[", modifiers: .command)
                    .help("Back")
            }
            VStack(alignment: .leading, spacing: 2) {
                Text(title).font(.title2.weight(.semibold)).foregroundStyle(Theme.text)
                if let subtitle { Text(subtitle).font(.callout).foregroundStyle(Theme.textDim) }
            }
            Spacer()
            trailing
        }
    }
}

extension PageHeader where Trailing == EmptyView {
    init(title: String, subtitle: String? = nil, back: (() -> Void)? = nil) {
        self.init(title: title, subtitle: subtitle, back: back) { EmptyView() }
    }
}
