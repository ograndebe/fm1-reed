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

## Serial protocol

115200 8N1 over the CDC port (the baud rate is ignored by USB CDC). Commands are
one line; responses are one line.

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

The lab UAC input exposes the FM-1 as a USB audio device at 44.1 kHz. Record it
with any host tool (ffmpeg, a DAW, or the runner below). If the host does not see
the UAC interface (macOS USB composite quirks), fall back to the FM-1's analog
output into an audio interface and pass that input to the runner.

## Runner (WIP)

`tools/lab/run_profiles.py`:

- opens the CDC port and sets each profile;
- plays the same MIDI file on the FM-1's USB-MIDI port;
- records the UAC input with ffmpeg;
- writes WAV + loudness-matched 320 kbps MP3 + a CSV/JSON result set;
- queries CPU/underruns before and after each take.

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
