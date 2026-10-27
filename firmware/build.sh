#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Build the Reed FM-1 alpha image.
#
#   JIELI_TOOLCHAIN  default ~/.jieli/toolchain (Linux x86-64)
#   FELUCCA_BSP      default bsp/felucca-bsp (git submodule)
#
# On macOS/arm64 the JieLi tools run through Docker (linux/amd64), as Felucca's
# own build does. The output is firmware/build/reed.bin, not a .fwsc package:
# this alpha has no OTA entry yet. See firmware/README.md.
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
cd "$ROOT"
TC="${JIELI_TOOLCHAIN:-$HOME/.jieli/toolchain}"
BSP="${FELUCCA_BSP:-$ROOT/bsp/felucca-bsp}"
OUT="firmware/build"

[ -x "$TC/pi32v2/bin/clang" ] || { echo "build: no JieLi toolchain in $TC"; exit 1; }
[ -f "$BSP/chip/crt0.S" ] || { echo "build: no felucca-bsp in $BSP (git submodule update --init)"; exit 1; }

case "$(uname -s)-$(uname -m)" in
Linux-x86_64) DOCKER=0 ;;
*) DOCKER=1 ;;
esac
if [ "$DOCKER" = 1 ]; then
    docker info >/dev/null 2>&1 || { echo "build: Docker is not running"; exit 1; }
fi

rm -rf "$OUT"
mkdir -p "$OUT"

run() {
    if [ "$DOCKER" = 1 ]; then
        docker run --rm --platform linux/amd64 \
            -v "$ROOT:/work" -v "$TC:/opt/jieli:ro" -w /work \
            debian:bookworm-slim "$@"
    else
        "$@"
    fi
}

if [ "$DOCKER" = 1 ]; then
    CC="/opt/jieli/pi32v2/bin/clang"
    LD="/opt/jieli/pi32v2/bin/ld"
    OBJCOPY="/opt/jieli/common/bin/objcopy"
    OBJDUMP="/opt/jieli/common/bin/objdump"
    BSP_REL="bsp/felucca-bsp"
else
    CC="$TC/pi32v2/bin/clang"
    LD="$TC/pi32v2/bin/ld"
    OBJCOPY="$TC/common/bin/objcopy"
    OBJDUMP="$TC/common/bin/objdump"
    case "$BSP" in
    "$ROOT"/*) BSP_REL="${BSP#"$ROOT"/}" ;;
    *) echo "build: on Linux, put felucca-bsp under the project or set FELUCCA_BSP inside $ROOT"; exit 1 ;;
    esac
fi

CFLAGS="-target pi32v2 -Os -ffunction-sections -fno-builtin -Wall -Wno-unused-function \
 -Iengine/include -I$BSP_REL/chip -I$BSP_REL/boards/fm1"

run "$CC" -target pi32v2 -c "$BSP_REL/chip/crt0.S" -o "$OUT/crt0.o"
run "$CC" -target pi32v2 -c "$BSP_REL/chip/fm1_vec.S" -o "$OUT/vec.o"
run "$CC" -target pi32v2 -c "$BSP_REL/chip/fm1_isr.S" -o "$OUT/isr.o"
run "$CC" $CFLAGS -c firmware/src/main.c -o "$OUT/main.o"
run "$CC" $CFLAGS -c engine/src/wurli_engine.c -o "$OUT/wurli_engine.o"
run "$CC" $CFLAGS -c engine/src/wurli_tables.c -o "$OUT/wurli_tables.o"

run "$LD" -T firmware/app.ld \
    "$OUT/crt0.o" "$OUT/vec.o" "$OUT/isr.o" \
    "$OUT/main.o" "$OUT/wurli_engine.o" "$OUT/wurli_tables.o" \
    -o "$OUT/reed.elf"

run "$OBJCOPY" -O binary -j .text "$OUT/reed.elf" "$OUT/text.bin"
run "$OBJCOPY" -O binary -j .data "$OUT/reed.elf" "$OUT/data.bin"
run "$OBJCOPY" -O binary -j .ram_text "$OUT/reed.elf" "$OUT/rt.bin"
run "$OBJDUMP" -t "$OUT/reed.elf" > "$OUT/symbols.txt"

python3 - "$OUT" "$OUT/reed.bin" <<'PY'
import re, sys
out, dest = sys.argv[1:]
syms = open(f"{out}/symbols.txt").read()
def sym(name):
    m = re.search(r"^([0-9a-f]+) .*\s" + name + r"$", syms, re.M)
    if not m:
        raise SystemExit(f"missing symbol {name}")
    return int(m.group(1), 16)
load = sym("_data_load")
rt_load = sym("_rt_load")
start = sym("_start")
assert start == 0x02000120, hex(start)
img = bytearray(open(f"{out}/text.bin", "rb").read())
for at, part in sorted([(rt_load, open(f"{out}/rt.bin", "rb").read()),
                        (load, open(f"{out}/data.bin", "rb").read())]):
    if part:
        assert at - 0x02000120 >= len(img), "sections overlap"
        img += b"\xff" * (at - 0x02000120 - len(img)) + part
img += b"\xff" * (-len(img) % 4)
open(dest, "wb").write(img)
print(f"reed.bin: {len(img)} bytes")
PY

echo "built $OUT/reed.bin (alpha; no OTA / package yet)"
