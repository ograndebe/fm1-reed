# FM-1 firmware (alpha)

This is the hardware application, built on
[felucca-bsp](../bsp/felucca-bsp) (MPL-2.0). It is a **bring-up image**, not a
release: it builds, links and fits, but no one has run it on a physical FM-1 yet.
There is no OTA entry and no `.fwsc` package.

## What it does

- Initialises clocks, watchdog, guards, the 41-key scanner, the ST7789 LCD, ALNK0
  audio and the USB device.
- Runs the Reed engine in the ALNK0 DMA interrupt, 128-frame halves (2.9 ms).
- Plays the 27 keys through the engine; sends note on/off and parameters through
  a 64-entry queue to the audio interrupt.
- Reads USB-MIDI input (the BSP's class-compliant MIDI device) for notes, CC64
  sustain and CC7/CC1/CC11 volume/tremolo/drive.
- Draws the five parameters, note, voice count and octave on the LCD.
- Falls back to UBOOT if two consecutive boots do not reach 2 s in the main loop.

## Controls

| Control | Function |
| --- | --- |
| 27 keys (F3–G5) | notes; velocity is fixed at 100 in this alpha |
| Encoder 1 | volume |
| Encoder 2 | tremolo depth |
| Encoder 3 | preamp drive |
| Encoder 4 | speaker character |
| Encoder 5 | tune (−50..+50 cents) |
| Encoders 6/7 | unused |
| Button 0 | octave down |
| Button 1 | octave up |
| Button 2 | sustain toggle |
| Button 3 | panic (all notes off) |
| Buttons 4..13 | unused |

The button/encoder numbering is the BSP's; the physical labels are not assumed.
The LCD shows the same parameters as the browser emulator.

## Build

```sh
git submodule update --init --recursive
JIELI_TOOLCHAIN=$HOME/.jieli/toolchain ./firmware/build.sh
```

Requirements and Docker details are in [../BUILDING.md](../BUILDING.md). Output:

```
firmware/build/reed.bin     # bare app image, ~84 KB
firmware/build/reed.elf     # symbols
firmware/build/text.bin     # XIP payload
```

The image is linked at `0x02000120` with `firmware/app.ld`. The BSP's
`tools/get_toolchain.sh` fetches the JieLi clang/ld; the AC79 SDK is only needed
for a loader/package, which this alpha does not produce.

## Why there is no installer yet

Installing requires a package (app + update loader) and an application-side
update entry. Reed has neither. The boot guard (`two failed boots → UBOOT`) and
the BSP's rescue tools are the recovery path, but they are not a user-facing
installer. The first `.fwsc` will ship only after the image is played and updated
on real hardware. Do not flash `reed.bin` as a first experiment.

## Known limits / risks

- **Not hardware-validated.** Audio DMA timing, LCD colour/order, key ids,
  encoder direction and USB enumeration are written to the BSP contract but not
  observed on a unit.
- **No OTA/flash settings/presets/velocity.** Fixed velocity 100; no per-key
  velocity/pressure.
- **CPU headroom unknown.** The host benchmark is not a device measurement. Watch
  the first audio half-buffer timings; if the render exceeds ~70 %, reduce voices
  or move the chain to CPU1.
- **pi32v2 erratum.** 64-bit multiplies beside 16-bit loads can crash. The engine
  uses 32-bit table loads; if a crash appears, make the multiply helpers
  `noinline` (see [../docs/HARDWARE.md](../docs/HARDWARE.md)).
- **USB MIDI only.** USB audio (UAC) and the CDC console are compiled in through
  the BSP; the engine does not feed UAC yet.
- **No boot splash/logo.** The LCD comes up when the engine is already running.

## Next steps

1. Flash a unit, confirm the boot guard and LCD; run the BSP's hwtest first.
2. Measure the ALNK0 half-buffer render time; set the voice count from data.
3. Add velocity from the BSP's key timing; add flash settings.
4. Add the OTA hooks and build the loader/package; then the web installer.
5. A/B the exact circuit chain (v0.2) on the device.

## Licensing

`firmware/src/main.c` and `firmware/src/font5x7.h` are GPL-3.0-only (font:
BSD-3-Clause, Adafruit-GFX). The included BSP is MPL-2.0; see
[../LICENSING.md](../LICENSING.md) and the BSP's `NOTICE`/`LICENSING.md`.
