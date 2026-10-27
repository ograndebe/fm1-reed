# Lab build

Status: **implemented, hardware-unvalidated**. The lab image lets us measure
polyphony/quality profiles on a real FM-1; the release image will keep only the
profiles chosen from those measurements. The profile-selection page is WIP until
the hardware and the test song are available.

## Build

```sh
REED_LAB=1 ./firmware/build.sh   # firmware/build-lab/reed-lab.bin
./firmware/build.sh              # firmware/build/reed.bin (release/alpha)
```

The lab image adds:

- USB-CDC console for profile control and telemetry;
- USB audio capture (UAC1) at 44.1 kHz, so the host can record the same 24-bit
  output the DAC gets;
- USB-MIDI input as before;
- USB product string `FM-1 Reed LAB` and version `REED-LAB 0.2`.

The release image has no CDC/UAC, keeps the boot guard, and defaults to the
compile-time profile (currently `HQ_8` until the measurements are done).

**Do not confuse the two binaries.** The lab image is for measurements; it is not
a release and has not been validated on hardware.

## MIDI CC control (primary)

The lab image accepts MIDI CCs on the USB-MIDI port, on any channel. This is the
path used by the capture runner: send CCs, commit, then play the song on the
same port.

| CC | Meaning | Values |
|---|---|---|
| 20 | VOICES | 1..32 |
| 21 | MODES | 1..7 |
| 22 | PICKUP | 0 full, 127 lite |
| 23 | NOISE | 0 off, 127 on |
| 24 | DRAG | 0 off, 127 on |
| 25 | CHAIN | 0 full, 127 lite |
| 26 | MASTER | 0..30 = −24..+6 dB, 1 dB steps |
| 27 | COMMIT profile | >=64 |
| 120 | all sound off | 0 |
| 123 | all notes off | 0 |
| 1 / 7 / 11 / 64 | tremolo / volume / drive / sustain | existing controls |

The CCs 20..26 only stage a profile; CC27 commits it. The firmware sends no MIDI
acknowledgement, so the runner waits a settle time. Existing CCs (1/7/11/64) are
not staged. The test MIDI file must not use CC20..27; the runner sets the profile
before playback.

USB MIDI only in this alpha: TRS/DIN MIDI is not implemented yet.

## Serial protocol (telemetry / CDC control)

115200 8N1 over the CDC port (the baud rate is ignored by USB CDC). Commands are
one line; responses are one line. This is optional when using MIDI CCs; it gives
CPU/underrun telemetry.

| Command | Meaning |
|---|---|
| `P voices modes pickup noise drag chain master_db` | set a profile; `pickup`/`chain`: 0 full, 1 lite; `master_db`: −24..+6 |
| `S` | statistics: active voices, limit, last/max render µs, late halves, CPU % |
| `C` | clear the per-run max/late counters |
| `R` | reset to `HQ_8` (7 modes, full pickup, noise/drag on, 0 dB) |
| `X` | panic / all notes off |
| `V` | version |

Example:

```
P 16 5 0 1 1 0 0
OK 16 5 16777216
S
STAT active=0 limit=16 last_us=1200 max_us=1800 late=0 cpu=41
```

The commands only touch the app/engine: no flash writes, no clock/rail changes,
no loader/UBOOT access. Profiles are session-only; power-on returns to the
default. That is deliberate for the lab build: a bad profile cannot persist into
a boot loop.

## Capture

**P2/analog is the primary capture path.** Connect the FM-1 P2 output to an audio
interface/line-in, set a level that does not clip, and pass that ffmpeg input to
the runner.

The lab image also exposes a USB audio (UAC) input at 44.1 kHz. It is convenient
but not required; if the host does not see it (macOS composite quirks), use the P2
path. The runner records either one.

## Runner (WIP)

`tools/lab/run_profiles.py`:

- sends the profile over MIDI CCs (default) or the CDC console (`--control cdc`);
- plays the same MIDI file on the FM-1's USB-MIDI port;
- records the P2 input with ffmpeg (or UAC);
- writes WAV + loudness-matched 320 kbps MP3 + a CSV/JSON result set;
- optionally queries CPU/underruns over CDC if `--serial` is given.

The song and the capture device are not chosen yet. The script is a working
skeleton, not a claim that captures exist. See
[the profiles page](../web/profiles.html) for the methodology and the WIP table.

## Safety

- The lab code is firmware, so a bug can crash or reset the unit; the watchdog and
  the two-failed-boots UBOOT guard cover the usual cases.
- It does not write settings to flash, so a profile cannot cause a persistent boot
  loop. OTA/update entry is still not implemented in the alpha.
- 32 full voices is a deliberate over-budget test; expect dropouts. It is not a
  release profile.
- The FM-1's speaker/amplifier are the physical thermal limit, not the DSP core.
  Use a master limiter and moderate volume; avoid long full-scale 32-note tests.
- Do not add clock/PLL, rail, codec/amp-register or raw-flash commands to the lab
  console. Those are the changes that can make a unit hard to recover.

## Current limits

- No final profile menu yet; the release defaults to `HQ_8`.
- Lab profile changes are applied with a short IRQ-off window and all notes off;
  they are not meant to be changed while playing.
- UAC capture and CDC on the same composite are not hardware-validated.
- The MIDI song and the listening protocol are still WIP.
