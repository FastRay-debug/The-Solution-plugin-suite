# The Solution — requirements checklist (locked in)

Every requirement from the original Gemini design conversation and from building with Claude.
Nothing is removed from this list; items only move from ☐ to ✅. Build order agreed with Fast Ray:
**quick fixes → 3D hardware themes & holograms → features (skin studio, VR, learning, LAN) → module engines.**

## Done ✅
- −0.5 dBFS ceiling everywhere (true-peak limiter); double-click the badge to bypass
- Surgical precision mode (Shift+Space oscilloscope) in every module
- ` maximise panel · Ctrl+` fullscreen on another display (Esc returns) · dual-display use
- Double-click the 3D view → immersive fly-through world; right-click → skins / looks / Skin Editor / presets / rooms menu
- 3D creature world: walk/fly (WASD, gamepad), grab tracks with the 3D hand
- Billet Audio badge (top-left, every plugin) = command hub: presets, diagnostics & error log, panic reset
- Diagnostic Utility (real data): cores, GPU, temps where exposed, VU, heat meter, 60 s history, teach mode, CSV/XML export, robot hand with FAST RAY wrist display that moves aside, assignable title-bar lights, standalone Solution Utility app
- Macro strip 1–5 + 20-slot bank; macros bindable to keys
- Keyboard shortcuts for everything + Shortcut Manager (Ctrl+Shift+K) with print/PDF; Gemini's keys (Ctrl+Alt+V display/VR cycle, H/Shift+H hand pointers, Ctrl+Shift safe/vanilla load)
- Voice control of everything incl. DAW transport (offline, wake word "Solution"); LUNA keys
- Looks button (OS): cycle favourite looks, click-and-hold for slots
- Tooltips 600 ms with toggle; Beginner mode; Modifier-linked focus (Shift-drag)
- MIDI learn on every control (right-click / K)
- Lookahead limiter, DC Guard, Pre-Render check, real dialogue ducking, transient shaper, Infrasonic Lift (Bunker, 7.63 Hz, WIDE toggle)
- Rack Link: unmask, auto-align, ducking, glue, panic, session-wide monitoring-off
- Room correction with any mic (+ mic profiles), 24 listening rooms, binaural, stem focus, neural character (browse for models)
- DQS quality scaling; level-matched A/B; auto gain stage; Ultra Wide secret wet/dry screw
- One-click installer with checkboxes; vendor Billet Audio; AAX auto-detect

## To do ☐ (in build order)
### 3D hardware themes & holograms
- ✅ Universal 3D hardware themes (7: Hypercar, Art Deco, Glass Dome, Abyssal, Vintage Brass, Holo Glass, Fast Ray Sunset), session-synced with "only here" override, alongside photo skins; per-module live hologram (Bunker sub-wave cube, Orbit glass dome, Aether spectral waterfall, others ring) on one shared GPU engine
- ✅ Faceplate depth: bevels, screws, LED rims, mouse-follow 3D tilt, hologram projector base (more 3D interactivity still welcome)
- ✅ Colours: burnt/sunset orange, LED-glow blue, asymmetric blue pop (Fast Ray Sunset)
- ☐ 50 skins incl. a Vidalia skin (Vidalia High School, Kroger, local landmarks); skins + Skin Editor in a separate installer with per-skin checkboxes
- ✅ Analog VU meters full screen (Ctrl+Alt+U, meter menu); Ctrl+click panels (or Alt+`) then ` tiles them full screen, Ctrl+` tiles them on the other display, Esc restores
- ✅ Pop-out meter vertical/horizontal (⇅ button or V; remembered)

### Features
- ✅ Mobile Skin Studio: phone camera/library → frame, brightness/contrast/warmth, auto LED colours → sent to the plugin (live), saved for every Solution plugin
- ☐ Skin Editor as its own program (separate installer), launchable from every plugin (launcher already wired)
- ✅ Head tracking for binaural: OSC trackers + phone apps (GyrOSC, ZIG SIM, Sensors2OSC; IEM/SPARTA ypr/quaternion), phone remote page sensors, gamepad/joystick; recentre Ctrl+Alt+H; 3D head follows
- ✅ Drone telemetry: flight-log CSV (lat/lon/alt) becomes a sound's motion path
- ☐ Webcam face tracking; native VR headset (OpenXR) output
- ✅ Adaptive Learning (suggest only, local file): your usual values per module, "you usually adjust B after A", favourite controls → macro keys; Apply / Not now / Never; on/off, what it learned, forget everything
- ☐ AI Analyze extensions
- ✅ LAN / local DSP server (AudioGridder-style): Solution Utility runs a DSP Node (auto-discovered on the LAN / this PC); plugins offload with a chosen latency (reported to the DAW), state kept in sync, automatic fallback to local; output matches local processing exactly
- ✅ Voice: choose the microphone source (Voice settings → Microphone; saved, falls back to the Windows default)
- ✅ MIDI Controllers panel (Ctrl+Alt+M): presets (Korg nanoKONTROL2, Akai MPK Mini, Novation Launch Control XL, Behringer X-Touch Mini, Arturia MiniLab 3 / KeyLab Essential, generic), auto-setup wizard, mapping list with delete, MIDI map saved with the session; joystick/gamepad axes → any control
- ✅ History panel (Ctrl+Alt+Z, 🕘, voice "history"): every edit logged once it settles (presets = one entry), Undo This (selective), Go Back Here… keeping ticked controls, Whole Rack tab undoes edits in other instances, automation playback ignored, saved with the session
- ✅ Test-tone analyser (Ctrl+Alt+A, 📈, voice "analyzer"): measures a private copy of the module - log-sweep magnitude/phase (latency removed), harmonics H2-H10 + THD/THD+N with before/after spectra stacked, level transfer curve; precision grid locks to the 1 kHz cal tone (1 div = 1 ms, rails at -18 dBFS). Measuring third-party plugins comes with The Bridge hosting
- ✅ Global search / command palette (Ctrl+P or /, 🔍): actions, every control, presets, rooms, themes, skins; fuzzy; "width 120" sets a control; > actions, @ controls, # presets

### Module engines (real DSP per module)
- ☐ The Bunker: sub-harmonic synthesizer, dynamic low-end filtering, 3-layer Split-Core M/S
- ☐ The Anchor: 4-band soft clipper, Gold Clip-style curves, shapes, drive >100%, squash/crest detector (wanted vs unwanted distortion), transparent limiter option, dynamic multiband, up to 16x oversampling, PAM (Phase Alignment Matrix) button
- ☐ The Aether: generative soundscape (particle cloud + XY pad, granular), delay engine with Clarity-inspired features (focus/soften, tempo-synced ducking, tape THD), sidechain
- ☐ The Mosaic: STFT spectral morphing (1024–4096), 4-node XY pad, transient preservation
- ☐ The Void: procedural acoustic modeller (non-Euclidean spaces, phase scrambling)
- ☐ The Land: ground reflections; HOA ambisonics (needs multichannel)
- ☐ The Orbit: tube modelling, real regen feedback, Auto-Connect wizard
- ☐ The Bridge: DAW sync, sidechain/CV routing, hosting third-party VST3/AU/CLAP
- ☐ The Matrix: full modulation matrix (poly LFOs, envelopes, audio followers)
- ☐ Surround / Atmos output (separate project)

### Business / separate programs (later)
- ☐ Auto-updater; opt-in feedback & error reporting
- ☐ Copy protection with automatic online activation
- ☐ Interactive web manual (comments + drawing on pages), kept updated with every feature

### Not possible as specified
- Fan control (needs a kernel driver — declined); running on UAD DSP hardware (closed platform)
