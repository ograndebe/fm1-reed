# Licensing

Reed is **GPL-3.0-only**. The full text is in [LICENSE](LICENSE).

## Why GPL-3.0-only

The voice's physics and calibration are derived from
[OpenWurli](https://github.com/hal0zer0/openwurli), which is GPL-3.0-or-later.
The generated tables in `engine/src/wurli_tables.c` are derivative data, so the
engine and its tables are GPL-3.0. GPL-3.0-or-later code may be distributed under
GPL-3.0-only; that is the choice this project makes.

The FM-1 firmware links `felucca-bsp`, which is **MPL-2.0**. MPL-2.0 is compatible
with GPL-3.0 (it has no "incompatible with secondary licences" notice). The BSP's
files keep their MPL-2.0 terms when distributed; the combined firmware is
distributed under GPL-3.0-only, with the MPL notices preserved. The BSP is a git
submodule, so its source and notices travel with the repository; its
`LICENSING.md`, `NOTICE` and `LICENSES/` apply to those files.

Felucca itself (GPL-3.0) was read for the FM-1 application architecture; Reed
uses its hardware layer (`felucca-bsp`) and adapts the BSP hardware-test startup
and LCD patterns (MPL-2.0). It does not copy Felucca's application source.

## Third-party material

| Component | Licence | Where |
| --- | --- | --- |
| OpenWurli physics/calibration | GPL-3.0-or-later | generated tables, docs |
| felucca-bsp | MPL-2.0 | `bsp/felucca-bsp/` (submodule) |
| Adafruit-GFX 5×7 font | BSD-3-Clause | `firmware/src/font5x7.h` |
| JieLi AC79 SDK / toolchain | Apache-2.0 | not vendored; build-time dependency |
| M-VAVE / FM-1 names | trademarks | nominative use only |

The licence texts are in [LICENSES/](LICENSES/). The OpenWurli and felucca-bsp
repositories carry their own notices; this project does not relicense them.

## What is not included

- No Wurlitzer recordings, samples, impulse responses or IRs.
- No proprietary M-VAVE firmware or JieLi binaries.
- The browser site uses system fonts; no external font is served.
- The demo audio is rendered by this engine from an original four-bar pattern,
  not from a recording.

## SPDX

Source files carry SPDX identifiers. The generated table file is marked
GPL-3.0-only and its header names the OpenWurli source and commit. The font file
is BSD-3-Clause. BSP files keep their own MPL-2.0 headers.

## Trademarks

M-VAVE and FM-1 are trademarks of their respective owners. Reed is unofficial and
is not affiliated with or endorsed by M-VAVE, JieLi, Hügelton Instruments, the
OpenWurli authors or the Felucca authors.
