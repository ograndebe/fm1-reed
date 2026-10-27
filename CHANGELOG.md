# Changelog

## 0.1.0 — 2026-10-26

First public tree.

- Fixed-point Wurlitzer 200A engine in portable C99: 7-mode reed, hammer dwell,
  electrostatic pickup overlap law, attack noise, 8 voices with 5 ms steal fades,
  sustain and progressive dampers.
- Behavioural 200A chain: preamp asymmetry, 5.63 Hz LDR tremolo, volume-pot
  network, 33 Hz feedback shelf, crossover knee, speaker character, DC blocker.
- `tools/tablegen` (Rust) generates all per-note and pickup data from a pinned
  OpenWurli commit. The generated C is committed.
- Host tests (assert-based), golden fixed-point hash, 30 s stability, pitch,
  velocity, damper, sustain and stealing checks.
- Offline renderer: WAV, Standard MIDI File input, demo pattern, benchmark.
- Browser emulator: standalone wasm32 engine, AudioWorklet, 27-key UI, five
  parameters, computer keyboard and Web MIDI.
- Static GitHub Pages site and documentation page; Pages/CI workflows.
- FM-1 alpha firmware on felucca-bsp: 84 KB image, ALNK0 audio, 27 keys,
  encoders, LCD, USB-MIDI input, boot guard. **Not hardware-validated; no OTA.**
- Licensing and attribution for OpenWurli, Felucca, felucca-bsp and the
  Adafruit-GFX font.

Known limits: 44.1 kHz, 8 voices, no MLP, no exact circuit solver, fixed key
velocity in the firmware, no installer, no presets, no USB audio, no reverb.
