# The Solution — Complete Modular Audio Suite

## Download Ready-Made Builds (no compiling)

Every push to GitHub builds all 13 modules automatically:

1. Open the repo's **Actions** tab and click the latest green **Build plugins** run.
2. Scroll to **Artifacts** and download **TheSolution-Windows** or **TheSolution-macOS**.
3. Unzip it and follow `HOW TO INSTALL.txt` inside.

Tagged versions (e.g. `v0.2.0`) are also published on the **Releases** page.

## One-Click Build & Install (Windows 11)

1. Extract this folder anywhere on your computer.
2. Double-click **`Install.bat`**.

`Install.bat` handles everything from a single interactive menu:
- **Pre-flight dependency check & auto-install** (CMake, Git, Visual Studio C++ Build Tools, Microsoft WebView2 SDK)
- **Build & Install** all 11 modules (`[0]`), specific module numbers (`1,2,4`), or missing-only (`[M]`)
- **Skins toggle (`[T]`)** — `OFF` (default) installs the 13 skin images once to `%ProgramData%\FastRayMedia\The Solution\Skins`, shared by every plugin; `ON` embeds a copy in each plugin
- **Gather & Install** already-built VST3/CLAP/Standalone plugins (`[I]`)
- **Clean** the build folder (`[C]`, preserving `deps_cache/` and `webview2/`) or collected plugins (`[X]`)

## macOS Build & Install

Run `build_mac.command` to compile all 11 modules (**The Bunker**, **The Stage**, **The Land**, **The Loft**, **The Orbit**, **The Bridge**, **The Void**, **The Anchor**, **The Aether**, **The Mosaic**, and **Innerspace**) as **VST3**, **AU**, **CLAP**, and **Standalone** targets with the universal `-0.5 dBFS` ceiling guard and `600 ms` delayed tooltips, then run `install_all_plugins.command`.
