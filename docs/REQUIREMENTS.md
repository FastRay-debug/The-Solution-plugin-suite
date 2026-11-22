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
- ✅ AI Analyze hub (A, then 1-6): Mix Match (reference/genre) · Spatial Optimization (spreads tracks sharing frequencies, centres bass & dialogue, narrows wide lows; applies across the rack, undoable) · Intelligent Dialogue Ducking (notch follows where the dialogue tracks carry their words, 0.9-4.5 kHz) · Cinematic Cue Tracking (snapshots at timeline positions with SMPTE incl. 29.97 drop-frame, smooth crossfades; Ctrl+Alt+K / voice "cue") · Emotional Resonance Mapping (key mode, brightness, loudness, hit rate -> valence/arousal; optional gentle drive/space/width follow) · Smart Metadata Embedding (records each track's 3D path; writes ADM axml + chna + iXML into a copy of a bounced WAV, or JSON + ADM XML sidecars)
- ✅ Voice transport keys auto-detected for the DAW in use (host name + program file): Ableton, FL, Reaper, Cubase/Nuendo, Studio One, Pro Tools, LUNA, Bitwig, Cakewalk, Reason, Waveform, Mixbus/Ardour, Samplitude, Audition, Acid, Vegas, Mixcraft, Renoise, DP (Logic/GarageBand with the Mac version); edits remembered per DAW
- ✅ Holograms show each module's real response curve live (measured on a private copy in ~20 ms whenever a setting changes; gliding gold ribbon + fill, right channel when it differs, live spectrum behind), in every module and theme
- ✅ Auto Safe Mode (secret menu, Ctrl+Alt+G, voice "safe mode", Beginner Mode turns it on; off by default): eases extreme drive/character/transient settings, pads hot input into the saturators and makes it back up, backs off further if the ceiling keeps working - full-drive Anchor 21% → 1% THD at the same loudness; footer badge shows how much it holds back
- ✅ Three-way look: Custom GUI → Classic OS → Vanilla (plain searchable list of every control); O key / OS button cycle it (hold OS for saved look slots), remembered per instance. Also fixed: all saved UI settings (beginner, key map, looks, MIDI/joystick maps, meter orientation…) now come back when a session reopens
- ☐ Interactive Foley synthesizer from picture (Gemini idea - needs a video input; pairs with The Aether)
- ✅ LAN / local DSP server (AudioGridder-style): Solution Utility runs a DSP Node (auto-discovered on the LAN / this PC); plugins offload with a chosen latency (reported to the DAW), state kept in sync, automatic fallback to local; output matches local processing exactly
- ✅ Voice: choose the microphone source (Voice settings → Microphone; saved, falls back to the Windows default)
- ✅ MIDI Controllers panel (Ctrl+Alt+M): presets (Korg nanoKONTROL2, Akai MPK Mini, Novation Launch Control XL, Behringer X-Touch Mini, Arturia MiniLab 3 / KeyLab Essential, generic), auto-setup wizard, mapping list with delete, MIDI map saved with the session; joystick/gamepad axes → any control
- ✅ History panel (Ctrl+Alt+Z, 🕘, voice "history"): every edit logged once it settles (presets = one entry), Undo This (selective), Go Back Here… keeping ticked controls, Whole Rack tab undoes edits in other instances, automation playback ignored, saved with the session
- ✅ Test-tone analyser (Ctrl+Alt+A, 📈, voice "analyzer"): measures a private copy of the module - log-sweep magnitude/phase (latency removed), harmonics H2-H10 + THD/THD+N with before/after spectra stacked, level transfer curve; precision grid locks to the 1 kHz cal tone (1 div = 1 ms, rails at -18 dBFS). Measuring third-party plugins comes with The Bridge hosting
- ✅ Global search / command palette (Ctrl+P or /, 🔍): actions, every control, presets, rooms, themes, skins; fuzzy; "width 120" sets a control; > actions, @ controls, # presets

### Module engines (real DSP per module)
- ✅ The Bunker: sub-harmonic synthesizer (octave-down divider following 50-160 Hz), dynamic low tamer (120 Hz shelf dips only when lows boom, up to -12 dB), 3-layer Split-Core M/S (LR4 bands summing flat, per-band width, movable crossovers); all neutral by default so the original tuning is untouched
- ✅ The Anchor clipper suite: single or 4-band (120 Hz / 1 kHz / 6 kHz LR4), curves Soft / Gold smooth-knee / Tube / Hard / Arctan / Sine Fold + knee, drive to 1585% (+24 dB), ceiling, output, blend; transparent look-ahead limiter mode (1.3% vs 24% distortion); dynamic multiband (eases hot bands, same level: 20% → 4% distortion); 1x-16x oversampling with fixed latency; PAM keeps parallel blend flat (0.01 dB vs 25 dB comb); squash/crest detector shows CLEAN / GLUE / CRUNCH / SQUASHED. Off = original Anchor sound
- ✅ The Aether: particle cloud (granular grains from the last ~3 s; XY pad = texture/density with live particle view; scatter, spread, pitch unison/octave up/down/fifth/shimmer, freeze) + delay (1/16-1 bar tempo-synced or free, feedback, ping-pong, Focus, Soften, Duck against your playing or the Rack Link sidechain, Tape wow/flutter + saturation); both mixes 0 by default = original sound
- ✅ The Mosaic: STFT spectral morph (1024 / 2048 / 4096, 75% overlap; morph 0 reconstructs the input exactly), 4-node XY pad (Air / Warm / Hollow / Radio, right-click a corner to capture what is playing; saved with the session), Morph amount, transient preservation (click fidelity 3.5 → 26 dB); latency (4096) only while on
- ✅ The Void: procedural acoustic modeller - draw the floor plan (drag/add/remove corners, presets Studio / L-Room / Hexa Hall / Corridor / Cathedral), place sound and listener (arrow keys, MIDI/joystick-mappable); image-source reflections to 2nd order + floor/ceiling with real delays, absorption and direction; 8-line FDN tail at the Sabine RT60 (measured 0.39 s vs 0.43 s); Warp = non-Euclidean path bending; Scramble = phase-scrambling diffusion (same level, L/R correlation 0.03); bioluminescent wireframe view with pulsing reflection paths
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
