import Foundation

/// What prepare and the games need on this Mac, for the Setup screen. Each missing item comes with the command to
/// run; the launcher never runs `brew install` itself.
public struct Dependency: Identifiable, Equatable, Sendable, Codable {
    public let id: String
    public let name: String
    public let ok: Bool
    public let optional: Bool
    /// The version or the reason it failed.
    public let detail: String
    /// The command that fixes it, shown when not ok.
    public let fix: String

    public init(id: String, name: String, ok: Bool, optional: Bool = false, detail: String, fix: String) {
        self.id = id
        self.name = name
        self.ok = ok
        self.optional = optional
        self.detail = detail
        self.fix = fix
    }
}

public enum DependencyCheck {
    /// Runs one command (with the fixed PATH); injectable for the checks.
    public typealias Runner = (_ exe: String, _ args: [String]) -> Shell.Result

    public static let defaultRunner: Runner = { exe, args in
        Shell.run("/usr/bin/env", [exe] + args, environment: ChildEnvironment.base(), timeout: 20)
    }

    static func firstLine(_ s: String) -> String {
        s.split(separator: "\n").first.map { String($0).trimmingCharacters(in: .whitespaces) } ?? ""
    }

    /// Every item. `python` is the resolved interpreter (nil = none imports the packages yet); `vulkan` the Vulkan
    /// loader frame generation needs (optional).
    public static func run(python: URL?, vulkan: URL? = VulkanLoader.find(),
                           runner: Runner = defaultRunner) -> [Dependency] {
        var out: [Dependency] = []
        func tool(_ id: String, _ name: String, _ args: [String], fix: String, optional: Bool = false) {
            let r = runner(id, args)
            out.append(Dependency(id: id, name: name, ok: r.ok, optional: optional,
                                  detail: r.ok ? firstLine(r.output) : "not found", fix: fix))
        }
        tool("cmake", "CMake", ["--version"], fix: "brew install cmake")
        tool("ninja", "Ninja (faster builds)", ["--version"], fix: "brew install ninja", optional: true)
        tool("clang", "Clang (Command Line Tools)", ["--version"], fix: "xcode-select --install")
        tool("git", "Git", ["--version"], fix: "xcode-select --install")
        for (id, name, pkg, fix) in [("sdl2", "SDL2 (sdl2-compat is fine)", "sdl2", "brew install sdl2-compat"),
                                     ("epoxy", "libepoxy", "epoxy", "brew install libepoxy")] {
            let r = runner("pkg-config", ["--modversion", pkg])
            out.append(Dependency(id: id, name: name, ok: r.ok,
                                  detail: r.ok ? firstLine(r.output) : "pkg-config cannot find \(pkg)", fix: fix))
        }
        if let python {
            let r = runner(python.path, ["-c", "import " + PythonEnv.packages.joined(separator: ", ")
                                             + "; import sys; print(sys.version.split()[0])"])
            out.append(Dependency(id: "python", name: "Python with capstone, pefile, numpy", ok: r.ok,
                                  detail: r.ok ? "\(firstLine(r.output)) · \(python.path)" : firstLine(r.output),
                                  fix: "\(python.path) -m pip install capstone pefile numpy"))
        } else {
            out.append(Dependency(id: "python", name: "Python with capstone, pefile, numpy", ok: false,
                                  detail: "no Python with the packages yet",
                                  fix: "Setup › Create the Python environment (needs brew install python@3.12)"))
        }
        out.append(Dependency(id: "vulkan", name: "Vulkan (MoltenVK) for frame generation", ok: vulkan != nil,
                              optional: true, detail: vulkan?.path ?? "no libvulkan or libMoltenVK in "
                                  + VulkanLoader.dirs.joined(separator: ", "),
                              fix: "brew install molten-vk vulkan-loader"))
        return out
    }

    /// True when every required item is there.
    public static func allRequiredOK(_ deps: [Dependency]) -> Bool {
        deps.allSatisfy { $0.ok || $0.optional }
    }
}
