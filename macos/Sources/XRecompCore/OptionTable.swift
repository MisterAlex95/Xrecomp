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
    /// The consoles whose games read it (XBA-278; the Android launcher's GameSettings.appliesTo). A game's Settings
    /// shows these rows only; Default settings shows every row.
    public let consoles: Set<Console>

    public init(_ id: String, _ section: Section, _ title: String, _ detail: String, keys: [String],
                onlyWhen: (key: String, value: String)? = nil, consoles: Set<Console> = [.xbox],
                _ choices: [Choice]) {
        self.id = id
        self.section = section
        self.title = title
        self.detail = detail
        self.keys = keys
        self.choices = choices
        self.onlyWhen = onlyWhen
        self.consoles = consoles
    }

    public func applies(to console: Console) -> Bool { consoles.contains(console) }

    public var defaultIndex: Int { choices.firstIndex(where: \.isDefault) ?? 0 }
}

/// The options the Mac launcher offers: the Android table (RecompConfig.java) for what the desktop runtimes read,
/// without the Android-only keys (RECOMP_BIG_CORES, the shell self-test), plus the window options of the SDL
/// presenter. Since XBA-278 the launcher plays GBA, PS1, N64 and GameCube games too: their rows (rewind, the console
/// picture, the internal resolution, the N64's pak) and the shared ones say so in `consoles`. Frame generation needs
/// the shader cache from the player's Lossless.dll (Settings › Graphics › Choose Lossless.dll, LsfgCache) and a
/// Vulkan loader (MoltenVK).
public enum OptionTable {
    typealias C = ConfigOption.Choice
    /// Every console's games.
    static let every: Set<Console> = Set(Console.allCases)
    /// The console runtimes (the shared window, src/video/host_window.c).
    static let consoleGames: Set<Console> = [.gba, .ps1, .n64, .gc]

    public static let all: [ConfigOption] = [
        ConfigOption("language", .general, "Language",
                     "The console language the game uses when its disc has it. System: the Mac's language.",
                     keys: ["RECOMP_LANGUAGE"],
                     [C("System", nil), C("English", "en"), C("Français", "fr"), C("Deutsch", "de"),
                      C("Español", "es"), C("Italiano", "it"), C("Português", "pt"), C("日本語", "ja"),
                      C("한국어", "ko"), C("中文", "zh")]),
        ConfigOption("rumble", .general, "Controller rumble", "The pad's motors when the game uses them.",
                     keys: ["RECOMP_RUMBLE"], consoles: every, [C("On", nil), C("Off", "0")]),
        ConfigOption("rewind", .general, "Rewind",
                     "Hold the rewind key (R, or the pad's hotkey) to go back in time. Auto: on, but for an Xbox "
                     + "title whose save states do not work.",
                     keys: ["RECOMP_REWIND"], consoles: every, [C("Auto", nil), C("On", "1"), C("Off", "0")]),
        ConfigOption("rewind-mb", .general, "Rewind memory",
                     "The history's memory: more goes further back. A large Xbox or GameCube game needs 128 or 256.",
                     keys: ["RECOMP_REWIND_MB"], consoles: every,
                     [C("32 MB", "32"), C("64 MB", nil), C("128 MB", "128"), C("256 MB", "256")]),
        ConfigOption("n64-pak", .general, "Controller pak",
                     "Nintendo 64: the pak in the pad. Auto: the game's own choice.",
                     keys: ["RECOMP_N64_PAK"], consoles: [.n64],
                     [C("Auto", nil), C("Rumble Pak", "rumble"), C("Controller Pak", "controller")]),
        ConfigOption("fullscreen", .general, "Window", "Start in a window or fullscreen. F11 switches while playing.",
                     keys: ["RECOMP_FULLSCREEN"], consoles: every, [C("Window", nil), C("Fullscreen", "1")]),
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
        ConfigOption("console-scale", .graphics, "Picture size",
                     "GBA, PlayStation, N64 and GameCube games: the picture in the window. Integer: whole multiples, "
                     + "sharpest.",
                     keys: ["RECOMP_SCALE"], consoles: consoleGames,
                     [C("Fit, keep the shape", nil), C("Integer", "integer"), C("Stretch", "stretch")]),
        ConfigOption("console-filter", .graphics, "Picture filter",
                     "GBA, PlayStation, N64 and GameCube games. The console's own: sharp for the GBA, smooth for "
                     + "the others.",
                     keys: ["RECOMP_FILTER"], consoles: consoleGames,
                     [C("The console's own", nil), C("Sharp", "nearest"), C("Smooth", "linear")]),
        ConfigOption("upscale", .graphics, "Console resolution",
                     "PlayStation, N64 (Vulkan renderer) and GameCube games drawn at a multiple of the console's "
                     + "resolution. 8x: the N64 and the GameCube only.",
                     keys: ["RECOMP_UPSCALE"], consoles: [.ps1, .n64, .gc],
                     [C("Native", nil), C("2x", "2"), C("4x", "4"), C("8x", "8")]),
        ConfigOption("aspect", .graphics, "Aspect ratio",
                     "Auto: 16:9 where the game draws wide frames, 4:3 elsewhere.",
                     keys: ["RECOMP_ASPECT"], [C("Auto", nil), C("4:3", "4:3"), C("16:9", "16:9")]),
        ConfigOption("widescreen", .graphics, "Widescreen",
                     "The console's widescreen setting: games that support it draw 16:9 frames (show them with Aspect "
                     + "ratio 16:9 or Auto).",
                     keys: ["RECOMP_WIDESCREEN"], [C("Off", nil), C("On", "1")]),
        ConfigOption("480p", .graphics, "480p", "The console's 480p setting: games that offer progressive output use it.",
                     keys: ["RECOMP_480P"], [C("Off", nil), C("On", "1")]),
        ConfigOption("vsync", .graphics, "VSync", "Waits for the display's refresh.",
                     keys: ["RECOMP_PRESENT_VSYNC"], consoles: every, [C("On", nil), C("Off", "0")]),
        ConfigOption("filtering", .graphics, "Texture filtering", "CPU rasteriser only. Smoother is slower.",
                     keys: ["RECOMP_TEX_BILINEAR", "RECOMP_TEX_ANISO"],
                     [C("Sharp", nil, nil), C("Smooth", "1", nil), C("Anisotropic 8x", nil, "8"),
                      C("Anisotropic 16x", nil, "16")]),
        ConfigOption("shader", .graphics, "Picture shader", "A filter over the finished picture.",
                     keys: ["RECOMP_SHADER"], consoles: every,
                     [C("None", nil), C("Sharp", "sharp"), C("Scanlines", "scanlines"), C("CRT", "crt"),
                      C("FXAA", "fxaa"), C("CAS (sharpen)", "cas")]),
        ConfigOption("shader-strength", .graphics, "Shader strength",
                     "How strong the scanlines, CRT or CAS shader is. Default: the shader's own.",
                     keys: ["RECOMP_SHADER_STRENGTH"], consoles: every,
                     [C("Default", nil), C("25 %", "25"), C("50 %", "50"), C("75 %", "75"), C("100 %", "100")]),
        ConfigOption("shader-curvature", .graphics, "CRT curvature", "The CRT shader's screen curve.",
                     keys: ["RECOMP_SHADER_CURVATURE"], onlyWhen: ("RECOMP_SHADER", "crt"), consoles: every,
                     [C("Off", nil), C("Slight", "30"), C("Strong", "70")]),
        ConfigOption("framegen", .graphics, "Frame generation",
                     "Generated frames between the game's own (LSFG). Needs your Lossless.dll (below) and MoltenVK. "
                     + "Performance: about 4x cheaper.",
                     keys: ["RECOMP_LSFG", "RECOMP_LSFG_MULT", "RECOMP_LSFG_PERF"], consoles: every,
                     [C("Off", nil, nil, nil), C("2x", "1", "2", nil), C("2x performance", "1", "2", "1")]),
        ConfigOption("hrtf", .audio, "3D audio (HRTF)", "Positions 3D sounds for headphones.",
                     keys: ["RECOMP_APU_HRTF"], [C("On", nil), C("Off", "0")]),
        ConfigOption("audio-buffer", .audio, "Audio buffer", "Longer buffers crackle less but lag more.",
                     keys: ["RECOMP_AUDIO_BUF_SAMPLES"], consoles: every,
                     [C("Short", "256"), C("Normal", nil), C("Long", "1024"), C("Longest", "2048")]),
        ConfigOption("raster-threads", .advanced, "Raster threads",
                     "Threads drawing for the CPU rasteriser. Auto: the cores minus 4.",
                     keys: ["RECOMP_RASTER_THREADS"],
                     [C("Auto", nil), C("1", "1"), C("2", "2"), C("4", "4"), C("6", "6"), C("8", "8")]),
    ]

    public static func options(in section: ConfigOption.Section) -> [ConfigOption] {
        all.filter { $0.section == section }
    }

    /// A game's rows: those its console reads (nil: every row, Default settings).
    public static func options(in section: ConfigOption.Section, for console: Console?) -> [ConfigOption] {
        options(in: section).filter { o in console.map { o.applies(to: $0) } ?? true }
    }

    public static func option(_ id: String) -> ConfigOption? { all.first { $0.id == id } }
}
