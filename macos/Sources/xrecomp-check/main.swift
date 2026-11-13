import Foundation

// swift run xrecomp-check [filter]: every check whose name contains filter (all by default).
registerSetupChecks()
registerPrepareChecks()
registerLibraryChecks()
registerGameChecks()
registerTitleAppChecks()
let code = H.run(filter: CommandLine.arguments.dropFirst().first)
for d in tempDirs { try? FileManager.default.removeItem(at: d) }
exit(code)
