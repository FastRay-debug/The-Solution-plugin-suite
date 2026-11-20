# The Solution — Complete Modular Audio Suite

## One-Click Build & Install (Windows 11)

1. Extract this folder anywhere on your computer.
2. Double-click **`Install.bat`**.

`Install.bat` handles everything from a single interactive menu:
- **Pre-flight dependency check & auto-install** (CMake, Git, Visual Studio C++ Build Tools, Microsoft WebView2 SDK)
- **Build & Install** all 11 modules (`[0]`), specific module numbers (`1,2,4`), or missing-only (`[M]`)
- **Cars & Places Skins toggle (`[T]`)** — build with all 50 `/cars` & `/places` skins (`ON`) or a lightweight no-skins build (`OFF`)
- **Gather & Install** already-built VST3/CLAP/Standalone plugins (`[I]`)
- **Clean** the build folder (`[C]`, preserving `deps_cache/` and `webview2/`) or collected plugins (`[X]`)

## macOS Build & Install

Run `build_mac.command` to compile all 11 modules (**The Bunker**, **The Stage**, **The Land**, **The Loft**, **The Orbit**, **The Bridge**, **The Void**, **The Anchor**, **The Aether**, **The Mosaic**, and **Innerspace**) as **VST3**, **AU**, **CLAP**, and **Standalone** targets with the universal `-0.5 dBFS` ceiling guard and `600 ms` delayed tooltips, then run `install_all_plugins.command`.
