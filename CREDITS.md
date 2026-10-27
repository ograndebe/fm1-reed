# Credits

Reed stands on a lot of careful work. None of these people or projects endorse
this firmware; mistakes here are Reed's.

## OpenWurli

The Wurlitzer 200A research, the modal reed model, the electrostatic pickup field
solve, the hammer/damper model, the circuit characterisation and the per-note
calibration are from [OpenWurli](https://github.com/hal0zer0/openwurli) by
**hal0zer0** and contributors. The project's `docs/research/` and the calibration
notes are the reason the voice can be ported at all. OpenWurli also thanks Doc
Wurly, OldBassMan, BustedGear and the Wurlitzer repair community; that debt
travels with this port.

## Felucca and felucca-bsp

[Felucca](https://github.com/hugelton/Felucca) by **Leo Kuroshita
(@kurogedelic), Hügelton Instruments** is the proof that a custom firmware fits
the FM-1, and the source of the application architecture this project learned
from. [felucca-bsp](https://github.com/hugelton/felucca-bsp) is the hardware
layer Reed actually builds on: the AC79 clock/IRQ/flash/USB work, the FM-1 board
files, the update loader and the CPU errata. Contributors include **Charles
Vestal**, **Kerem Kilic (Ellic Studio)**, **Jangada (@zednaked)** and others named
in the BSP's `NOTICE`.

## Hardware and tools

- **M-VAVE** for the FM-1 and its documentation-free hardware, which the
  community reverse-engineered.
- **JieLi** for the AC79 SDK and the pi32v2 toolchain (Apache-2.0).
- **Adafruit** for the classic 5×7 bitmap font used by the bring-up UI
  (BSD-3-Clause).
- **Emscripten/LLVM** for the WebAssembly build.

## Fonts, audio and art

The browser site uses system fonts only. The demo audio is rendered by Reed's
engine from an original four-bar pattern; it is not a recording and not a sample
library. The font in the firmware LCD is Adafruit's; see
[LICENSES/BSD-Adafruit-GFX.txt](LICENSES/BSD-Adafruit-GFX.txt).

## AI assistance

Reed is developed with AI coding assistance for code, tests, documentation and
the site. The architecture, the choice of what to port, the calibration targets
and the listening decisions remain human responsibilities. No generative AI is
used for audio or artwork.

If your work is here and not credited, please open an issue; it will be fixed.
