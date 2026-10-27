#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Build the browser emulator's WebAssembly engine.
# Requires Emscripten (emcc). The engine is freestanding C: no libc, no libm.
set -eu
cd "$(dirname "$0")/.."

OUT=web/try/wurli.wasm
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

CFLAGS="-O3 -nostdlib -fno-builtin -DNDEBUG -Iengine/include"
emcc $CFLAGS -c engine/src/wurli_engine.c -o "$TMP/engine.o"
emcc $CFLAGS -c engine/src/wurli_tables.c -o "$TMP/tables.o"
emcc $CFLAGS -c web/wasm_glue.c -o "$TMP/glue.o"

# Emscripten ships wasm-ld; use it directly so the result is a standalone
# module with no JS runtime and no imports.
if command -v brew >/dev/null 2>&1; then
  WASM_LD="$(brew --prefix emscripten 2>/dev/null || true)/libexec/llvm/bin/wasm-ld"
fi
[ -x "${WASM_LD:-}" ] || WASM_LD="$(command -v wasm-ld || true)"
if [ ! -x "${WASM_LD:-}" ]; then
  WASM_LD="$(emcc --print-prog-name=wasm-ld 2>/dev/null || true)"
fi
if [ ! -x "${WASM_LD:-}" ]; then
  WASM_LD="$(dirname "$(command -v emcc)")/../libexec/llvm/bin/wasm-ld"
fi
if [ ! -x "$WASM_LD" ]; then
  echo "wasm-ld not found (install Emscripten or LLVM)" >&2
  exit 1
fi

"$WASM_LD" --no-entry --export-memory \
  --export=wr_init_web --export=wr_note_on_web --export=wr_note_off_web \
  --export=wr_sustain_web --export=wr_param_web --export=wr_all_off_web \
  --export=wr_panic_web --export=wr_voices_web --export=wr_out_ptr \
  --export=wr_render_web \
  "$TMP/engine.o" "$TMP/tables.o" "$TMP/glue.o" -o "$OUT"

echo "built $OUT ($(wc -c < "$OUT" | tr -d ' ') bytes)"
