# The Solution Suite — Clean Source Handoff

- **Stack**: C++20, JUCE 8.0.7, `clap-juce-extensions`, `RTNeural`, WebView2 UI (`UI/index.html`)
- **13 Consolidated Modules**: `TheBunker`, `TheStage`, `TheLand`, `TheLoft`, `TheOrbit`, `TheBridge`, `TheVoid`, `TheAnchor`, `TheAether`, `TheMosaic`, `Innerspace`, `TheMatrix`, `TheScope`
- **Formats**: VST3, CLAP, AU (macOS), Standalone, plus automatic AAX detection (`C:/SDKs/AvidAAXSDK` or `aax_sdk/`)
- **Skins**: All 13 hyperrealistic `.jpg` faceplates are in `UI/skins/` (shared by both `UI/index.html` and `SkinEditor/index.html` via `../UI/skins/`).

## Manual CMake Build
```bash
cmake -B build -DBUNDLE_SKINS=ON
cmake --build build --config Release
```
