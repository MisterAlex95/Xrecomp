// mkicon.swift SVG OUT.iconset: renders the app icon (docs/images/xrecomp-icon.svg, original art) at every size
// iconutil wants. NSImage reads SVG on macOS 14 and later. Run by build-app.sh:
//   swift scripts/mkicon.swift ../docs/images/xrecomp-icon.svg build/AppIcon.iconset && iconutil -c icns ...
import AppKit

let args = CommandLine.arguments
guard args.count == 3, let svg = NSImage(contentsOfFile: args[1]) else {
    FileHandle.standardError.write("usage: mkicon.swift icon.svg out.iconset (and the SVG must load)\n".data(using: .utf8)!)
    exit(1)
}
let out = URL(fileURLWithPath: args[2])
try? FileManager.default.createDirectory(at: out, withIntermediateDirectories: true)
for base in [16, 32, 128, 256, 512] {
    for scale in [1, 2] {
        let px = base * scale
        guard let rep = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: px, pixelsHigh: px, bitsPerSample: 8,
                                         samplesPerPixel: 4, hasAlpha: true, isPlanar: false, colorSpaceName: .deviceRGB,
                                         bytesPerRow: 0, bitsPerPixel: 0) else { exit(1) }
        rep.size = NSSize(width: px, height: px)
        NSGraphicsContext.saveGraphicsState()
        NSGraphicsContext.current = NSGraphicsContext(bitmapImageRep: rep)
        NSGraphicsContext.current?.imageInterpolation = .high
        // The macOS icon grid: the artwork inset to about 80 % of the canvas.
        let inset = Double(px) * 0.1
        svg.draw(in: NSRect(x: inset, y: inset, width: Double(px) - 2 * inset, height: Double(px) - 2 * inset))
        NSGraphicsContext.restoreGraphicsState()
        let name = scale == 1 ? "icon_\(base)x\(base).png" : "icon_\(base)x\(base)@2x.png"
        try! rep.representation(using: .png, properties: [:])!.write(to: out.appendingPathComponent(name))
    }
}
