# The Solution — Complete Modular Audio Suite

## What's in every module

| Feature | Where | What it does |
|---|---|---|
| Presets | 🗄 VAULT | 10 factory presets + your own, shared by all modules, also in your DAW's preset menu |
| True-peak ceiling + LUFS | top bar / meter popup | -0.5 dBTP look-ahead limiter; BS.1770 momentary / short-term / integrated loudness |
| Rack Link | click **LK** | Session view of every module, duck groups (no sidechain routing), suite-wide Glue set on The Scope |
| Room correction | Room panel → ● SWEEP / VIEW | Measures your room with **any mic**, fits correction filters; mic list + calibration files |
| Binaural | 🎧 BINAURAL | Hear the mix on virtual speakers around your head (headphones) |
| Mix Assistant | 🎯 ASSIST | Compare with a reference track or genre target; one-click air / width / glue |
| Stem Focus | Stem Focus menu | Process only drums, tonal parts or the centre vocal; SOLO to hear it |
| Neural Character | 🧠 CHARACTER | Run captured models of real hardware (see `docs/NEURAL_MODELS.md`) |
| Phone remote | 📱 REMOTE | Control the module from a phone/tablet on the same Wi-Fi with a pairing code |
| A/B · Δ | top bar | Loudness-matched original / hear only what the module changes |
| Pro Tricks | ★ PRO | Auto gain staging, mono-the-lows + Phase Guard, Smooth (harshness), Phone Bass, streaming preview |
| Listening Rooms | 🏛 ROOMS | 24 rooms (studios, venues, devices, cars, mono) + Translation Tour; monitoring only |
| Unmask | click **LK** | Priority track (vocal/kick) makes the rest of its group dip only where they collide |
| Auto-Align | click **LK** | Fixes timing and polarity between mics on the same source with one click |
| 115 Hz notch | Room panel | Off / On / Auto (engages only when 115 Hz builds up) |
| 3D Stage | centre view | Every Solution track as a glowing orb: drag to move, Shift+drag height, scroll width, real distance, clash glow → one-click Unmask, motion paths synced to tempo, 360° sphere in headphone mode, real vectorscope and spectrum |
| Safe Bounce | ★ PRO | Monitoring tools switch themselves off for offline bounces; red warning bar with one-click turn-off (every module) |

## Download & Install (Windows)

Every push to GitHub builds a Windows installer automatically:

1. Open the repo's **Actions** tab and click the latest green **Build plugins** run.
2. Under **Artifacts**, download **TheSolution-Windows-Installer** and unzip it.
3. Double-click **`The_Solution_Suite_Setup.exe`**, tick what you want (formats, modules, skins), click **Install**.

Uninstall any time from Windows **Settings → Apps → The Solution Suite**.
The macOS installer (`.pkg`) is built on demand: Actions → Build plugins → **Run workflow**.

## One-Click Build & Install (Windows 11)

1. Extract this folder anywhere on your computer.
2. Double-click **`Install.bat`**.

`Install.bat` handles everything from a single interactive menu:
- **Pre-flight dependency check & auto-install** (CMake, Git, Visual Studio C++ Build Tools, Microsoft WebView2 SDK)
- **Build & Install** all 11 modules (`[0]`), specific module numbers (`1,2,4`), or missing-only (`[M]`)
- **Skins toggle (`[T]`)** — `OFF` (default) installs the 13 skin images once to `%ProgramData%\Billet Audio\The Solution\Skins`, shared by every plugin; `ON` embeds a copy in each plugin
- **Gather & Install** already-built VST3/CLAP/Standalone plugins (`[I]`)
- **Clean** the build folder (`[C]`, preserving `deps_cache/` and `webview2/`) or collected plugins (`[X]`)

## macOS Build & Install

Run `build_mac.command` to compile all 11 modules (**The Bunker**, **The Stage**, **The Land**, **The Loft**, **The Orbit**, **The Bridge**, **The Void**, **The Anchor**, **The Aether**, **The Mosaic**, and **Innerspace**) as **VST3**, **AU**, **CLAP**, and **Standalone** targets with the universal `-0.5 dBFS` ceiling guard and `600 ms` delayed tooltips, then run `install_all_plugins.command`.
