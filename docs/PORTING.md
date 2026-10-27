# Porting notes

This is the honest map of the port: what came across unchanged, what was
rewritten for fixed point, what is behavioural in v0.1, and how each claim was
checked.

## Source and pin

The physics and per-note calibration come from
[OpenWurli](https://github.com/hal0zer0/openwurli) at commit
`531680b5b49eb4a734a8d8ef7a9c13cb83cf79f9`. The pin is in
`tools/tablegen/Cargo.toml`. The generated file is committed so an engine build
does not need Rust or network access.

`tools/tablegen` calls OpenWurli's public functions rather than copying formulas:

- `tables::note_params` — mode ratios, amplitudes, decay rates.
- `tables::pickup_swing_mm_with_config` and `air_drag_db_per_mm_with_config`.
- `tables::velocity_exponent`, `velocity_scurve`, `output_scale_with_config`.
- `tables::volume_pot_gain`.
- `variation::freq_detune`, `variation::mode_amplitude_offsets`.
- `pickup::{pickup_law_for, pickup_band_sign, PICKUP_REST_OFFSET_MM,
  PICKUP_SENSITIVITY, PICKUP_FC}` and the four `PickupLawTable` structures.

If OpenWurli changes a calibration, regenerate the C file; do not hand-edit it.

```sh
cd tools/tablegen
cargo run --release > ../../engine/src/wurli_tables.c
```

## The voice: direct port

### Reed

OpenWurli's `ModalReed` is seven damped quadrature modes. The fixed-point version
uses the same state and update:

```
s' = s·cos + c·sin
c' = c·cos − s·sin
out += amp·env·s
```

- Rotation coefficients are Q30, generated once per note from the untuned
  increment; a tuned note (`WR_P_TUNE`) derives them from the shared sine table.
- Decay is applied per 32-sample block: `env *= exp(−α·32/fs)`. The error versus
  the per-sample decay is below the Q30 resolution.
- The onset ramp is the raised cosine table; pp uses its square, ff the cosine,
  matching OpenWurli's two cases.
- Air drag on mode 1 is applied per block with the linearised update
  `env *= 1 − drag·env`, as in the reference.
- Quadrature radius renormalisation runs every 1024 samples (integer sqrt, Q15
  reciprocal). It is needed because fixed-point rotation slowly changes radius.

### Hammer and damper

- Dwell attenuation is computed at note-on from the mode ratio and the
  velocity-dependent cycles (`0.75..1.0` of the fundamental period; the dwell
  formula reduces to `ft = ratio · cycles`). A 1/16-step exponential table gives
  `exp(−z)` with linear interpolation.
- The onset length is `max((1+(1−v))/f0, 2 ms)`, converted to samples with a
  32-bit divide at note-on.
- The damper uses OpenWurli's per-mode rates (`base·3^m`, capped at 2000 s⁻¹)
  and the register-dependent ramp. The per-block gain is linearly interpolated
  across the ramp, then held at the per-mode value. This is a time-quantised
  version of the reference's per-sample exponential; the audible difference is
  below the block length (0.7 ms).
- Top-five reeds (MIDI ≥ 92) have no damper.

### Pickup

The four field-solved capacitance tables are stored as Q30 splines. The engine
evaluates them with the same natural-cubic formula and the pre-scaled second
derivative (`mh = m·du²/6`), then runs the trapezoidal node recursion.

Differences from the float version:

1. The reciprocal of `m_avg + β` is held at its rest value `1/(1+β)`. The node
   capacitance moves by at most `C_reed/C_total = 1/80` of the source; the
   resulting error is <0.3 %, i.e. below 0.03 dB. This removes a per-sample
   division.
2. `m_avg` is computed in 64 bits (the two Q30 values overflow int32 when added).
3. The spline position uses a precomputed `1/du` Q16 and a 64-bit product; the
   table lookup is clamped at the last knot, as in the reference.

### Attack noise

The LCG, band-pass coefficients (RBJ constant 0 dB peak, Q = 0.7) and the 15 ms
exponential decay are generated per note. The 16-sample raised-cosine fade-in is
kept so simultaneous note-ons do not produce a step.

## Validation

Before the FX chain existed, the engine was compiled with `-DWR_DRY` (voices only)
and compared with `openwurli/tools/reed-renderer` for C4, velocity 100:

| Metric | OpenWurli | Reed | Difference |
| --- | --- | --- | --- |
| Peak | −40.41 dBFS | −40.23 dBFS | 0.18 dB |
| RMS, 0–0.1 s | −45.8 dB | −45.6 dB | 0.2 dB |
| RMS, 0.1–0.3 s | −46.2 dB | −46.1 dB | 0.1 dB |
| RMS, 0.3–0.6 s | −47.1 dB | −47.0 dB | 0.1 dB |
| RMS, 0.8–1.2 s | −49.5 dB | −49.3 dB | 0.2 dB |
| F0 | 262.5 Hz | 262.5 Hz | <0.1 cent |
| 20–100 Hz band | −8.4 dB | −8.7 dB | 0.3 dB |
| 100–300 Hz | +18.9 dB | +18.6 dB | 0.3 dB |
| 1–3 kHz | −3.1 dB | −3.0 dB | 0.1 dB |
| 3–10 kHz | −32.0 dB | −32.3 dB | 0.3 dB |

The engine's own test suite additionally checks pitch, velocity, damper, sustain,
stealing, 30 s stability and a golden hash.

## The chain: behavioural in v0.1

OpenWurli's preamp is a nine-node MNA system with a Newton solve on two BJT
kernels; its power amp is a closed-loop nonlinear solver; the tremolo is a
generated Twin-T circuit. A literal fixed-point port is a phase of its own, not a
weekend change:

- The preamp's Newton iterations need `exp` and `sqrt` per device per iteration.
  At 88.2 kHz that is millions of transcendental evaluations per second, and the
  pi32v2 has no FPU.
- The circuit's dynamic range (0.6–15 V, µA–mA) needs careful per-node scaling.
- The power amp's solver has known convergence edge cases in the float code.

v0.1 therefore uses calibrated behavioural stages:

| Stage | v0.1 | Exact target |
| --- | --- | --- |
| Preamp | input HPF, tanh asymmetry at 0.08 V, 6.17× | 9-node MNA + Newton |
| Tremolo | 5.63 Hz sine → LDR attack/release → gain | generated Twin-T waveform |
| Power amp | 33 Hz shelf, 69.2×, tanh clip, crossover knee | closed-loop 7-BJT solver |
| Speaker | polynomial + tanh + 30 Hz/6.5 kHz | same model, with cutoff interpolation |
| Oversampling | none | 2× on the mono chain |
| MLP | off | optional per-note corrections |

The behavioural chain is calibrated against OpenWurli's full render, not guessed:
C4 at velocity 100, volume 0.8, speaker 1.0. The 2–5 kHz band matches within
1 dB; the reference has more very-high-frequency content (above 5 kHz) because its
circuit solver's Class AB crossover generates high-order harmonics. A sharp
crossover knee in the firmware chain recovers most of it (about 3 dB short above
5 kHz). The next phase replaces the approximation rather than chasing it with EQ.

## Fixed-point rules

- Never use `float`/`double` in `engine/` or `firmware/`. The host tooling may.
- Prefer Q30 for oscillators and the pickup; Q24 for gain and filters.
- A 32×32→64 multiply is one instruction on the pi32v2, but **the chip has a
  documented erratum**: a 64-bit multiply inlined among 16-bit loads can corrupt a
  register. Keep tables in 32-bit words and avoid 16-bit accesses in hot loops. If
  a crash appears, move the multiply into a `noinline` helper (see
  `felucca-bsp/docs/CPU.md`).
- Avoid 64-bit division. The engine has none; note-on uses 32-bit division, and
  the crossover uses a 32-bit reciprocal only near zero.
- No per-sample `sin`, `exp` or `sqrt`: tables, quadrature rotations, or
  per-block coefficient updates.

## Known limitations

- 8 voices by default (`WR_MAX_VOICES`, compile-time; `make WR_MAX_VOICES=10`),
  not OpenWurli's 64.
- 44.1 kHz only. The I2S runs at 44.1176 kHz; the 0.04 % difference is 0.7 cents.
- The keybed velocity is fixed at 100 in the alpha firmware.
- The tremolo is amplitude modulation, not the circuit's timbral modulation.
- The speaker cutoffs are fixed at the authentic setting; the character knob
  blends dry/wet and the nonlinearity, not the cutoff.
- No MLP corrections, no samples, no reverb.
- The firmware image has no OTA entry yet.

## How to compare against OpenWurli

```sh
# Reed dry voice (no FX)
cc -O2 -DWR_DRY -Iengine/include \
   engine/src/wurli_engine.c engine/src/wurli_tables.c \
   tools/render/smoke.c -lm -o /tmp/reed-dry
/tmp/reed-dry 60 100 2

# OpenWurli voice, from its tree
cd openwurli
cargo run --release -p reed-renderer -- \
  --note 60 --velocity 100 --duration 2 --output /tmp/ref60.wav
```

`tools/render/smoke.c` is a development helper, not part of the shipping CLI. It
writes `/tmp/reed-note.wav`.

For the full chain, use OpenWurli's `preamp-bench render` with the same volume and
speaker settings, then compare band energies with an FFT. Remember that
`preamp-bench` prints its raw output; the plugin applies a +14 dB alignment on top.
