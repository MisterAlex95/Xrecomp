import Foundation

/// One row of the per-title Settings: a curated RECOMP_* option (docs/runtime/environment-variables.md) and its
/// choices. A choice holds one value per key; nil = no line (the runtime default).
public struct ConfigOption: Identifiable, Sendable {
    public enum Section: String, CaseIterable, Sendable {
        case general = "General", graphics = "Graphics", audio = "Audio", advanced = "Advanced"
    }

    public struct Choice: Sendable, Equatable {
        public let label: String
        public let values: [String?]

        public init(_ label: String, _ values: String?...) {
            self.label = label
            self.values = values
        }

        public var isDefault: Bool { values.allSatisfy { $0 == nil } }
    }

    public let id: String
    public let section: Section
    public let title: String
    public let detail: String
    public let keys: [String]
    public let choices: [Choice]
    /// The row only matters when this key has this value (the frame cap with the GPU renderer).
    public let onlyWhen: (key: String, value: String)?

    public init(_ id: String, _ section: Section, _ title: String, _ detail: String, keys: [String],
                onlyWhen: (key: String, value: String)? = nil, _ choices: [Choice]) {
        self.id = id
        self.section = section
        self.title = title
        self.detail = detail
        self.keys = keys
        self.choices = choices
        self.onlyWhen = onlyWhen
    }

    public var defaultIndex: Int { choices.firstIndex(where: \.isDefault) ?? 0 }
}

/// The options the Mac launcher offers: the Android table (RecompConfig.java) without the Android-only keys
/// (RECOMP_BIG_CORES, the shell self-test) and frame generation (desktop builds have it since XBA-187, but it needs
/// MoltenVK and a shader cache from the player's Lossless.dll, which this launcher cannot import yet:
/// tools/lsfg/lsfg_translate, then RECOMP_LSFG=1), plus the window options of the SDL presenter.
public enum OptionTable {
    typealias C = ConfigOption.Choice

    public static let all: [ConfigOption] = [
        ConfigOption("language", .general, "Language",
                     "The console language the game uses when its disc has it. System: the Mac's language.",
                     keys: ["RECOMP_LANGUAGE"],
                     [C("System", nil), C("English", "en"), C("Français", "fr"), C("Deutsch", "de"),
                      C("Español", "es"), C("Italiano", "it"), C("Português", "pt"), C("日本語", "ja"),
                      C("한국어", "ko"), C("中文", "zh")]),
        ConfigOption("fullscreen", .general, "Window", "Start in a window or fullscreen.",
                     keys: ["RECOMP_FULLSCREEN"], [C("Window", nil), C("Fullscreen", "1")]),
        ConfigOption("window-scale", .general, "Window size", "The window's size, a multiple of 640 x 480.",
                     keys: ["RECOMP_WINDOW_SCALE"],
                     [C("2x (default)", nil), C("1x", "1"), C("3x", "3"), C("4x", "4")]),
        ConfigOption("gpu", .graphics, "Renderer",
                     "The CPU rasteriser is the safe default. The GPU renderer (OpenGL 4.1) is faster but newer.",
                     keys: ["RECOMP_GPU"], [C("CPU", nil), C("GPU (OpenGL)", "1")]),
        ConfigOption("fps-cap", .graphics, "Frame cap",
                     "Auto: the game's own rate, or a steady lower one. A number: at most that many fps (GPU only).",
                     keys: ["RECOMP_GPU_FPS_CAP"], onlyWhen: ("RECOMP_GPU", "1"),
                     [C("Auto", nil), C("30", "30"), C("60", "60"), C("Off", "0")]),
        ConfigOption("gpu-scale", .graphics, "Internal resolution",
                     "Draws at a multiple of 640 x 480 (GPU renderer only). Fit: from the window's height.",
                     keys: ["RECOMP_GPU_SCALE"], onlyWhen: ("RECOMP_GPU", "1"),
                     [C("1x", nil), C("2x", "2"), C("3x", "3"), C("Fit", "auto")]),
        ConfigOption("aspect", .graphics, "Aspect ratio",
                     "Auto: 16:9 where the game draws wide frames, 4:3 elsewhere.",
                     keys: ["RECOMP_ASPECT"], [C("Auto", nil), C("4:3", "4:3"), C("16:9", "16:9")]),
        ConfigOption("vsync", .graphics, "VSync", "Waits for the display's refresh.",
                     keys: ["RECOMP_PRESENT_VSYNC"], [C("On", nil), C("Off", "0")]),
        ConfigOption("filtering", .graphics, "Texture filtering", "CPU rasteriser only. Smoother is slower.",
                     keys: ["RECOMP_TEX_BILINEAR", "RECOMP_TEX_ANISO"],
                     [C("Sharp", nil, nil), C("Smooth", "1", nil), C("Anisotropic 8x", nil, "8"),
                      C("Anisotropic 16x", nil, "16")]),
        ConfigOption("hrtf", .audio, "3D audio (HRTF)", "Positions 3D sounds for headphones.",
                     keys: ["RECOMP_APU_HRTF"], [C("On", nil), C("Off", "0")]),
        ConfigOption("audio-buffer", .audio, "Audio buffer", "Longer buffers crackle less but lag more.",
                     keys: ["RECOMP_AUDIO_BUF_SAMPLES"],
                     [C("Short", "256"), C("Normal", nil), C("Long", "1024"), C("Longest", "2048")]),
        ConfigOption("raster-threads", .advanced, "Raster threads",
                     "Threads drawing for the CPU rasteriser. Auto: the cores minus 4.",
                     keys: ["RECOMP_RASTER_THREADS"],
                     [C("Auto", nil), C("1", "1"), C("2", "2"), C("4", "4"), C("6", "6"), C("8", "8")]),
    ]

    public static func options(in section: ConfigOption.Section) -> [ConfigOption] {
        all.filter { $0.section == section }
    }

    public static func option(_ id: String) -> ConfigOption? { all.first { $0.id == id } }
}
