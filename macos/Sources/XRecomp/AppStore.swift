import AppKit
import XRecompCore

@MainActor
final class AppStore: LauncherActions {
    let model = LauncherModel()
    init() { model.actions = self }
    func start() {}
    func rescan() {}
    func addGame(_ url: URL) {}
    func prepare(_ id: String, force: Bool) {}
    func cancelPrepare(_ id: String) {}
    func play(_ id: String) {}
    func stop(_ id: String) {}
    func loadConfig(_ id: String?) -> RecompConfig { RecompConfig() }
    func saveConfig(_ config: RecompConfig, for id: String?) {}
    func storage(_ id: String) -> [(Storage.Item, Int64)] { [] }
    func delete(_ item: Storage.Item, of id: String) {}
    func importSaves(_ id: String) -> Int { 0 }
    func reveal(_ url: URL) {}
    func titlePaths(_ id: String) -> TitlePaths? { nil }
    func setToolkit(_ path: String) {}
    func addLibraryFolder(_ path: String) {}
    func removeLibraryFolder(_ path: String) {}
    func setWorkRoot(_ path: String?) {}
    func recheckSetup() {}
    func createVenv() {}
    func licences() -> [(String, String)] { [] }
}
