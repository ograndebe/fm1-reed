# Hardware notes

The hardware facts below are established by
[felucca-bsp](https://github.com/hugelton/felucca-bsp)'s board layer and its
measurements, not re-derived here. Reed consumes that layer.

## M-VAVE FM-1

| Item | Fact |
| --- | --- |
| SoC | JieLi AC79 (WL82 family), pi32v2 core |
| Clock | 360 MHz under M-VAVE's bootloader (320 MHz under m-bootloader 1.2); 24 MHz crystal |
| FPU | none — fixed point only |
| RAM | 96 KB usable for this application (72 KB `.data/.bss` + pool in Felucca's map) |
| XIP flash | about 0x8DFBC bytes (~580 KB) at 0x02000120 |
| Keys | 27 notes, F3–G5 (MIDI 53–79) |
| Buttons | 14 (the BSP ids 0..13; 0/1 are octave down/up) |
| Encoders | 7 |
| Display | ST7789 240×240 IPS, SPI |
| LEDs | 41 (one per key/button) |
| Master pot | ADC channel, 10-bit |
| Audio | ALNK0 I2S to an external codec; double-buffered DMA |
| USB | USB0 full-speed device: MIDI, CDC, optional UAC audio |
| Update | mask-ROM UBOOT / JieLi update mode, Felucca loader and package |

## Audio path

- The BSP owns the codec bring-up and ALNK0. The application supplies a buffer of
  two halves, each `HALF_FRAMES=128` stereo frames, as `int32_t` 24-bit
  left-justified samples.
- I2S runs at about 44,117.6 Hz (TIMER4 tick constant `DAC_TICKS=544` per frame).
  Reed's engine is 44.1 kHz; the 0.04 % difference is 0.7 cents at A4.
- The DMA interrupt (`FM1_IRQ_ALNK0`) asks which half is free, fills it, and
  acknowledges. One half is 2.9 ms; that is the render deadline.
- Felucca shifts Q15 to 24-bit with `OUT_SHIFT=7` (−6 dBFS ceiling). Reed maps its
  Q20 output with `<<3`, i.e. 1.0 Q20 = 2^23, full 24-bit scale. The engine's
  output alignment (+4 dB) leaves headroom for eight voices.

## Interrupts and timers

| Source | Use | Priority |
| --- | --- | --- |
| ALNK0 | audio half-buffer | 3 |
| TIMER5 | 10 kHz input/LED tick, USB poll every 5th tick (2 kHz), millisecond counter | 4 (above ALNK0, as Felucca) |
| TIMER4 | free-running 24 MHz time base | — |
| exception | fault report, reset | 7 |

The FM-1's second core exists and is used by Felucca for some parts. Reed v0.1
stays on CPU0; the plan can move the shared FX chain to CPU1 if the render
measurement demands it. The second core has a documented shared-RAM misread on
some units; use it only with the BSP's handshake/retirement checks.

## CPU erratum

`felucca-bsp/docs/CPU.md` documents a pi32v2 issue: 32×32→64-bit multiplies
inlined among 16-bit loads occasionally corrupt a register and crash. The
conditions found so far:

- 16-bit loads beside 64-bit multiplies: crashes within a minute in an AAC
  decoder.
- 32-bit loads beside 64-bit multiplies: 2 errors in 4 billion.
- Moving the multiplies into `noinline` helpers made the decoder clean.

Reed keeps its tables in 32-bit words and has no 16-bit loads in the DSP loops.
The engine's 64-bit multiplies are in `mul30`/`mul24`/`mul28` and are natural
candidates for `noinline` if a unit misbehaves. This is a hardware risk, not a
compiler warning; it must be tested on the target.

## Boot, update and recovery

- A standalone FM-1 image is linked at `0x02000120`; the BSP's `crt0.S` copies
  `.data` and `.ram_text`, starts the clocks and jumps to `fm1_cstart`.
- Reed's alpha image adds a boot guard: two boots that do not reach 2 s in the
  main loop write the UBOOT request and reset. That is the safety net for a bad
  build, but it is not a substitute for the official recovery path.
- Installing custom firmware is at your own risk. Recovery needs the stock/UBOOT
  path; Felucca's documentation points at
  [FM-1-transporter](https://github.com/kurogedelic/FM-1-transporter) and
  `felucca-bsp/tools/fm1_rescue`. Read those before flashing anything.
- Reed v0.1 does **not** include the OTA entry or a `.fwsc` package. Do not treat
  `firmware/build/reed.bin` as an installable release.

## Pin map and board facts

Do not duplicate the pin map here: it belongs to
`bsp/felucca-bsp/boards/fm1/` and is maintained with the hardware tests. In
particular:

- `fm1_input.h` — columns/rows, key ids, LED shift registers, encoder tables,
  velocity/pressure option.
- `fm1_lcd_hw.h` — SPI1, D/C, CS, backlight.
- `fm1_audio.h` — ALNK0, codec control lines, DMA half protocol.
- `fm1_adc.h` — master pot, battery.
- `fm1_usb.h`, `chip/usb.c` — USB0 device, MIDI/CDC/UAC.

If a board revision changes, update the BSP and this project inherits it.
