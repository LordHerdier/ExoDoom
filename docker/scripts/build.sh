#!/usr/bin/env bash
set -euo pipefail

cd /work

mkdir -p build/isodir/boot/grub
cp /usr/share/grub/unicode.pf2 build/isodir/boot/grub/

CFLAGS=(-std=gnu99 -ffreestanding -O2 -Wall -Wextra)
LDFLAGS=(-T src/linker.ld -ffreestanding -O2 -nostdlib)

echo "[1/6] Assemble boot.s"
i686-elf-as src/boot.s -o build/boot.o

echo "[2/6] Compile C sources"
objs=(build/boot.o)

for c in src/*.c; do
  o="build/$(basename "${c%.c}.o")"
  echo "    CC $(basename "$c")"
  i686-elf-gcc -c "$c" -o "$o" "${CFLAGS[@]}"
  objs+=("$o")
done

echo "[3/6] Link kernel -> build/exodoom"
i686-elf-gcc "${LDFLAGS[@]}" -o build/exodoom \
  "${objs[@]}" -lgcc

echo "[4/6] Sanity check multiboot header"
if grub-file --is-x86-multiboot build/exodoom; then
  echo "    multiboot confirmed"
else
  echo "    ERROR: not a valid multiboot kernel"
  exit 1
fi

echo "[5/6] Build ISO staging tree"
mkdir -p build/isodir/boot
cp build/exodoom build/isodir/boot/exodoom
cp src/grub.cfg build/isodir/boot/grub/grub.cfg

echo "[6/6] Create ISO -> build/exodoom.iso"
grub-mkrescue -o build/exodoom.iso build/isodir >/dev/null

echo "Done:"
ls -lh build/exodoom build/exodoom.iso
