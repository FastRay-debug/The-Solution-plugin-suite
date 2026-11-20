# The Solution Suite — Clean Source Handoff

- **Stack**: C++20, JUCE 8.0.7, `clap-juce-extensions`, `RTNeural`, WebView2 UI (`UI/index.html`)
- **13 Consolidated Modules**: `TheBunker`, `TheStage`, `TheLand`, `TheLoft`, `TheOrbit`, `TheBridge`, `TheVoid`, `TheAnchor`, `TheAether`, `TheMosaic`, `Innerspace`, `TheMatrix`, `TheScope`
- **Formats**: VST3, CLAP, AU (macOS), Standalone, plus automatic AAX detection (`C:/SDKs/AvidAAXSDK` or `aax_sdk/`)
- **Skins**: All 13 `.jpg` faceplates are in `UI/skins/` (also used by `SkinEditor/index.html` via `../UI/skins/`).
  At runtime every plugin loads them from one shared folder, so the images ship once instead of 13+ times:
  - Windows: `%APPDATA%\Fast Ray Media\The Solution\Skins`, then `%ProgramData%\Fast Ray Media\The Solution\Skins`
  - macOS: `~/Library/Application Support/Fast Ray Media/The Solution/Skins`, then `/Library/Application Support/...`
  - Building with `-DBUNDLE_SKINS=ON` also embeds them in each plugin as a fallback. With neither, the skin picker is hidden.

## Manual CMake Build
```bash
cmake -B build
cmake --build build --config Release
```
