# Reed

**A Wurlitzer 200A, rebuilt in fixed point for the M-VAVE FM-1.**

Reed is an open-source custom firmware project for the M-VAVE FM-1. The sound is a
physical model ported from [OpenWurli](https://github.com/hal0zer0/openwurli): seven
modal reed modes, a field-solved electrostatic pickup, a hammer/damper model and the
200A's amplifier character. No samples, no impulse responses.

The engine is portable C99 with **no floating point, no libc and no allocation**. The
same source builds for the host, for WebAssembly and for the FM-1's JieLi AC79
(pi32v2, 360 MHz, no FPU). The firmware layer builds on
[felucca-bsp](https://github.com/hugelton/felucca-bsp), extracted from
[Felucca](https://github.com/hugelton/Felucca).

**Live:** [ograndebe.github.io/fm1-reed](https://ograndebe.github.io/fm1-reed/) ·
[Emulator](https://ograndebe.github.io/fm1-reed/try/) ·
[Documentation](https://ograndebe.github.io/fm1-reed/docs.html) ·
[Source](https://github.com/ograndebe/fm1-reed) ·
[Português](README.pt-BR.md) · [Plan](docs/PLAN.md)

| | |
|---|---|
| Engine | **v0.1, working** — fixed-point voice + behavioural amplifier/speaker |
| Browser emulator | **working** — the real engine as WebAssembly in an AudioWorklet |
| FM-1 firmware | **alpha** — builds a bring-up image; not validated on hardware |
| Web installer | **planned** — no image is published before hardware validation |
| Licence | GPL-3.0-only |

## Try it

The browser emulator is the shipping engine compiled to WebAssembly. It needs a
secure context (localhost or HTTPS):

```sh
./web/build.sh
python3 -m http.server -d web 8080
# open http://localhost:8080/try/
```

Click **Power**, play the 27 keys (F3–G5), use the computer keyboard
(`A W S E D F T G Y H U J K`, octave `Z/X`, sustain `Space`) or connect a MIDI
keyboard (Web MIDI). The five sliders are the same parameters as the firmware:
volume, tremolo, drive, speaker character and tune.

The site under `web/` is plain static HTML/CSS/JS. `.github/workflows/pages.yml`
publishes it to GitHub Pages at <https://ograndebe.github.io/fm1-reed/>.

## Quick start (host)

```sh
make test          # build and run the engine test suite
make reed          # build the offline renderer
./build/reed demo --out demo.wav
./build/reed note --note 60 --vel 100 --seconds 3 --out c4.wav
./build/reed midi --in song.mid --out song.wav
./build/reed bench
```

Requires a C99 compiler and `make`. The renderer reads Standard MIDI Files
(format 0/1, PPQ, tempo changes, note on/off, CC64). The engine is 44.1 kHz,
mono, Q20 output.

## What is actually inside

```
modal reed (7 modes) → hammer dwell/onset → electrostatic pickup
        → preamp → LDR tremolo → volume pot → power amp → speaker
```

- **Reed**: Euler–Bernoulli mode ratios with tip mass, per-note inharmonicity,
  decay, air drag, quadrature oscillators. Note-on tables come from OpenWurli's
  physics, not from samples.
- **Pickup**: the field-solved capacitance-vs-overlap law for four reed
  sections, one moving reed against a fixed plate node. The nonlinearity is in
  the source, exactly as in the 200A.
- **Hammer/damper**: Gaussian contact dwell, register-dependent onset, attack
  noise, progressive felt damping.
- **Chain**: input coupling, asymmetric preamp, Twin-T/LDR tremolo (5.63 Hz),
  the drawn volume-pot network, 33 Hz feedback shelf, closed-loop power amp with
  crossover distortion, and the 4×8″ speaker.
- **Fixed point**: Q30 reed/pickup, Q24 gain staging, Q28 biquads, Q32 phase,
  32×32→64 MACs. Ten voice slots, eight sounding voices, 5 ms steal crossfade.

### Fidelity

The voice is a direct port. Before the FX chain was added, C4 at velocity 100
matched OpenWurli's float voice to **0.2 dB peak** and **0.3 dB in every
spectral band from 20 Hz to 20 kHz**, with the same decay curve.

The preamp, power amp and speaker are a **calibrated behavioural model** in v0.1,
not the nine-node MNA/Newton solver. The tremolo uses the real rate, LDR
attack/release and depth law with a sine Twin-T approximation. OpenWurli's MLP
per-note corrections are off. The exact circuit solver, 2× oversampling and 64
voices are on the roadmap; see [docs/PORTING.md](docs/PORTING.md) and
[docs/PLAN.md](docs/PLAN.md).

## Firmware

```sh
git submodule update --init --recursive
./firmware/build.sh     # needs the JieLi toolchain and Docker on macOS/arm64
```

The alpha image is `firmware/build/reed.bin` (about 84 KB): ALNK0 I2S audio, the
engine in the DMA interrupt, 27 keys + 14 buttons, 7 encoders, the 240×240 LCD,
USB-MIDI input and a boot guard that falls back to UBOOT after two failed boots.
It has **no OTA/installer entry and has not been validated on a physical FM-1**.
Do not flash it as a first test. Read [firmware/README.md](firmware/README.md) and
[BUILDING.md](BUILDING.md) first.

## Project layout

| Path | What |
| --- | --- |
| `engine/` | Portable fixed-point engine, public API and host tests |
| `tools/tablegen/` | Rust generator for the OpenWurli-derived tables |
| `tools/render/` | Offline WAV / MIDI renderer and benchmark |
| `web/` | GitHub Pages site and the WebAssembly emulator |
| `firmware/` | FM-1 alpha application on felucca-bsp |
| `bsp/felucca-bsp` | Hardware layer (git submodule, MPL-2.0) |
| `docs/` | Architecture, porting, hardware, plan and firmware notes |
| `LICENSES/` | Third-party licence texts |

## Documentation

- [docs/PLAN.md](docs/PLAN.md) — phased plan and release scope
- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) — engine structure, data flow, Q formats
- [docs/PORTING.md](docs/PORTING.md) — what is exact, what is behavioural, calibration
- [docs/HARDWARE.md](docs/HARDWARE.md) — FM-1 facts, audio/timer/interrupt layout
- [BUILDING.md](BUILDING.md) — host, emulator and firmware builds, recovery
- [firmware/README.md](firmware/README.md) — wiring, UI, current alpha limits
- [web/docs.html](web/docs.html) — the same material on the site

## Credits

The physics, calibration and Wurlitzer 200A research are
[OpenWurli](https://github.com/hal0zer0/openwurli)'s (GPL-3.0-or-later). The FM-1
hardware facts and update path come from [Felucca](https://github.com/hugelton/Felucca)
and [felucca-bsp](https://github.com/hugelton/felucca-bsp) (GPL-3.0 / MPL-2.0).
The 5×7 font is Adafruit-GFX's (BSD-3-Clause). See [CREDITS.md](CREDITS.md) and
[LICENSING.md](LICENSING.md).

M-VAVE and FM-1 are trademarks of their respective owners. Reed is unofficial and
is not affiliated with or endorsed by M-VAVE, JieLi, Hügelton Instruments or the
OpenWurli authors.
