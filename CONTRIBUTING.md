# Contributing

Reed is a small project with a deliberately narrow engine. The most useful
contributions are measured fixes, hardware reports and documentation.

## Before you start

- Read [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) and
  [docs/PORTING.md](docs/PORTING.md).
- Run `make test` and `./tests/run_tests.sh`.
- For a sound change, render before/after WAVs and describe what changed.
- For a hardware change, say which FM-1 revision and what you measured.

## Engine rules

1. **No floating point** in `engine/` or `firmware/`. No `float`, `double`,
   `math.h` or compiler-rt float helpers. The host tools may use floats.
2. **No libc, no allocation** in the engine. It must build freestanding.
3. **No per-sample transcendentals.** Use tables, quadrature updates or
   per-block coefficients.
4. **Avoid 64-bit division.** 32-bit division at note-on/control rate is fine.
   Read the pi32v2 erratum in [docs/HARDWARE.md](docs/HARDWARE.md).
5. **Keep tables in 32-bit words**; avoid 16-bit loads in DSP loops.
6. **Do not edit `engine/src/wurli_tables.c` by hand.** Change the generator,
   regenerate, and run the tests.

## Sound changes

- The golden hash in `engine/tests/test_engine.c` must only change when the sound
  change is intended. Include the old/new hash in the PR.
- A voice change should be compared with OpenWurli's float renderer (see
  [docs/PORTING.md](docs/PORTING.md)). State the peak, band and decay deltas.
- A chain change should include an A/B recording or a band-energy table.
- No samples, no IRs, no "just add an EQ" fixes to hide a modelling mistake.

## Firmware changes

- Build with `./firmware/build.sh`; the image must fit the XIP budget.
- Do not add a hardware fact that is not in felucca-bsp or measured on a unit.
  Link the measurement.
- Keep the boot guard and watchdog path intact. A build that can brick a unit
  without recovery is not acceptable.
- The alpha image has no OTA entry; adding one is a reviewed change, not a
  drive-by.

## Licensing

By contributing you agree to license your change under GPL-3.0-only. Do not copy
code from a project whose licence is incompatible with GPL-3.0. If you adapt
MPL-2.0 BSP code, keep the file's MPL header and note the change. Add an SPDX
identifier to new files.

## Style

- C99, 4 spaces, no tabs. Match the surrounding code.
- Prefer explicit integer widths (`int32_t`, `uint32_t`).
- Comments explain *why* a bound or a simplification exists, not what the line
  does.
- Keep the engine API small; the firmware and the worklet are the only clients.

## Reporting

Open an issue with:

- what you expected, what happened;
- the command or the page and browser;
- for hardware: FM-1 revision, firmware image, whether UBOOT/rescue works;
- for sound: a WAV or a spectrum, not just "it sounds off".
