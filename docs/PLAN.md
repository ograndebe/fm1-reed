# Plan

Reed is built in phases so that the sound can be judged before the hardware
exists, and the hardware can be brought up with a known-good engine. This file is
the contract for each phase: scope, acceptance criteria and the risks that decide
whether the next phase is allowed to start.

## Goals

1. A Wurlitzer 200A model that runs in real time on the M-VAVE FM-1.
2. The voice derived from OpenWurli's physics, not from samples.
3. One portable engine source for host, browser and device.
4. A browser emulator that sounds and behaves like the firmware.
5. A documented, recoverable install path.

Non-goals for v0.1: presets, sequencer, multiple engines, Bluetooth, WiFi, USB
audio, polyphony above 8, 48 kHz, MLP corrections, an installer.

## Phase 0 — research (done)

- Read OpenWurli's DSP, calibration and the Wurlitzer 200A research.
- Read Felucca, felucca-bsp and the FM-1 board facts.
- Decide the split: **felucca-bsp** is the hardware layer; Felucca's full
  application is not forked. This keeps the new firmware small enough to fit,
  test and reason about.
- Establish the CPU/memory envelope: 360 MHz pi32v2, no FPU, 96 KB of RAM, about
  560 KB of XIP flash, 8 voices as the working budget.

## Phase 1 — v0.1, desktop engine and emulator (current)

**Deliverables**

- `engine/`: freestanding C99 fixed-point engine, public API, host tests.
- `tools/tablegen/`: pinned OpenWurli table generator.
- `tools/render/`: WAV renderer, Standard MIDI File input, demo, benchmark.
- `web/`: landing page, documentation page, WebAssembly emulator.
- `firmware/`: alpha bring-up image on felucca-bsp.
- Documentation, licensing and CI.

**Acceptance**

- The voice matches OpenWurli's float voice: peak within 0.5 dB, spectral bands
  within 1 dB, pitch within 10 cents, same decay slope.
- Engine tests pass on a clean host build.
- The emulator plays polyphonically with no dropouts and no digital clipping at
  the default settings.
- The firmware image links and fits; it is explicitly **not** claimed to boot on
  hardware yet.
- No sample data, no floating point in the engine, no libc on the device.

**Status:** engine, renderer, tests and emulator are working. The firmware image
builds (84 KB) but has not been run on an FM-1; the update entry is intentionally
absent.

## Phase 2 — v0.2, exact circuit chain

**Goal:** replace the behavioural preamp/power-amp/speaker with the circuit model,
in fixed point, and add 2× oversampling.

Work:

- Port the 9-node preamp MNA/Newton solver (or a reduced-order equivalent that
  preserves its measured transfer) to Q formats. Early experiments show the
  nonlinear device equations are the expensive part; solve at 2× only for the
  mono chain, as OpenWurli does.
- Port the Twin-T oscillator's waveform instead of the sine approximation.
- Port the power amp's closed-loop feedback shelf and crossover table.
- Emit a 48 kHz table set; test a 48 kHz build.
- A/B regression: render a fixed note/chord set with both OpenWurli (float) and
  Reed (fixed) and compare band energies, THD and decay. The acceptance target is
  the same as Phase 1 for the voice, within 1.5 dB for the chain.
- Measure the device cost of the new chain on the FM-1; if it exceeds the audio
  deadline, reduce the solver iteration count or move the chain to the second
  core.

**Exit:** the chain is exact at the configured oversampling, or the remaining
difference is documented and accepted.

## Phase 3 — v0.3, hardware bring-up

**Goal:** the alpha image boots, makes sound and plays from the keybed on a real
FM-1.

Work:

- Validate clocks, LCD, key scan, encoders and buttons against felucca-bsp's
  hwtest findings.
- Audio: confirm the DMA halves, 24-bit level, and the 44.1 kHz engine vs the
  44.1176 kHz I2S. Measure the render time per half buffer.
- MIDI: USB device enumeration, note on/off, CC64/7/1/11, panic.
- Add velocity: the BSP exposes key timing; start with two or three velocity
  layers and a calibration pass.
- Watchdog/guard/fault path: two failed boots → UBOOT; crash screen.
- Tune voice count (8 vs 10/12) from measured CPU, not guesswork.
- Add flash settings (volume/tremolo/drive/speaker/tune), then presets.
- Add OTA hooks and the update entry, using felucca-bsp's `ota.c` contract.

**Exit:** a unit can be flashed, played, updated and recovered. Only then is a
`.fwsc` package published.

## Phase 4 — v0.4, installer and release

- Web installer (Web MIDI, Chrome/Edge) using the felucca-bsp package format and
  the loader.
- A release package with SHA-256 sums, the GPL/MPL/BSD notices and a recovery
  guide.
- CI builds the firmware reproducibly and checks the ROM budget.
- Signed/validated image; no installer for untested builds.

## Phase 5 — v1.0, calibration

- A listening pass against the reference recordings and a real 200A if available.
- Factory presets that show the model's range (bark, mellow, tremolo, speaker
  character).
- Final documentation: manual, MIDI implementation chart, troubleshooting.

## Risks and decisions

| Risk | Mitigation |
| --- | --- |
| pi32v2 64-bit multiply/16-bit load bug | Keep tables in 32-bit words, avoid 64-bit divides in hot paths, measure on hardware, add a `noinline` multiply helper if a crash appears. |
| 8 voices vs 64 | The FM-1 has 27 keys; 8 sounding voices is the budget. Steal quietest/oldest, crossfade 5 ms. Increase only with measured headroom. |
| Audio deadline | 128-frame halves (2.9 ms). Measure the worst half on hardware; reduce FX/voice count if over 80 %. |
| Behavioural chain audibly different | Voice is exact; chain is calibrated. Phase 2 closes the gap; the docs state the difference. |
| Flash/ROM | 84 KB image now; tables are 30 KB of it. Room for the exact solver and UI. |
| Installer/recovery | No package until hardware validation. Boot guard and UBOOT path exist. |
| Licensing | Everything is GPL-3.0-compatible; generated tables inherit OpenWurli's GPL. See LICENSING.md. |

## Success metrics

- Single C4 mf: within 1 dB of OpenWurli's plugin alignment.
- Eight-note ff chord: no hard clipping; peak below full scale.
- Render cost on the FM-1: under 70 % of the audio half-buffer in the worst case.
- Emulator: 44.1 kHz, no dropouts, under 100 ms key-to-sound latency on a laptop.
- Flash: image under 300 KB, leaving room for the loader and presets.
