#!/usr/bin/env bash
set -euo pipefail

cd /work

mkdir -p build/isodir/boot/grub
cp /usr/share/grub/unicode.pf2 build/isodir/boot/grub/

if [[ "${DEBUG:-0}" == "1" ]]; then
  CFLAGS=(-std=gnu99 -ffreestanding -g -O0 -Wall -Wextra)
else
  CFLAGS=(-std=gnu99 -ffreestanding -O2 -Wall -Wextra)
fi

if [[ "${TESTING:-0}" == "1" ]]; then
  CFLAGS+=(-DTESTING)
fi

LDFLAGS=(-T src/linker.ld -ffreestanding -O2 -nostdlib)

echo "[1/7] Assemble boot.s"
i686-elf-as src/boot.s -o build/boot.o

echo "[2/7] Compile C sources"
objs=(build/boot.o)

for c in src/*.c; do
  o="build/$(basename "${c%.c}.o")"
  echo "    CC $(basename "$c")"
  i686-elf-gcc -c "$c" -o "$o" "${CFLAGS[@]}"
  objs+=("$o")
done

#assemble isr.s
if [ -f src/isr.s ]; then
  echo "    AS isr.s"
  i686-elf-as src/isr.s -o build/isr.o
  objs+=(build/isr.o)
fi

if [[ "${TESTING:-0}" == "1" ]]; then
  echo "[2b/7] Compile kernel test sources"
  for c in tests/kernel/*.c; do
    o="build/$(basename "${c%.c}.o")"
    echo "    CC $(basename "$c")"
    i686-elf-gcc -c "$c" -o "$o" "${CFLAGS[@]}" -I src/
    objs+=("$o")
  done
fi

echo "[3/7] Link kernel -> build/exodoom"
i686-elf-gcc "${LDFLAGS[@]}" -o build/exodoom \
  "${objs[@]}" -lgcc

echo "[4/7] Sanity check multiboot header"
if grub-file --is-x86-multiboot build/exodoom; then
  echo "    multiboot confirmed"
else
  echo "    ERROR: not a valid multiboot kernel"
  exit 1
fi

echo "[5/7] Build ISO staging tree"
mkdir -p build/isodir/boot
cp build/exodoom build/isodir/boot/exodoom
cp src/grub.cfg build/isodir/boot/grub/grub.cfg

echo "[6/7] Copy WAD into ISO"
WAD_PATH=""
for candidate in wads/freedoom2.wad assets/freedoom2.wad; do
    if [[ -f "$candidate" ]]; then
        WAD_PATH="$candidate"
        break
    fi
done
if [[ -z "$WAD_PATH" ]]; then
    echo "ERROR: missing WAD — place freedoom2.wad in wads/ or assets/"
    exit 1
fi
echo "    WAD $WAD_PATH"
cp "$WAD_PATH" build/isodir/boot/freedoom2.wad

echo "[7/7] Create ISO -> build/exodoom.iso"
grub-mkrescue -o build/exodoom.iso build/isodir >/dev/null

echo "Done:"
ls -lh build/exodoom build/exodoom.iso
