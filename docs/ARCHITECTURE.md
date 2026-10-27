# Architecture

## Layers

```
OpenWurli (Rust, physics) ──[tools/tablegen]──► engine/src/wurli_tables.c
                                                      │
                          engine/src/wurli_engine.c ──┴── engine/include/wurli.h
                              │                 │                  │
                      tools/render         web/wasm_glue.c     firmware/src/main.c
                      (host WAV/MIDI)      (AudioWorklet)      (felucca-bsp)
```

The engine has no platform dependencies. It reads only the generated tables and
writes Q20 mono samples. The host, browser and firmware layers own audio I/O,
events and the UI.

## Files

| File | Role |
| --- | --- |
| `engine/include/wurli.h` | public API and the two state structs |
| `engine/include/wurli_tables.h` | generated table interface |
| `engine/src/wurli_engine.c` | voice allocation, reed, pickup, FX, renderer |
| `engine/src/wurli_tables.c` | generated data (do not edit) |
| `engine/tests/test_engine.c` | assert-based host tests |
| `tools/tablegen/src/main.rs` | generator (pinned OpenWurli commit) |
| `tools/render/render.c` | offline CLI, SMF parser, WAV writer |
| `web/wasm_glue.c` | static engine + output buffer for wasm |
| `firmware/src/main.c` | FM-1 app, audio/timer ISRs, LCD, key/encoder/USB MIDI |

## API

```c
wr_engine_t e;
wr_init(&e, 44100);
wr_note_on(&e, 60, 100);
wr_note_off(&e, 60);
wr_set_sustain(&e, 1);
wr_set_param(&e, WR_P_TREMOLO, 90);
wr_render(&e, out_q20, frames);
```

Parameters are 0..127 (`WR_P_VOLUME`, `WR_P_TREMOLO`, `WR_P_DRIVE`,
`WR_P_SPEAKER`) except `WR_P_TUNE`, which is −50..+50 cents. There is no
allocation, no file I/O and no callback.

`wr_render` is **not** reentrant. Call it from one thread (the audio thread) and
use the firmware's event queue (or the worklet's message port) to deliver notes
and parameters. `wr_note_on` and `wr_note_off` must not run concurrently with
`wr_render`; the firmware queues them into the DMA interrupt, the worklet
receives them on the audio thread, and the host tests call them in order.

## Number formats

| Data | Format | Notes |
| --- | --- | --- |
| Reed quadrature state | Q30 | 1.0 = 2^30 |
| Mode amplitude/envelope | Q30 | per-32-sample multiplier |
| Pickup node state | Q30 | `m` in Q30, rest at 1.0 |
| Pickup law/evaluation | Q30 | cubic spline, `mh = m·du²/6` pre-scaled |
| FX/gain | Q24 | 1.0 = 2^24 |
| Biquad coefficients | Q28 | DF-II transposed |
| Output bus | Q20 | 1.0 = 2^20 |
| Phase | Q32 | 2^32 = one cycle |
| Velocity/gain tables | Q30/Q24 | generated |

The multiplication helpers (`mul30`, `mul28`, `mul24`) cast to `int64_t`, multiply
and shift. Divisions are 32-bit and occur only at note-on, in the crossover knee
near zero, and in the control-rate tables. There is no 64-bit division, so the
firmware does not rely on compiler-rt helpers.

## Voice lifecycle

- **Allocation**: `WR_MAX_VOICES` slots (8 by default, compile-time). A free slot wins; a same-note held voice is
  retriggered with a 5 ms fade; otherwise the quietest released/oldest voice is
  copied to a fade and the slot restarts.
- **Note-on**: reads the per-note row (phase rotation, amplitudes, decay,
  damper, swing, drag), applies the velocity layer, computes the hammer dwell
  attenuation from the mode ratios and speed, and starts the onset.
- **Per block (32 samples)**: envelope decay, damper gain ramp (per mode), air
  drag, amplitude × envelope.
- **Per sample**: 7-mode quadrature sum, onset, attack noise, pickup law and
  node recursion, post-pickup gain.
- **Note-off**: starts the progressive damper. Top five reeds (MIDI ≥ 92) have
  no damper. Sustain (CC64) delays the damper until release.
- **Stealing**: the old voice is copied into a small fade state and rendered with
  a 5 ms linear gain while the new voice starts. The fade shares the old note's
  rotation and pickup state.

## Pickup

The field-solved table gives capacitance as a function of penetration `u` (mm).
The engine evaluates the C² cubic spline, subtracts the rest value, computes the
small node capacitance change, and runs the trapezoidal recursion:

```
m  = 1 + (C_reed / C_total) · s
w  = (w·(m_avg − β) − (s − s_prev)) / (m_avg + β)
out = w · sensitivity
```

`β = π·f_c/fs`. The reciprocal of `m_avg + β` is held at its rest value
(1/(1+β)); the error is below 0.3 % over the full swing because
`C_reed/C_total = 1/80`. This is the only algebraic simplification in the voice,
and it is documented in the generated table header.

## Shared FX chain

1. Input coupling high-pass (one-pole).
2. Preamp: effective input nonlinearity at a 0.08 V scale, asymmetric bias, then
   the 6.17× stage gain; output normalized to the 14.5 V rail.
3. Tremolo: 5.63 Hz sine Twin-T approximation → half-wave LED drive → CdS
   attack/release envelope → depth divider → gain. The real circuit's shunt
   network is reduced to a gain law; timbre modulation is not modelled in v0.1.
4. Volume pot: OpenWurli's R-11/10 kΩ audio taper table, Q24.
5. Power amp: 33 Hz feedback shelf, closed-loop 69.2×, tanh rail clip, and a
   sharp crossover knee that supplies the high-order harmonics the circuit's
   Class AB stage produces.
6. Speaker: polynomial (`0.2·c·x² + 0.6·c·x³`), tanh excursion limit, 30 Hz
   high-pass / 6.5 kHz low-pass, dry/wet character blend.
7. Final 12 Hz DC blocker and +4 dB output alignment.

The v0.1 chain is behavioural. See [PORTING.md](PORTING.md) for what is exact and
what the next phase replaces.

## Firmware integration

- `firmware/src/main.c` is a unity build on felucca-bsp: it includes `usb.c`,
  `libc.c` and the engine.
- Main loop: key scan, note/button edges, encoders, USB MIDI drain, LCD updates,
  watchdog feed.
- Note/param events go into a 64-entry queue consumed by the audio ISR. The ISR
  drains the queue, renders 128 frames, writes 24-bit stereo into the free DMA
  half and acknowledges.
- TIMER5 runs at 10 kHz for the input/LED tick and polls USB at 2 kHz. Priority 4
  (above ALNK0 at 3), matching Felucca, so USB polling cannot be starved by a
  render.
- The engine state is in `.bss`; the tables are `.rodata` in XIP. The image is
  about 84 KB.

## Memory and CPU budgets

| Item | Size / cost |
| --- | --- |
| Engine state (8 voices) | ~6 KB RAM |
| Audio DMA buffer | 2 × 128 × 2 × 4 B = 2 KB RAM |
| Tables | ~30 KB flash |
| Firmware text | ~84 KB total image |
| Host benchmark | ~169 ns/sample for 8 voices (Apple silicon, 44.1 kHz) |
| Device budget | 2.9 ms per half buffer; target <70 % used |

The host number is not a device measurement. The Phase 3 exit criteria require
the real FM-1 timings (the BSP has cycle/stall counters).
