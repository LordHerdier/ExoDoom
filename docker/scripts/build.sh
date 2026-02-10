#!/usr/bin/env bash
set -euo pipefail

# Expect repo mounted at /work
cd /work

mkdir -p build/isodir/boot/grub

echo "[1/6] Assemble boot.s"
i686-elf-as src/boot.s -o build/boot.o

echo "[2/6] Compile kernel.c"
i686-elf-gcc -c src/kernel.c -o build/kernel.o \
  -std=gnu99 -ffreestanding -O2 -Wall -Wextra

echo "[3/6] Link kernel -> build/exodoom"
i686-elf-gcc -T src/linker.ld -o build/exodoom \
  -ffreestanding -O2 -nostdlib \
  build/boot.o build/kernel.o -lgcc

echo "[4/6] Sanity check multiboot header"
if grub-file --is-x86-multiboot build/exodoom; then
  echo "    multiboot confirmed"
else
  echo "    ERROR: not a valid multiboot kernel"
  exit 1
fi

echo "[5/6] Build ISO staging tree"
cp build/exodoom build/isodir/boot/exodoom
cp src/grub.cfg build/isodir/boot/grub/grub.cfg

echo "[6/6] Create ISO -> build/exodoom.iso"
grub-mkrescue -o build/exodoom.iso build/isodir >/dev/null

echo "Done:"
ls -lh build/exodoom build/exodoom.iso