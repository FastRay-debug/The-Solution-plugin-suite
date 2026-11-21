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
- ☐ Analog meters full screen; Ctrl-click several panels and tile them full screen together
- ☐ Pop-out meter vertical/horizontal option

### Features
- ✅ Mobile Skin Studio: phone camera/library → frame, brightness/contrast/warmth, auto LED colours → sent to the plugin (live), saved for every Solution plugin
- ☐ Skin Editor as its own program (separate installer), launchable from every plugin (launcher already wired)
- ✅ Head tracking for binaural: OSC trackers + phone apps (GyrOSC, ZIG SIM, Sensors2OSC; IEM/SPARTA ypr/quaternion), phone remote page sensors, gamepad/joystick; recentre Ctrl+Alt+H; 3D head follows
- ✅ Drone telemetry: flight-log CSV (lat/lon/alt) becomes a sound's motion path
- ☐ Webcam face tracking; native VR headset (OpenXR) output
- ☐ Adaptive Learning (suggest only, local); AI Analyze extensions
- ☐ LAN / local DSP server (AudioGridder-style)
- ☐ Voice: choose the microphone source
- ☐ MIDI controller presets for major manufacturers + auto-setup wizard; joystick mapping for any control
- ☐ Undo history per module + global, with selective undo (go back but keep chosen changes)
- ☐ Test-tone analyser (PluginDoctor-style): tone before/after a plugin, before/after stacked; precision grid follows the calibration tone
- ☐ Global search (presets/sounds)

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
