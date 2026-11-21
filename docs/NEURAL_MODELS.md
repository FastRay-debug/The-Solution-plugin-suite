# Neural Character models

Every module has a **🧠 CHARACTER** panel that runs a neural model of real hardware — a tape
machine, tube preamp, console channel, compressor colour — on the module's sound.

## Where models go

Put model files (`.json`) in the shared Models folder (the panel's **📂 MODELS FOLDER** button opens it):

- Windows: `%APPDATA%\Billet Audio\The Solution\Models`
- macOS: `~/Library/Application Support/Billet Audio/The Solution/Models`

Every module sees the same folder. The chosen model is saved with your session.

## What kind of model

- **RTNeural / Keras-style JSON** (the format exported by GuitarML-style trainers and used by AIDA-X).
- **Mono in, mono out** (1 input, 1 output). The suite runs one copy per channel.
- Typical architectures: LSTM or GRU with 8–32 units, optionally followed by a dense layer.
  Bigger models sound no better for subtle colour and cost more CPU.
- Add `"samplerate": 48000` (or whatever you trained at) to the JSON so the panel can warn when
  your session runs at a different rate.
- Neural Amp Modeler `.nam` files are a different format and are not supported.

Minimal example (a model that halves the signal — useful only for testing):

```json
{ "in_shape": [null, null, 1], "samplerate": 48000,
  "layers": [ { "type": "dense", "activation": "", "shape": [null, null, 1], "weights": [[[0.5]], [0.0]] } ] }
```

## Capturing your own gear

1. **Get a training signal.** Trainers ship an `input.wav` (a few minutes of varied material:
   sweeps, noise bursts, drums, bass, vocals at several levels). Use it as-is.
2. **Play it through the hardware** at the level you'd normally drive it, and record the output
   at the same sample rate (48 kHz is a good default). Keep the round trip sample-aligned — most
   trainers include a tool to line the files up.
3. **Train** with a trainer that exports RTNeural JSON (for example GuitarML's
   Automated-GuitarAmpModelling). An LSTM-16 or GRU-16 trains in minutes on a GPU, longer on a CPU.
4. **Copy the `.json`** into the Models folder, press **↻ RESCAN**, pick it, and set
   **Drive** to match how hard you drove the hardware. **Amount** blends it with the clean signal.

Only capture gear you own or have permission to model, and check the licence of any
model files you download before shipping them with the suite.
