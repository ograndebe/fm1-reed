# Polyphony and quality profiles

Status: **analysis / design**, not implemented. This document answers how to let
the FM-1 run more notes by spending less per voice, how to expose that on the
hardware, and what can go wrong.

## 1. What exists today

- The engine has a compile-time array of `WR_MAX_VOICES` voices
  (`engine/include/wurli.h`). The default is 8; the CI also builds a 32-slot
  capacity variant.
- `wr_init()` zeroes the whole engine. No voice is active until `wr_note_on()`.
- `wr_render()` walks the slots and calls `voice_block()` only for `v->active`.
  Inactive slots do no DSP. There is **no hidden 64-voice engine** and no
  preloaded note.
- The host benchmark (Apple silicon, 44.1 kHz, full 7-mode voice):

| Active voices | ns/sample | Realtime |
|---|---|---|
| 8 | 186 | 122× |
| 16 | 322 | 70× |
| 24 | 470 | 48× |
| 32 | 619 | 37× |

This is a host number, not the AC79. It shows the scaling is linear in active
voices and that 32 voices is ~3.3× the cost of 8.

Memory is not the blocker: `wr_voice_t` is 508 B; a 32-voice engine is 16.4 KB of
`.bss` (96 KB available).

## 2. The levers that actually save CPU

| Lever | Saving | Sound risk |
|---|---|---|
| Fewer modal modes per voice | High: each mode is a rotation + envelope + sum | Drops the faint upper partials; attack loses some “metal” |
| Cheaper pickup | Medium–high: spline + node recursion per voice/sample | The pickup nonlinearity is a major part of the Wurlitzer character |
| Lower audio rate (22.05 kHz + interpolation) | ~2× | Aliasing from the nonlinear pickup; high harmonics |
| Disable attack noise | Small (15 ms per note) | Slightly less mechanical attack |
| Disable air drag / reduce damper updates | Small | Less growl/decay detail |
| Second core (16+16) | ~2× | CPU1 shared-RAM errata, complexity |
| 2× oversampling (future exact chain) | Negative | Better anti-aliasing, more CPU |

The first two are the useful levers. Dropping the attack noise is not a real
quality tier by itself.

### Mode budget

OpenWurli's base mode amplitudes are approximately:

```
[1.0, 0.005, 0.0035, 0.0018, 0.0011, 0.0007, 0.0005]
```

Modes 4–7 are −55 to −66 dB. A 3–5 mode voice is a credible trade for high
polyphony; it must be A/B-checked on the pickup's attack and on chords. Keeping
the first N modes is simple: the per-note tables already contain all 7.

### Pickup budget

The full pickup evaluates a cubic spline and runs the trapezoidal node recursion.
Options, from conservative to aggressive:

1. Linear interpolation in the same law table (small saving, tiny sound change).
2. Coarser table stride (small).
3. Static slope + one-pole high-pass + soft clip (large saving, loses the
   overlap-law harmonics).
4. Full pickup only for HQ; lite for MQ/LQ.

Do not decide this from the host benchmark alone; the pickup is where the
Wurlitzer “bark” lives. A blind CPU cut here can turn the model into a generic
electric piano.

## 3. Proposed configuration model

Separate three things:

1. **Capacity** — compile-time `WR_MAX_VOICES` (32). Memory only.
2. **Active limit** — runtime `voice_limit` (1..32). Real: no more active voices.
3. **Voice quality** — runtime profile (modes, pickup variant, noise/drag). Real:
   fewer operations per active voice.

```c
typedef struct {
    uint8_t max_voices;   /* 8, 16, 32 */
    uint8_t modes;        /* 7, 5, 3 */
    uint8_t pickup;       /* FULL, LITE */
    uint8_t noise;        /* 0/1 */
    uint8_t air_drag;     /* 0/1 */
} wr_profile_t;
```

Proposed default profiles (hardcoded, future simple menu):

| Preset | Voices | Modes | Pickup | Noise | Intended use |
|---|---|---|---|---|---|
| `HQ_8` | 8 | 7 | full | on | default, current sound |
| `MQ_16` | 16 | 5 | full | on | larger chords, close sound |
| `LQ_32` | 32 | 3 | lite | off | dense/sustained passages |
| `CUSTOM` | any | any | any | any | advanced/debug only |

`wr_set_profile(e, profile)` applies to new notes. Changing quality while notes
are held is not sample-continuous; the menu should either:

- apply on the next note only, or
- fade all notes (5 ms) and restart the profile, like a patch change.

For a menu preset, the second is cleaner. For an advanced “live” experiment, the
first is less disruptive.

### Why this is not fake

A profile is real only if:

- `wr_note_on()` never activates more than `voice_limit` voices;
- `voice_block()` loops only `profile.modes` modes, not all 7;
- the pickup call is the full or lite implementation, not the full one plus a
  flag;
- noise/air-drag code is skipped, not computed and discarded;
- lowering the limit fades/deactivates excess voices.

Tests should prove it:

- limit 4 + 32 note-ons → `wr_active_voices() == 4`;
- host render time for 32 LQ < 32 full, and close to 16 full;
- golden hashes per profile (separate, intentional);
- on hardware, the CPU meter drops when the profile changes under a sustained
  chord. If it does not, the profile is fake.

## 4. Feasibility estimate

Let `C_full` be the current cost per full voice and `C_chain` the shared chain.
Rough per-voice shares from the host benchmark:

- `C_full` ≈ 18–19 ns/sample; `C_chain` ≈ 30–40 ns.
- 8 full ≈ 186 ns; 16 full ≈ 322 ns; 32 full ≈ 619 ns.

Expected profile costs (to be measured, not promised):

- 16 MQ, 5 modes, full pickup: ~0.8 × C_full per voice → ~16 full-equivalents ×
  0.8 ≈ 13 full-equivalents. Similar to 12–13 full voices.
- 32 LQ, 3 modes, lite pickup: ~0.45–0.6 × C_full → ~14–19 full-equivalents.

So the intended 8/16/32 set is roughly **1× / 1.4× / 1.7–2.4×** the current
8-voice load, not 4×. Whether that fits depends on the AC79 factor:

```
CPU_fraction ≈ host_seconds_per_sample × 44100 × F
```

where `F` is the AC79 slowdown versus the host (unknown until measured; likely
20–40× for this scalar, XIP, no-FPU core). If 8 full uses 20–25 % of the half
buffer, 32 LQ at ~2× would use 40–50 %; plausible. If 8 full already uses 35 %,
32 LQ may exceed 70 % and needs more cuts or two cores.

**32 full on one core is not a target.** It is ~4× the current load and leaves
no margin for the exact circuit chain, USB, LCD and interrupts. If 32 *full* is a
hard requirement, the realistic path is 16+16 on both cores, with the CPU1
errata and fallback work.

## 5. Hardware exploration

1. Implement the engine profile + limit first, plus a CLI/emulator selector.
   Validate sound and host cost.
2. Add a firmware debug page:
   ```
   POLYPHONY   8 / 16 / 32
   QUALITY     HQ / MQ / LQ / CUSTOM
   ACTIVE      0..32
   CPU         0..100%
   SAVE
   EXIT
   ```
   Encoder 5 navigates, encoder 6 changes, button 4 enters/exits, button 5
   saves. The current firmware uses encoders 0–4 and buttons 0–3; 5/6 and 4–13
   are free.
3. Measure on the FM-1:
   - 8 HQ, 16 MQ, 32 LQ with a sustained chord, sustain pedal and tremolo;
   - worst case: 32 notes + speaker character 100 + volume 100;
   - `audio_max_us`/CPU per half buffer; target <70 %;
   - 30 min thermal/battery run; output level/clipping; listen for dropouts.
4. Tune the profile mode counts/pickup from the measurements, then freeze the
   presets and hide `CUSTOM` from the normal menu.

The CPU meter is essential: without it the user cannot tell whether 32 LQ is
safe on their unit.

## 6. Persistence

Settings fit in the BSP's firmware-data area:

- `FL_XDATA0_LO = 0x93000` .. `FL_XDATA0_HI = 0x97000` (`fm1_flash.h`).
- The loader writes only below `0x93000`; keep bytes `0xF00..0xF07` of each
  4 KB sector erased (the loader looks for an update record there).
- Use two sectors (A/B) with a small record: magic, version, `voice_limit`,
  `profile`, other params, CRC32. Invalid CRC → `HQ_8`.
- Save only on an explicit SAVE/menu exit, not on every encoder click. Flash
  erase/program blocks XIP; expect a short audio gap. Do it with notes released;
  show “SAVING”.
- Flash endurance is not a concern at menu-save frequency; A/B protects against
  power loss.

## 7. Risks

### Audio dropouts and clicks
If the render exceeds the 2.9 ms half-buffer, the DMA underruns: clicks, dropouts,
possibly a DC step. This is the most likely failure. It does not damage the
hardware. Mitigate with a CPU meter, a safe default, and optionally automatic
voice shedding (Felucca has a shedder). Do not let the menu set 32 and leave it
there if the CPU is red.

### Crash / reset / boot loop
A bug in a new profile, a bad pointer, or CPU1 can crash the app. The watchdog
and the two-failed-boots → UBOOT guard cover most cases. A saved `LQ_32` profile
that crashes during early render could make a boot loop; mitigate with:
- settings CRC and fallback to `HQ_8`;
- a safe-mode key (hold PANIC at boot) that ignores stored settings;
- boot guard/Ubooot;
- test profiles on hardware before saving them as defaults.

### Flash and bricking
- Writing only `FL_XDATA0` cannot corrupt the app, loader or UBOOT.
- Power loss during a settings save can corrupt that record; A/B + CRC fixes it.
- Power loss during a firmware **update** can corrupt the app; the loader/UBOOT
  recovery is the safety net. That is a different operation from changing a
  voice profile.
- A “brick” here means the app does not boot. Most cases are recoverable through
  UBOOT/rescue. A hard brick (bootloader/loader damaged) is rare and needs the
  rescue path; do not touch those areas for a voice setting.

### Thermal, current and battery
- The FM-1 runs the AC79 at its stock 360 MHz; the profile does not change
  voltage or clock. More voices mean more CPU activity, more current and more
  heat, but **DSP load alone does not “burn” the chip**. The device is designed
  for continuous audio; still, test case temperature after 30 min at 32 LQ.
- Battery life (if running on battery) will drop with higher polyphony.
- Do not overclock or raise rails to chase 32 voices. The BSP calls a higher
  clock a concept, not a validated feature.
- The real thermal risk is the **audio path**: 32 voices sum ~12 dB louder than
  8. Continuous full-scale/clipped output can heat the speaker/amp and is bad for
  hearing. Keep a master limiter/gain, test at moderate volume, and do not run a
  32-note fff chord at maximum for hours.
- The exact circuit chain (v0.2) may add CPU and heat; measure after it lands.

### CPU1 (dual core)
The second core is documented to misread shared RAM on some FM-1s. Enabling it
can crash/reset; use the BSP's handshake, check word and retirement/fallback. It
does not damage the hardware. Do not make 32 voices depend on CPU1 without a
one-core fallback.

### Hearing
High SPL is a health risk, not a hardware one. Use headphones at moderate level;
the emulator and hardware share the same output level.

## 8. What I would not do

- No dynamic allocation; 32 static slots are cheap and predictable.
- No overclock or rail changes.
- No writing the loader/UBOOT area for settings.
- No “32 voices” label unless 32 profiles are measured and the quality trade is
  documented.
- No per-sample branch on `profile.modes` if it can be avoided: specialize the
  voice block per profile or hoist the branch. The mode loop bound is a runtime
  value but the compiler can still optimize; measure before micro-optimizing.
- No change of quality while a chord is held without a fade; it will click.

## 9. Recommended order

1. Runtime `voice_limit` + capacity 32 + tests/benchmark (no menu).
2. Profile struct + HQ/MQ/LQ variants; A/B and host cost.
3. Emulator/CLI selector; listen to the profiles.
4. Firmware debug menu + CPU meter; hardware measurements.
5. Persistence (A/B, CRC, safe mode).
6. Freeze the three presets; hide custom controls.

Only after step 4 can we honestly say “32 LQ works on the FM-1”. Until then, 32 is
a design target, not a promise.
