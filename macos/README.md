# XRecomp for macOS

The Mac launcher: a SwiftUI app that lists your games, prepares them with the toolkit next to it and plays them.
Command Line Tools are enough (no Xcode project).

```
bash macos/scripts/check.sh        # build, checks, smoke run
bash macos/scripts/build-app.sh    # macos/build/XRecomp.app (ad-hoc signed)
swift run --package-path macos XRecomp --demo   # every screen from mock data
```

Everything else, including the Mac checklist: [docs/macos/launcher.md](../docs/macos/launcher.md).
