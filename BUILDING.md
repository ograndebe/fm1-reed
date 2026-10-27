# Building

## Prerequisites

| Task | Needs |
| --- | --- |
| Engine, tests, renderer | C99 compiler, `make`, libm |
| Table regeneration | Rust (stable), network for the pinned OpenWurli dependency |
| Browser emulator | Emscripten (`emcc`) with `wasm-ld` |
| Firmware | JieLi pi32v2 toolchain; Docker on macOS/arm64 |
| Demo MP3 | `ffmpeg` (optional) |

The repository uses a submodule for the hardware layer:

```sh
git clone --recurse-submodules <repo>
# or, in an existing clone:
git submodule update --init --recursive
```

## Host engine

```sh
make test          # build + run engine/tests/test_engine.c
make reed          # build build/reed
make bench         # build + run the 8-voice benchmark
```

Manual equivalent:

```sh
cc -O2 -std=c99 -Wall -Wextra -Iengine/include \
   engine/src/wurli_engine.c engine/src/wurli_tables.c \
   engine/tests/test_engine.c -lm -o build/test_engine
./build/test_engine
```

The tests are assert-based and print the measurements they check. The golden test
is a fixed-point hash of one second of C4 ff; the engine has no floating point, so
the hash is deterministic across platforms. Update it only when the sound change
is intended.

## Offline renderer

```sh
./build/reed note --note 60 --vel 100 --seconds 3 --out c4.wav
./build/reed midi --in song.mid --out song.wav \
    --volume 110 --tremolo 70 --speaker 100 --drive 64 --tune 0 --gain 0
./build/reed demo --out demo.wav
./build/reed bench
```

`--volume`, `--tremolo`, `--drive` and `--speaker` take 0..127; `--tune` is
−50..+50 cents; `--gain` is a post-render dB trim. The MIDI reader supports format
0/1, PPQ timing, tempo changes, note on/off and CC64; other controllers are
ignored. Output is 16-bit stereo 44.1 kHz, both channels the mono model.

## Browser emulator

```sh
./web/build.sh
python3 -m http.server -d web 8080
# http://localhost:8080/try/
```

`web/build.sh` compiles `engine/src/*.c` and `web/wasm_glue.c` with Emscripten's
clang into objects, then links a standalone wasm32 module with Emscripten's
`wasm-ld`. The module imports nothing: the AudioWorklet receives the bytes from
the page and calls `WebAssembly.instantiate`. There is no JS runtime, no
Emscripten glue file.

If `wasm-ld` is not found, install Emscripten (`brew install emscripten`) or
LLVM with the wasm target. The script looks in
`$(brew --prefix emscripten)/libexec/llvm/bin/wasm-ld` and in `PATH`.

## Firmware (alpha)

The app is a standalone image on felucca-bsp. It does not need the JieLi SDK
(no loader/package yet), but it does need the JieLi Linux toolchain:

```sh
cd bsp/felucca-bsp
JIELI_TOOLCHAIN=$HOME/.jieli/toolchain tools/get_toolchain.sh   # if not installed
cd ../..
JIELI_TOOLCHAIN=$HOME/.jieli/toolchain ./firmware/build.sh
```

On Linux x86-64 the toolchain runs natively. On macOS (including Apple silicon)
the script runs each tool in `debian:bookworm-slim` under Docker
(`--platform linux/amd64`), as Felucca's build does. Docker Desktop must be
running and the repository must be under a shared folder such as `/Users`.

Output: `firmware/build/reed.bin` (~84 KB) and the ELF/symbols.

**This is not an installable release.** There is no OTA entry, no loader package
and no hardware validation. To flash an alpha you need the stock/UBOOT path and a
way to recover; read [HARDWARE.md](HARDWARE.md) and the Felucca recovery notes
before trying. Installing custom firmware is at your own risk.

## Regenerating the tables

```sh
cd tools/tablegen
cargo run --release > ../../engine/src/wurli_tables.c
```

The generator pins OpenWurli in `Cargo.toml`. The generated file is committed;
normal builds need neither Rust nor network. The output is derivative GPL data.
After regenerating, run `make test`; the golden hash will change if the physics
changed.

## CI

`.github/workflows/ci.yml` builds and tests the host engine on Linux and macOS.
`.github/workflows/pages.yml` builds the wasm and publishes `web/` to GitHub
Pages. The firmware build is not in CI: it needs the proprietary-adjacent JieLi
toolchain and Docker, and the image is not hardware-validated.

## Troubleshooting

- **`reed.wasm` 404 in the emulator**: run the server with `-d web`, not from the
  repo root; open `/try/`.
- **`AudioWorklet` / MIDI fails**: use `localhost` or HTTPS. Web MIDI is
  Chrome/Edge only.
- **No sound after Power**: the page fetches the wasm before creating the worklet;
  check the console/network tab. Headless Chrome may not run an AudioWorklet
  without an audio device.
- **Firmware build: `unknown target triple`**: the assembly compile lost
  `-target pi32v2`; use the provided script, not a hand-written command.
- **Firmware build: Docker cannot see the repo**: move the checkout under
  `/Users` (Docker Desktop file sharing), or run on Linux x86-64.
